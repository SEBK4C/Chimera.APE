// §7 — lazy supervision of organ child processes on loopback ports.
#pragma once

#include <string>
#include <vector>

namespace chimera {

// One supervised child. Lifecycle: spawn() → wait_healthy() → use → stop().
class Child {
 public:
  Child() = default;
  ~Child();  // stop(5s) if still running
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  // Spawns argv[0] with args; stdout+stderr both go to log_path (appended).
  // Returns false if exec fails outright.
  bool spawn(const std::vector<std::string>& argv, const std::string& log_path);

  // Polls GET health_path on port with exponential backoff until 200 or
  // timeout. On failure (or child death) returns false; error() then carries
  // the log tail.
  bool wait_healthy(int port, const std::string& health_path, int timeout_ms = 120000);

  bool running() const;
  // TERM, then KILL after grace_ms (§7 signals).
  void stop(int grace_ms = 5000);

  int pid() const { return pid_; }
  const std::string& error() const { return error_; }
  // Last ~30 lines of the child's log, for crash messages.
  std::string log_tail() const;

 private:
  int pid_ = -1;
  std::string log_path_;
  std::string error_;
};

// Ask the kernel for a free loopback port (bind(0) → getsockname → close).
// Racy in principle; children re-bind immediately and collisions surface as
// failed health checks, which abort cleanly.
int pick_port();

}  // namespace chimera
