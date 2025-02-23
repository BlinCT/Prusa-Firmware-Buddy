from http import accept_header, connection_header, read_boundary, read_header_value, request, print_after_upload_header

if __name__ == "__main__":
    want_headers = {
        'X-Api-Key': read_header_value('XApiKey'),
        'Content-Length': read_header_value('ContentLength'),
        'If-None-Match': read_header_value('IfNoneMatch'),
        'Print-After-Upload': print_after_upload_header(),
        'Content-Type': read_boundary(),
        'Connection': connection_header(),
        'Accept': accept_header(),
    }
    http, final = request(want_headers)
    compiled = http.compile("nhttp::parser::request")
    with open("http_req_automaton.h", "w") as header:
        header.write(compiled.cpp_header())
    with open("http_req_automaton.cpp", "w") as file:
        file.write(compiled.cpp_file())
