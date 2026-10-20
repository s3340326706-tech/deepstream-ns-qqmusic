// http.hpp —— libcurl 薄封装。PC 用系统 curl，Switch 用 switch-curl。
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace se {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool has_body = false;

    long timeout_sec = 8;
    size_t max_body = 4u * 1024u * 1024u;
    bool insecure = false;      // 只在调试时开；会暴露 Cookie
    bool follow_redirect = true;
    std::string cainfo;         // Switch: "romfs:/cacert.pem"
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string content_type;
    std::string error;          // 非空表示传输层失败

    bool ok() const { return error.empty(); }
};

HttpResponse http_perform(const HttpRequest& req);

}  // namespace se
