#include "connect.hpp"
#include "httpc.hpp"
#include "os_porting.hpp"
#include "tls/tls.hpp"
#include "render.hpp"
#include "socket.hpp"
#include "crc32.h"

#include <cmsis_os.h>
#include <log.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <debug.h>
#include <cstring>
#include <optional>
#include <variant>

using http::ContentType;
using http::Status;
using json::JsonRenderer;
using json::JsonResult;
using std::decay_t;
using std::get;
using std::get_if;
using std::holds_alternative;
using std::is_same_v;
using std::min;
using std::monostate;
using std::nullopt;
using std::optional;
using std::string_view;
using std::variant;
using std::visit;

LOG_COMPONENT_DEF(connect, LOG_SEVERITY_DEBUG);

namespace con {

namespace {

    std::atomic<OnlineStatus> last_known_status = OnlineStatus::Unknown;

    OnlineStatus err_to_status(const Error error) {
        switch (error) {
        case Error::Connect:
            return OnlineStatus::NoConnection;
        case Error::Dns:
            return OnlineStatus::NoDNS;
        case Error::InternalError:
        case Error::ResponseTooLong:
        case Error::SetSockOpt:
            return OnlineStatus::InternalError;
        case Error::Network:
        case Error::Timeout:
            return OnlineStatus::NetworkError;
        case Error::Parse:
            return OnlineStatus::Confused;
        case Error::Tls:
            return OnlineStatus::Tls;
        default:
            return OnlineStatus::Unknown;
        }
    }

    uint32_t cfg_crc(configuration_t &config) {
        uint32_t crc = 0;
        crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(config.host), strlen(config.host));
        crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(config.token), strlen(config.token));
        crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&config.port), sizeof config.port);
        crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&config.tls), sizeof config.tls);
        crc = crc32_calc_ex(crc, reinterpret_cast<const uint8_t *>(&config.enabled), sizeof config.enabled);
        return crc;
    }

    enum class Progress {
        Rendering,
        Done,
    };

    class BasicRequest final : public Request {
    private:
        HeaderOut hdrs[3];
        Progress progress = Progress::Rendering;
        Renderer renderer;
        using RenderResult = variant<size_t, Error>;
        const char *target_url;
        static const char *url(const Sleep &) {
            // Sleep already handled at upper level.
            assert(0);
            return "";
        }
        static const char *url(const SendTelemetry &) {
            return "/p/telemetry";
        }
        static const char *url(const Event &) {
            return "/p/events";
        }

    public:
        BasicRequest(core_interface &core, const printer_info_t &info, const configuration_t &config, const Action &action)
            : hdrs {
                { "Fingerprint", info.fingerprint, FINGERPRINT_HDR_SIZE },
                { "Token", config.token, nullopt },
                { nullptr, nullptr, nullopt }
            }
            , renderer(RenderState {
                  info,
                  core.get_data(),
                  action,
              })
            , target_url(visit([](const auto &action) { return url(action); }, action)) {}
        virtual const char *url() const override {
            return target_url;
        }
        virtual ContentType content_type() const override {
            return ContentType::ApplicationJson;
        }
        virtual Method method() const override {
            return Method::Post;
        }
        virtual const HeaderOut *extra_headers() const override {
            return hdrs;
        }
        virtual RenderResult write_body_chunk(char *data, size_t size) override {
            switch (progress) {
            case Progress::Done:
                return 0U;
            case Progress::Rendering: {
                const auto [result, written_json] = renderer.render(reinterpret_cast<uint8_t *>(data), size);
                switch (result) {
                case JsonResult::Abort:
                    assert(0);
                    progress = Progress::Done;
                    return Error::InternalError;
                case JsonResult::BufferTooSmall:
                    // Can't fit even our largest buffer :-(.
                    //
                    // (the http client flushes the headers before trying to
                    // render the body, so we have a full buffer each time).
                    progress = Progress::Done;
                    return Error::InternalError;
                case JsonResult::Incomplete:
                    return written_json;
                case JsonResult::Complete:
                    progress = Progress::Done;
                    return written_json;
                }
            }
            }
            assert(0);
            return Error::InternalError;
        }
    };

    // TODO: We probably want to be able to both have a smaller buffer and
    // handle larger responses. We need some kind of parse-as-it-comes approach
    // for that.
    const constexpr size_t MAX_RESP_SIZE = 256;

    // Wait one second between config retries and similar.
    const constexpr uint32_t IDLE_WAIT = 1000;

    using Cache = variant<monostate, tls, socket_con, Error>;
}

class connect::CachedFactory final : public ConnectionFactory {
private:
    const char *hostname = nullptr;
    Cache cache;

public:
    virtual variant<Connection *, Error> connection() override {
        // Note: The monostate state should not be here at this moment, it's only after invalidate and similar.
        if (Connection *c = get_if<tls>(&cache); c != nullptr) {
            return c;
        } else if (Connection *c = get_if<socket_con>(&cache); c != nullptr) {
            return c;
        } else {
            Error error = get<Error>(cache);
            // Error is just one-off. Next time we'll try connecting again.
            cache = monostate();
            return error;
        }
    }
    virtual const char *host() override {
        return hostname;
    }
    virtual void invalidate() override {
        cache = monostate();
    }
    template <class C>
    void refresh(const char *hostname, C &&callback) {
        this->hostname = hostname;
        if (holds_alternative<monostate>(cache)) {
            callback(cache);
        }
        assert(!holds_alternative<monostate>(cache));
    }
    uint32_t cfg_fingerprint = 0;
};

connect::ServerResp connect::handle_server_resp(Response resp) {
    if (resp.content_length() > MAX_RESP_SIZE) {
        return Error::ResponseTooLong;
    }

    // Note: missing command ID is already checked at upper level.
    CommandId command_id = resp.command_id.value();
    // XXX Use allocated string? Figure out a way to consume it in parts?
    uint8_t recv_buffer[MAX_RESP_SIZE];
    size_t pos = 0;

    while (resp.content_length() > 0) {
        const auto result = resp.read_body(recv_buffer + pos, resp.content_length());
        if (holds_alternative<size_t>(result)) {
            pos += get<size_t>(result);
        } else {
            return get<Error>(result);
        }
    }

    const string_view body(reinterpret_cast<const char *>(recv_buffer), pos);

    // Note: Anything of these can result in an "Error"-style command (Unknown,
    // Broken...). Nevertheless, we return a Command, which'll consider the
    // whole request-response pair a successful one. That's OK, because on the
    // lower-level it is - we consumed all the data and are allowed to reuse
    // the connection and all that.
    switch (resp.content_type) {
    case ContentType::TextGcode:
        return Command::gcode_command(command_id, body);
    case ContentType::ApplicationJson:
        return Command::parse_json_command(command_id, body);
    default:;
        // If it's unknown content type, then it's unknown command because we
        // have no idea what to do about it / how to even parse it.
        return Command {
            command_id,
            UnknownCommand {},
        };
    }
}

optional<OnlineStatus> connect::communicate(CachedFactory &conn_factory) {
    configuration_t config = core.get_connect_config();

    if (!config.enabled) {
        planner.reset();
        osDelay(IDLE_WAIT);
        return OnlineStatus::Off;
    }

    if (config.host[0] == '\0' || config.token[0] == '\0') {
        planner.reset();
        osDelay(IDLE_WAIT);
        return OnlineStatus::NoConfig;
    }

    auto action = planner.next_action();

    // Handle sleeping first. That one doesn't need the connection.
    if (auto *s = get_if<Sleep>(&action)) {
        osDelay(s->milliseconds);
        // Don't change the status now, we just slept
        return nullopt;
    }

    // Make sure to reconnect if the configuration changes (we ignore the
    // 1:2^32 possibility of collision).
    const uint32_t cfg_fingerprint = cfg_crc(config);
    if (cfg_fingerprint != conn_factory.cfg_fingerprint) {
        conn_factory.cfg_fingerprint = cfg_fingerprint;
        conn_factory.invalidate();
    }

    // Let it reconnect if it needs it.
    conn_factory.refresh(config.host, [&](Cache &cache) {
        Connection *connection;
        if (config.tls) {
            cache.emplace<tls>();
            connection = &std::get<tls>(cache);
        } else {
            cache.emplace<socket_con>();
            connection = &std::get<socket_con>(cache);
        }

        if (const auto result = connection->connection(config.host, config.port); result.has_value()) {
            cache = *result;
        }
    });

    HttpClient http(conn_factory);

    BasicRequest request(core, printer_info, config, action);
    const auto result = http.send(request);

    if (holds_alternative<Error>(result)) {
        planner.action_done(ActionResult::Failed);
        conn_factory.invalidate();
        return err_to_status(get<Error>(result));
    }

    Response resp = get<Response>(result);
    if (!resp.can_keep_alive) {
        conn_factory.invalidate();
    }
    switch (resp.status) {
    // The server has nothing to tell us
    case Status::NoContent:
        planner.action_done(ActionResult::Ok);
        return OnlineStatus::Ok;
    case Status::Ok: {
        if (resp.command_id.has_value()) {
            const auto sub_resp = handle_server_resp(resp);
            return visit([&](auto &&arg) -> optional<OnlineStatus> {
                // Trick out of std::visit documentation. Switch by the type of arg.
                using T = decay_t<decltype(arg)>;

                if constexpr (is_same_v<T, monostate>) {
                    planner.action_done(ActionResult::Ok);
                    return OnlineStatus::Ok;
                } else if constexpr (is_same_v<T, Command>) {
                    planner.action_done(ActionResult::Ok);
                    planner.command(arg);
                    return OnlineStatus::Ok;
                } else if constexpr (is_same_v<T, Error>) {
                    planner.action_done(ActionResult::Failed);
                    planner.command(Command {
                        resp.command_id.value(),
                        BrokenCommand {},
                    });
                    conn_factory.invalidate();
                    return err_to_status(arg);
                }
            },
                sub_resp);
        } else {
            // We have received a command without command ID
            // There's no better action for us than just throw it away.
            planner.action_done(ActionResult::Refused);
            conn_factory.invalidate();
            return OnlineStatus::Confused;
        }
    }
    case Status::RequestTimeout:
    case Status::TooManyRequests:
    case Status::ServiceTemporarilyUnavailable:
    case Status::GatewayTimeout:
        conn_factory.invalidate();
        // These errors are likely temporary and will go away eventually.
        planner.action_done(ActionResult::Failed);
        return OnlineStatus::ServerError;
    default:
        conn_factory.invalidate();
        // We don't know that exactly the server answer means, but we guess
        // that it will persist, so we consider it refused and throw the
        // request away.
        planner.action_done(ActionResult::Refused);
        // Switch just to provide proper error message
        switch (resp.status) {
        case Status::BadRequest:
            return OnlineStatus::InternalError;
        case Status::Unauthorized:
        case Status::Forbidden:
            return OnlineStatus::Auth;
        default:
            return OnlineStatus::ServerError;
        }
    }
}

void connect::run() {
    CONNECT_DEBUG("%s", "Connect client starts\n");
    // waits for file-system and network interface to be ready
    //FIXME! some mechanisms to know that file-system and network are ready.
    osDelay(IDLE_WAIT);

    CachedFactory conn_factory;

    while (true) {
        const auto new_status = communicate(conn_factory);
        if (new_status.has_value()) {
            last_known_status = *new_status;
        }
    }
}

connect::connect()
    : printer_info(core.get_printer_info()) {}

OnlineStatus last_status() {
    return last_known_status;
}

}
