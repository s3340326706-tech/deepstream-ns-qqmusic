#include "se/http.hpp"

#include <curl/curl.h>

#include <mutex>

namespace se {
namespace {

std::once_flag g_curl_once;

void ensure_global() {
    std::call_once(g_curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Sink {
    std::string data;
    size_t cap = 0;
    bool overflow = false;
};

size_t on_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    Sink* s = static_cast<Sink*>(userdata);
    size_t n = size * nmemb;
    if (s->data.size() + n > s->cap) {
        s->overflow = true;
        return 0;  // 返回 0 让 curl 中止传输
    }
    s->data.append(ptr, n);
    return n;
}

}  // namespace

HttpResponse http_perform(const HttpRequest& req) {
    ensure_global();

    HttpResponse out;
    CURL* c = curl_easy_init();
    if (!c) {
        out.error = "curl_easy_init failed";
        return out;
    }

    Sink sink;
    sink.cap = req.max_body;

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers)
        hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());

    curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeout_sec);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, req.follow_redirect ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, req.insecure ? 0L : 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, req.insecure ? 0L : 2L);
    if (!req.cainfo.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, req.cainfo.c_str());

    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

    if (req.method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        if (req.has_body) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    } else if (req.method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (req.has_body) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    }

    CURLcode rc = curl_easy_perform(c);

    long status = 0;
    char* ctype = nullptr;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);

    out.status = status;
    out.content_type = ctype ? ctype : "";
    out.body.swap(sink.data);

    if (sink.overflow) {
        out.error = "response exceeds size cap";
    } else if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return out;
}

}  // namespace se
