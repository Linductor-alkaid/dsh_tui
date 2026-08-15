#pragma once

#include <string>
#include <vector>

namespace dsh_tui {

struct BridgeProcess {
  int pid = -1;
  int event_fd = -1;    // parent reads JSON events emitted by dsh on its fd 3
  int command_fd = -1;  // parent writes commands read by dsh on its fd 4
};

/// Build the explicit DeepSeek Harness launcher argv. Never uses a bare `dsh`
/// so apt's dancer's distributed shell cannot be picked up by accident.
std::vector<std::string> BuildDeepSeekLauncherArgv(const std::string& resume_session_id);

/// Ensure `$DSH_HOME/profiles/tui` exists and links this checkout's
/// packages/dsh-tui bundle into the profile node_modules.
bool EnsureTuiProfile(std::string& error);

/// Fork/exec DeepSeek Harness with fd 3/4 wired as the TUI bridge transport.
/// `DSH_TUI_PARENT=1` tells the JS bridge to use those fds instead of
/// spawning another native frontend.
BridgeProcess SpawnDeepSeekBridge(const std::string& resume_session_id,
                                  std::string& error);

/// Reap a bridge child without blocking indefinitely.
void ReapBridgeProcess(BridgeProcess& process);

}  // namespace dsh_tui
