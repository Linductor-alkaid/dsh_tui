#pragma once

#include <sys/types.h>

#include <memory>
#include <string>

#include "dsh_config.hpp"
#include "executor/comm.hpp"
#include "executor/executor.hpp"

namespace ftxui {
class App;
}

namespace dsh_tui {

struct RunnerEvent {
  enum class Type { StdoutLine, StderrLine, ProcessExit };

  Type type = Type::StdoutLine;
  std::string text;
  int exit_code = -1;
  bool signaled = false;
};

struct DshProcess {
  pid_t pid = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
};

/// Fork/exec the configured dsh binary with captured stdout/stderr pipes.
DshProcess SpawnDsh(const DshInvocation& invocation, std::string& error);

/// Kill (if needed), reap, and close the pipe ends owned by a process handle.
void TerminateDshProcess(DshProcess& process);

/// Blocking-I/O worker owned by executor. Reads both child pipes with poll(),
/// forwards line events into an MPSC channel, and reports the child's exit.
class DshProcessWorker final : public executor::IBlockingIoWorker {
 public:
  DshProcessWorker(DshProcess process,
                   executor::comm::MpscChannel<RunnerEvent>* events,
                   ftxui::App* screen);

  ~DshProcessWorker() override;
  void run(std::stop_token stop_token) override;
  void wakeup() noexcept override {}

 private:
  DshProcess process_;
  executor::comm::MpscChannel<RunnerEvent>* events_ = nullptr;
  ftxui::App* screen_ = nullptr;
};

}  // namespace dsh_tui
