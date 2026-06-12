// Minimal HTTP/1.1 client for loopback organ traffic (§7). No TLS, no
// redirects, no chunked *requests*; handles chunked and content-length
// responses. Connection: close per request — organ calls are batched and
// coarse, so connection reuse buys nothing worth the state.
#pragma once

#include <optional>
#include <string>

namespace chimera {

struct HttpResponse {
  int status = 0;
  std::string body;
};

// Returns nullopt on transport errors (connect/read failure). HTTP-level
// errors come back as status codes.
std::optional<HttpResponse> http_request(
    const std::string& method, int port, const std::string& path,
    const std::string& body = "",
    const std::string& content_type = "application/json",
    int timeout_ms = 120000, const std::string& host = "127.0.0.1");

inline std::optional<HttpResponse> http_get(int port, const std::string& path,
                                            int timeout_ms = 5000) {
  return http_request("GET", port, path, "", "", timeout_ms);
}

inline std::optional<HttpResponse> http_post(int port, const std::string& path,
                                             const std::string& body,
                                             const std::string& content_type = "application/json",
                                             int timeout_ms = 120000) {
  return http_request("POST", port, path, body, content_type, timeout_ms);
}

}  // namespace chimera
