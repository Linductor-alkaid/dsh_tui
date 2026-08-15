#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "dsh_runner.hpp"

namespace dsh_tui {

struct HostOutput {
  std::deque<std::string> lines;
};

class RunOutput {
 public:
  bool running = false;
  bool completed = false;
  int exit_code = -1;
  bool signaled = false;
  std::string command;
  std::string target_summary;
  std::string spawn_error;
  std::vector<std::string> known_hosts;
  std::map<std::string, HostOutput> hosts;
  std::vector<std::string> stderr_lines;
  std::chrono::steady_clock::time_point started_at;

  void Start(const DshInvocation& invocation, std::vector<std::string> hosts);
  void Apply(const RunnerEvent& event);
  void MarkSpawnError(std::string message);
  void Clear();
  size_t TotalLines() const;
};

}  // namespace dsh_tui
