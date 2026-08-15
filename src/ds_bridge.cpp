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
  std::string buffer;
  char chunk[8192];

  while (!stop_token.stop_requested()) {
    pollfd descriptor{event_fd_, static_cast<short>(POLLIN | POLLHUP), 0};
    int ready = ::poll(&descriptor, 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;

    ssize_t count = ::read(event_fd_, chunk, sizeof(chunk));
    if (count <= 0) {
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      break;
    }
    buffer.append(chunk, static_cast<size_t>(count));
    if (buffer.size() > kMaxLineBytes) break;

    size_t newline = std::string::npos;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (line.empty()) continue;

      auto json = Json::parse(line);
      if (!json.has_value()) continue;
      auto event = ParseInboundEvent(*json);
      if (!event.has_value()) continue;

      if (events_ == nullptr) continue;
      auto sent = events_->send_for(std::move(*event), std::chrono::milliseconds(500));
      if (!sent.ok) {
        buffer.clear();
        break;
      }
      PostWakeup(screen_);
    }
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
