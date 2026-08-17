#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsh_tui {

struct BridgeProcess {
  std::intptr_t process_handle = -1;
  std::intptr_t pid = -1;
  int event_fd = -1;    // parent reads JSON events from fd 3 or a Windows pipe
  int command_fd = -1;  // parent writes commands to fd 4 or a Windows pipe
  int stderr_fd = -1;   // captured dsh/npx diagnostics for the status rail
};

/// Build the explicit DeepSeek Harness launcher argv. Never uses a bare `dsh`
/// so apt's dancer's distributed shell cannot be picked up by accident.
std::vector<std::string> BuildDeepSeekLauncherArgv(const std::string& resume_session_id);

/// Ensure `$DSH_HOME/profiles/tui` exists and links this checkout's
/// packages/dsh-tui bundle into the profile node_modules.
bool EnsureTuiProfile(std::string& error);

/// Launch DeepSeek Harness with fd 3/4 (POSIX) or named pipes (Windows).
/// `DSH_TUI_PARENT=1` tells the JS bridge to use that transport instead of
/// spawning another native frontend.
BridgeProcess SpawnDeepSeekBridge(const std::string& resume_session_id,
                                  std::string& error);

/// Reap a bridge child without blocking indefinitely.
void ReapBridgeProcess(BridgeProcess& process);

}  // namespace dsh_tui
