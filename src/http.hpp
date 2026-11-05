#ifndef DESK_HTTP_HPP
#define DESK_HTTP_HPP
#include <curl/curl.h>
#include <memory>
#include <new>
#include <string>

struct CurlDeleter {
    void operator()(CURL *handle) const noexcept { curl_easy_cleanup(handle); }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

class HttpHeaders {
    curl_slist *head_ = nullptr;
public:
    HttpHeaders() = default;
    HttpHeaders(const HttpHeaders &) = delete;
    HttpHeaders &operator=(const HttpHeaders &) = delete;
    ~HttpHeaders() { curl_slist_free_all(head_); }
    void append(const char *value) {
        auto *next = curl_slist_append(head_, value);
        if (!next) throw std::bad_alloc{};
        head_ = next;
    }
    void append(const std::string &value) { append(value.c_str()); }
    curl_slist *get() const noexcept { return head_; }
};

// Keep connection caches, but never retain request headers, bodies or callbacks.
class CurlRequest {
    CURL *handle_;
public:
    explicit CurlRequest(CURL *handle) noexcept : handle_(handle) { curl_easy_reset(handle_); }
    CurlRequest(const CurlRequest &) = delete;
    CurlRequest &operator=(const CurlRequest &) = delete;
    ~CurlRequest() { curl_easy_reset(handle_); }
};
#endif
