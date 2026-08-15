#include "output_state.hpp"

#include <algorithm>

namespace dsh_tui {

namespace {

constexpr size_t kMaxHostLines = 800;
constexpr size_t kMaxStderrLines = 800;

}  // namespace

void RunOutput::Clear() {
  running = false;
  completed = false;
  exit_code = -1;
  signaled = false;
  command.clear();
  target_summary.clear();
  spawn_error.clear();
  known_hosts.clear();
  hosts.clear();
  stderr_lines.clear();
}

void RunOutput::Start(const DshInvocation& invocation, std::vector<std::string> host_list) {
  Clear();
  running = true;
  completed = false;
  command = invocation.command;
  target_summary = TargetSummary(invocation);
  known_hosts = std::move(host_list);
  started_at = std::chrono::steady_clock::now();
  if (!known_hosts.empty()) {
    for (const auto& host : known_hosts) hosts[host] = HostOutput{};
  }
}

void RunOutput::MarkSpawnError(std::string message) {
  running = false;
  completed = true;
  exit_code = 127;
  spawn_error = std::move(message);
}

void RunOutput::Apply(const RunnerEvent& event) {
  switch (event.type) {
    case RunnerEvent::Type::StdoutLine: {
      std::string line = event.text;
      std::string host = "(dsh)";
      size_t separator = line.find(": ");
      if (separator != std::string::npos && separator > 0) {
        host = line.substr(0, separator);
        line = line.substr(separator + 2);
      }
      auto& output = hosts[host];
      output.lines.push_back(std::move(line));
      while (output.lines.size() > kMaxHostLines) output.lines.pop_front();
      break;
    }
    case RunnerEvent::Type::StderrLine:
      stderr_lines.push_back(event.text);
      while (stderr_lines.size() > kMaxStderrLines) stderr_lines.erase(stderr_lines.begin());
      break;
    case RunnerEvent::Type::ProcessExit:
      running = false;
      completed = true;
      exit_code = event.exit_code;
      signaled = event.signaled;
      break;
  }
}

size_t RunOutput::TotalLines() const {
  size_t total = stderr_lines.size();
  for (const auto& [host, output] : hosts) total += output.lines.size();
  return total;
}

}  // namespace dsh_tui
