#include "http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

namespace chimera {

namespace {

bool send_all(int fd, const char* data, size_t n, int timeout_ms) {
  size_t off = 0;
  while (off < n) {
    struct pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return false;
    ssize_t w = ::send(fd, data + off, n - off, 0);
    if (w <= 0) return false;
    off += static_cast<size_t>(w);
  }
  return true;
}

// Reads until EOF or (when content-length known) the full body is in.
std::optional<std::string> read_response(int fd, int timeout_ms) {
  std::string buf;
  char tmp[16384];
  long long want = -1;  // total bytes expected (headers + body), -1 unknown
  size_t header_end = std::string::npos;
  while (true) {
    if (want >= 0 && static_cast<long long>(buf.size()) >= want) break;
    struct pollfd p{fd, POLLIN, 0};
    int pr = poll(&p, 1, timeout_ms);
    if (pr <= 0) return std::nullopt;
    ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
    if (r < 0) return std::nullopt;
    if (r == 0) break;  // EOF
    buf.append(tmp, static_cast<size_t>(r));
    if (header_end == std::string::npos) {
      header_end = buf.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        // naive content-length scan (lowercased copy of header block)
        std::string head = buf.substr(0, header_end);
        for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto cl = head.find("content-length:");
        if (cl != std::string::npos) {
          want = std::atoll(head.c_str() + cl + 15) +
                 static_cast<long long>(header_end) + 4;
        }
      }
    }
  }
  return buf;
}

// Dechunk a Transfer-Encoding: chunked body.
std::string dechunk(const std::string& body) {
  std::string out;
  size_t i = 0;
  while (i < body.size()) {
    size_t nl = body.find("\r\n", i);
    if (nl == std::string::npos) break;
    long len = std::strtol(body.c_str() + i, nullptr, 16);
    if (len <= 0) break;
    i = nl + 2;
    if (i + static_cast<size_t>(len) > body.size()) break;
    out.append(body, i, static_cast<size_t>(len));
    i += static_cast<size_t>(len) + 2;  // skip body + CRLF
  }
  return out;
}

}  // namespace

std::optional<HttpResponse> http_request(const std::string& method, int port,
                                         const std::string& path,
                                         const std::string& body,
                                         const std::string& content_type,
                                         int timeout_ms, const std::string& host) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return std::nullopt;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    return std::nullopt;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  std::ostringstream req;
  req << method << " " << path << " HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Connection: close\r\n";
  if (!body.empty() || method == "POST") {
    if (!content_type.empty()) req << "Content-Type: " << content_type << "\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
  }
  req << "\r\n" << body;
  std::string r = req.str();
  if (!send_all(fd, r.data(), r.size(), timeout_ms)) return std::nullopt;

  auto raw = read_response(fd, timeout_ms);
  if (!raw) return std::nullopt;

  HttpResponse resp;
  size_t sp = raw->find(' ');
  if (sp == std::string::npos) return std::nullopt;
  resp.status = std::atoi(raw->c_str() + sp + 1);
  size_t he = raw->find("\r\n\r\n");
  if (he == std::string::npos) return std::nullopt;
  std::string head = raw->substr(0, he);
  for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  resp.body = raw->substr(he + 4);
  if (head.find("transfer-encoding: chunked") != std::string::npos)
    resp.body = dechunk(resp.body);
  return resp;
}

}  // namespace chimera
