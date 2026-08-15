#include "ds_bridge.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include "ftxui/component/app.hpp"
#include "ftxui/component/event.hpp"

namespace dsh_tui {

namespace {

constexpr size_t kMaxLineBytes = 8U * 1024U * 1024U;

void PostWakeup(ftxui::App* screen) {
  if (screen != nullptr) screen->PostEvent(ftxui::Event::Custom);
}

}  // namespace

void BridgeReader::run(std::stop_token stop_token) {
  for (int fd : {event_fd_, stderr_fd_}) {
    if (fd < 0) continue;
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  struct LineReader {
    std::string buffer;
    bool eof = false;
    void Read(int fd, std::vector<std::string>& lines) {
      if (eof || fd < 0) return;
      char chunk[8192];
      for (;;) {
        ssize_t count = ::read(fd, chunk, sizeof(chunk));
        if (count > 0) {
          buffer.append(chunk, static_cast<size_t>(count));
          if (buffer.size() > kMaxLineBytes) buffer.resize(kMaxLineBytes);
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
  };

  LineReader event_reader;
  LineReader log_reader;
  while (!stop_token.stop_requested()) {
    pollfd descriptors[2] = {
        {event_fd_, static_cast<short>(POLLIN | POLLHUP), 0},
        {stderr_fd_, static_cast<short>(stderr_fd_ >= 0 ? (POLLIN | POLLHUP) : 0), 0},
    };
    int ready = ::poll(descriptors, stderr_fd_ >= 0 ? 2 : 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;

    std::vector<std::string> lines;
    if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
      event_reader.Read(event_fd_, lines);
      for (auto& line : lines) {
        if (std::getenv("DSH_TUI_DEBUG_BRIDGE") != nullptr) {
          std::fprintf(stderr, "bridge-line: %s\n", line.c_str());
        }
        auto json = Json::parse(line);
        if (!json.has_value()) continue;
        auto event = ParseInboundEvent(*json);
        if (!event.has_value()) continue;
        if (std::getenv("DSH_TUI_DEBUG_BRIDGE") != nullptr) {
          std::fprintf(stderr, "bridge-reader: %s %s\n", event->text.c_str(), event->secondary.c_str());
        }
        if (events_ == nullptr) continue;
        auto sent = events_->send_for(std::move(*event), std::chrono::milliseconds(500));
        PostWakeup(screen_);
        if (!sent.ok) { lines.clear(); break; }
      }
      lines.clear();
    }
    if (stderr_fd_ >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
      log_reader.Read(stderr_fd_, lines);
      for (auto& line : lines) {
        InboundEvent log_event;
        log_event.type = InboundEvent::Type::BridgeLog;
        log_event.text = std::move(line);
        if (events_ == nullptr) continue;
        auto sent = events_->send_for(std::move(log_event), std::chrono::milliseconds(500));
        PostWakeup(screen_);
        if (!sent.ok) break;
      }
    }
    if (event_reader.eof && (stderr_fd_ < 0 || log_reader.eof)) break;
  }

  if (events_ != nullptr) {
    InboundEvent bye;
    bye.type = InboundEvent::Type::Bye;
    bye.text = "bridge-closed";
    (void)events_->send_for(std::move(bye), std::chrono::milliseconds(500));
    events_->close();
    PostWakeup(screen_);
  }
}

bool SendCommand(int command_fd, const OutboundCommand& command) {
  if (command_fd < 0) return false;
  std::string line = OutboundJson(command);
  line.push_back('\n');
  size_t written = 0;
  while (written < line.size()) {
    ssize_t count = ::write(command_fd, line.data() + written, line.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        pollfd descriptor{command_fd, POLLOUT, 0};
        if (::poll(&descriptor, 1, 200) <= 0) return false;
        continue;
      }
      return false;
    }
    written += static_cast<size_t>(count);
  }
  return true;
}

}  // namespace dsh_tui
