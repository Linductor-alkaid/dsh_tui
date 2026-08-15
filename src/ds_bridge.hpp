#pragma once

#include <stop_token>

#include "ds_protocol.hpp"
#include "executor/comm.hpp"
#include "executor/executor.hpp"

namespace ftxui {
class App;
}

namespace dsh_tui {

/// Reads JSON-line events from fd 3 and forwards them through an executor MPSC
/// channel to the FTXUI loop. Runs on a dedicated executor Blocking I/O worker.
class BridgeReader final : public executor::IBlockingIoWorker {
 public:
  BridgeReader(int event_fd, executor::comm::MpscChannel<InboundEvent>* events,
               ftxui::App* screen)
      : event_fd_(event_fd), events_(events), screen_(screen) {}

  void run(std::stop_token stop_token) override;
  void wakeup() noexcept override {}

 private:
  int event_fd_ = -1;
  executor::comm::MpscChannel<InboundEvent>* events_ = nullptr;
  ftxui::App* screen_ = nullptr;
};

bool SendCommand(int command_fd, const OutboundCommand& command);

}  // namespace dsh_tui
