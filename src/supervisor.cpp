#include "supervisor.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include "http.h"

namespace chimera {

Child::~Child() {
  if (running()) stop();
}

bool Child::spawn(const std::vector<std::string>& argv, const std::string& log_path) {
  log_path_ = log_path;
  int logfd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (logfd < 0) {
    error_ = "cannot open log " + log_path;
    return false;
  }

  std::vector<char*> cargv;
  for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(logfd);
    error_ = "fork failed";
    return false;
  }
  if (pid == 0) {
    ::dup2(logfd, 1);
    ::dup2(logfd, 2);
    ::close(logfd);
    // Children get their own process group so a ^C on the orchestrator
    // doesn't reach them before the drain logic does (§7).
    ::setpgid(0, 0);
    ::execvp(cargv[0], cargv.data());
    std::fprintf(stderr, "exec %s: %s\n", cargv[0], std::strerror(errno));
    _exit(127);
  }
  ::close(logfd);
  pid_ = pid;
  return true;
}

bool Child::running() const {
  if (pid_ <= 0) return false;
  // Reap if exited: kill(pid, 0) alone reports zombies as alive.
  int status = 0;
  pid_t r = ::waitpid(pid_, &status, WNOHANG);
  if (r == pid_) {
    const_cast<Child*>(this)->pid_ = -1;
    return false;
  }
  return r == 0;
}

bool Child::wait_healthy(int port, const std::string& health_path, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int backoff_ms = 50;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == pid_) {
      error_ = "child exited during startup; log tail:\n" + log_tail();
      pid_ = -1;
      return false;
    }
    auto resp = http_get(port, health_path, /*timeout_ms=*/2000);
    if (resp && resp->status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    backoff_ms = std::min(backoff_ms * 2, 2000);
  }
  error_ = "health check timed out on port " + std::to_string(port) +
           "; log tail:\n" + log_tail();
  return false;
}

void Child::stop(int grace_ms) {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::kill(pid_, SIGKILL);
  ::waitpid(pid_, nullptr, 0);
  pid_ = -1;
}

std::string Child::log_tail() const {
  std::ifstream in(log_path_, std::ios::binary | std::ios::ate);
  if (!in) return "(no log)";
  std::streamoff size = in.tellg();
  std::streamoff want = 4096;
  in.seekg(size > want ? size - want : std::streamoff(0));
  std::string tail((std::istreambuf_iterator<char>(in)), {});
  return tail;
}

int pick_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof addr;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

}  // namespace chimera
