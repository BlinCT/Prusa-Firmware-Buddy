/**
 * \file
 *
 * Support for uploading gcode.
 */
#pragma once

#include "req_parser.h"
#include "status_page.h"
#include "upload_state.h"

#include <unique_file_ptr.hpp>
#include <http/types.h>

#include <memory>
#include <optional>

struct Uploader;

namespace nhttp::printer {

/**
 * \brief A handler to accept incoming gcodes.
 *
 * This will take a POST or PUT request with valid form containing gcode and store it
 * onto the USB drive.
 */
class GcodeUpload final : private UploadHooks {
public:
    typedef bool UploadedNotify(char *name, bool start_print);

    struct PutParams {
        std::array<char, FILE_PATH_BUFFER_LEN> filepath;
        bool print_after_upload;
    };
    using UploadParams = std::variant<PutParams, UploadState>;

private:
    UploadParams upload;

    UploadedNotify *uploaded_notify;
    size_t size_rest;
    bool json_errors;
    // Shall we remove/delete the temp file?
    // Turns to false in move constructor/assignment in the old instance (it
    // relinquishes ownership)
    bool cleanup_temp_file;
    unique_file_ptr tmp_upload_file;
    // A way how to reconstruct the name of the temporary file.
    size_t file_idx;

    bool filename_checked;

    virtual Result data(std::string_view data) override;
    virtual Result finish(const char *final_filename, bool start_print) override;
    virtual Result check_filename(const char *filename) const override;

    handler::Step step(std::string_view input, PutParams &putParams);
    handler::Step step(std::string_view input, UploadState &uploader);

    GcodeUpload(UploadParams &&uploader, bool json_errors, size_t length, size_t upload_idx, unique_file_ptr file, UploadedNotify *uploaded);

public:
    bool want_read() const { return size_rest > 0; }
    bool want_write() const { return false; }
    handler::Step step(std::string_view input, bool terminated_by_client, uint8_t *output, size_t output_size);
    using UploadResult = std::variant<handler::StatusPage, GcodeUpload>;
    static UploadResult start(const handler::RequestParser &parser, UploadedNotify *uploaded, bool json_errors, UploadParams &&uploadParams);
    GcodeUpload(const GcodeUpload &other) = delete;
    GcodeUpload(GcodeUpload &&other);
    GcodeUpload &operator=(const GcodeUpload &other) = delete;
    GcodeUpload &operator=(GcodeUpload &&other);
    ~GcodeUpload();
};

}
