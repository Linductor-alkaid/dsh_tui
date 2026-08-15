#include "dsh_runner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ftxui/component/app.hpp"
#include "ftxui/component/event.hpp"

namespace dsh_tui {

namespace {

void SetNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void CloseFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void PostWakeup(ftxui::App* screen) {
  if (screen != nullptr) screen->PostEvent(ftxui::Event::Custom);
}

struct LineReader {
  std::string buffer;
  bool eof = false;

  // Read whatever is available on a non-blocking fd and return complete
  // lines. On EOF the final unterminated chunk is returned as a line too.
  void Read(int fd, std::vector<std::string>& lines) {
    if (eof || fd < 0) return;
    char chunk[8192];
    for (;;) {
      ssize_t count = ::read(fd, chunk, sizeof(chunk));
      if (count > 0) {
        buffer.append(chunk, static_cast<size_t>(count));
        if (buffer.size() > (16U << 20U)) buffer.resize(16U << 20U);
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (count < 0 && errno == EINTR) continue;
      eof = true;
      break;
    }

    size_t newline = std::string::npos;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      lines.push_back(buffer.substr(0, newline));
      buffer.erase(0, newline + 1);
    }
    if (eof && !buffer.empty()) {
      lines.push_back(std::move(buffer));
      buffer.clear();
    }
  }

  // Poll until EOF or the timeout elapses. Keeps consuming complete lines.
  void DrainFor(int fd, std::vector<std::string>& lines,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!eof && fd >= 0 && std::chrono::steady_clock::now() < deadline) {
      Read(fd, lines);
      if (eof) break;
      pollfd descriptor{fd, static_cast<short>(POLLIN | POLLHUP), 0};
      int ready = ::poll(&descriptor, 1, 40);
      if (ready <= 0) break;
    }
    Read(fd, lines);
  }
};

std::optional<int> ReapChild(pid_t pid) {
  int status = 0;
  for (;;) {
    pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == 0) return std::nullopt;
    if (result < 0 && errno == EINTR) continue;
    if (result < 0) return 128 + SIGKILL;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
  }
}

void EmitLines(std::vector<std::string> lines, RunnerEvent::Type type,
               executor::comm::MpscChannel<RunnerEvent>* events,
               ftxui::App* screen) {
  for (auto& line : lines) {
    RunnerEvent event;
    event.type = type;
    event.text = std::move(line);
    auto result = events->send_for(std::move(event), std::chrono::milliseconds(500));
    PostWakeup(screen);
    if (!result.ok) return;
  }
}

}  // namespace

DshProcess SpawnDsh(const DshInvocation& invocation, std::string& error) {
  std::vector<std::string> argv = BuildDshArgv(invocation);
  if (argv.size() < 3) {
    error = "empty dsh command line";
    return {};
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
    error = "pipe() failed: " + std::string(std::strerror(errno));
    CloseFd(stdout_pipe[0]);
    CloseFd(stdout_pipe[1]);
    CloseFd(stderr_pipe[0]);
    CloseFd(stderr_pipe[1]);
    return {};
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    error = "fork() failed: " + std::string(std::strerror(errno));
    CloseFd(stdout_pipe[0]);
    CloseFd(stdout_pipe[1]);
    CloseFd(stderr_pipe[0]);
    CloseFd(stderr_pipe[1]);
    return {};
  }

  if (pid == 0) {
    (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
    (void)::dup2(stderr_pipe[1], STDERR_FILENO);
    CloseFd(stdout_pipe[0]);
    CloseFd(stdout_pipe[1]);
    CloseFd(stderr_pipe[0]);
    CloseFd(stderr_pipe[1]);

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (auto& arg : argv) raw_argv.push_back(arg.data());
    raw_argv.push_back(nullptr);
    ::execvp(raw_argv[0], raw_argv.data());
    std::fprintf(stderr, "dsh_tui: execvp(%s) failed: %s\n", raw_argv[0],
                 std::strerror(errno));
    _exit(127);
  }

  CloseFd(stdout_pipe[1]);
  CloseFd(stderr_pipe[1]);
  SetNonBlocking(stdout_pipe[0]);
  SetNonBlocking(stderr_pipe[0]);

  DshProcess process;
  process.pid = pid;
  process.stdout_fd = stdout_pipe[0];
  process.stderr_fd = stderr_pipe[0];
  return process;
}

DshProcessWorker::DshProcessWorker(DshProcess process,
                                   executor::comm::MpscChannel<RunnerEvent>* events,
                                   ftxui::App* screen)
    : process_(process), events_(events), screen_(screen) {}

DshProcessWorker::~DshProcessWorker() {
  TerminateDshProcess(process_);
}

void TerminateDshProcess(DshProcess& process) {
  if (process.pid > 0) {
    (void)::kill(process.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
      auto status = ReapChild(process.pid);
      if (status.has_value()) {
        process.pid = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (process.pid > 0) {
      (void)::kill(process.pid, SIGKILL);
      for (int i = 0; i < 20; ++i) {
        auto status = ReapChild(process.pid);
        if (status.has_value()) {
          process.pid = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
  CloseFd(process.stdout_fd);
  CloseFd(process.stderr_fd);
}

void DshProcessWorker::run(std::stop_token stop_token) {
  LineReader stdout_reader;
  LineReader stderr_reader;
  std::optional<int> exit_code;

  while (!stop_token.stop_requested()) {
    if (process_.pid > 0) {
      auto status = ReapChild(process_.pid);
      if (status.has_value()) {
        exit_code = status;
        process_.pid = -1;
        break;
      }
    }

    pollfd descriptors[2] = {
        {process_.stdout_fd, static_cast<short>(POLLIN | POLLHUP), 0},
        {process_.stderr_fd, static_cast<short>(POLLIN | POLLHUP), 0},
    };
    int ready = ::poll(descriptors, 2, 100);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;

    std::vector<std::string> lines;
    if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
      stdout_reader.Read(process_.stdout_fd, lines);
      EmitLines(std::move(lines), RunnerEvent::Type::StdoutLine, events_, screen_);
    }
    lines.clear();
    if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
      stderr_reader.Read(process_.stderr_fd, lines);
      EmitLines(std::move(lines), RunnerEvent::Type::StderrLine, events_, screen_);
    }
  }

  if (stop_token.stop_requested() && process_.pid > 0) {
    (void)::kill(process_.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
      auto status = ReapChild(process_.pid);
      if (status.has_value()) {
        exit_code = status;
        process_.pid = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (process_.pid > 0) {
      (void)::kill(process_.pid, SIGKILL);
      for (int i = 0; i < 20; ++i) {
        auto status = ReapChild(process_.pid);
        if (status.has_value()) {
          exit_code = status;
          process_.pid = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }

  if (process_.pid > 0) TerminateDshProcess(process_);

  // The child is gone: drain whatever remains in the pipes for a bounded time.
  if (!stdout_reader.eof || !stderr_reader.eof) {
    std::vector<std::string> lines;
    stdout_reader.DrainFor(process_.stdout_fd, lines, std::chrono::milliseconds(600));
    EmitLines(std::move(lines), RunnerEvent::Type::StdoutLine, events_, screen_);
    lines.clear();
    stderr_reader.DrainFor(process_.stderr_fd, lines, std::chrono::milliseconds(300));
    EmitLines(std::move(lines), RunnerEvent::Type::StderrLine, events_, screen_);
  }

  RunnerEvent event;
  event.type = RunnerEvent::Type::ProcessExit;
  event.exit_code = exit_code.value_or(130);
  event.signaled = exit_code.has_value() && *exit_code > 128;
  (void)events_->send_for(std::move(event), std::chrono::milliseconds(500));
  PostWakeup(screen_);

  CloseFd(process_.stdout_fd);
  CloseFd(process_.stderr_fd);
  if (events_ != nullptr) events_->close();
}

}  // namespace dsh_tui
