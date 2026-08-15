#include "dsh_launcher.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#ifndef DSH_TUI_SOURCE_DIR
#define DSH_TUI_SOURCE_DIR ""
#endif

namespace dsh_tui {

namespace {

namespace fs = std::filesystem;

std::string HomeDirectory() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') return home;
  return {};
}

std::string DshHome() {
  static const std::string selected = [] {
    if (const char* env = std::getenv("DSH_HOME"); env != nullptr && *env != 0) {
      return std::string(env);
    }
    const fs::path default_home = fs::path(HomeDirectory()) / ".dsh";
    std::error_code ec;
    fs::create_directories(default_home / "profiles" / "tui", ec);
    ec.clear();
    const fs::path probe = default_home / "profiles" / "tui" / ".dsh_tui_write_test";
    bool writable = false;
    {
      std::ofstream output(probe);
      writable = output.good();
    }
    fs::remove(probe, ec);
    if (writable) return default_home.string();
    const std::string home = HomeDirectory();
    std::vector<fs::path> candidates;
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != 0) {
      candidates.push_back(fs::path(xdg) / "dsh_tui");
    }
    if (!home.empty()) {
      candidates.push_back(fs::path(home) / ".local" / "state" / "dsh_tui");
      candidates.push_back(fs::path(home) / ".cache" / "dsh_tui");
    }
    candidates.push_back(fs::path("/tmp") / ("dsh_tui-home-" + std::to_string(::getuid())));
    for (const auto& candidate : candidates) {
      fs::create_directories(candidate / "profiles" / "tui", ec);
      ec.clear();
      const fs::path candidate_probe = candidate / "profiles" / "tui" / ".dsh_tui_write_test";
      std::ofstream candidate_output(candidate_probe);
      if (!candidate_output) continue;
      candidate_output << "ok";
      candidate_output.close();
      fs::remove(candidate_probe, ec);
      return candidate.string();
    }
    return default_home.string();
  }();
  return selected;
}
void WriteFile(const fs::path& path, std::string_view content) {
  std::ofstream output(path);
  output << content;
}

bool FindOnPath(const std::string& executable) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) return false;
  std::string value = path;
  size_t begin = 0;
  while (begin <= value.size()) {
    size_t end = value.find(':', begin);
    std::string directory = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!directory.empty() && fs::is_regular_file(fs::path(directory) / executable)) return true;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

void CloseFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

}  // namespace

std::string FindCachedDeepSeekLauncher() {
  const fs::path home = HomeDirectory();
  if (home.empty()) return {};
  const fs::path npx_root = home / ".npm" / "_npx";
  std::error_code ec;
  if (!fs::exists(npx_root, ec)) return {};
  for (fs::directory_iterator it(npx_root, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path package = it->path() / "node_modules" / "@deepseek-ai" / "dsh";
    if (!fs::exists(package / "package.json", ec)) continue;
    const fs::path bin = it->path() / "node_modules" / ".bin" / "dsh";
    if (fs::is_regular_file(bin) || fs::is_symlink(bin)) return bin.string();
    const fs::path js = package / "lib" / "bin.js";
    if (fs::is_regular_file(js)) return js.string();
  }
  return {};
}

std::vector<std::string> BuildDeepSeekLauncherArgv(const std::string& resume_session_id) {
  std::vector<std::string> argv;
  if (const char* custom = std::getenv("DSH_BIN"); custom != nullptr && *custom != '\0') {
    argv.push_back(custom);
  } else if (const std::string cached = FindCachedDeepSeekLauncher(); !cached.empty()) {
    argv.push_back(cached);
  } else {
    argv.push_back("npx");
    argv.push_back("--yes");
    argv.push_back("@deepseek-ai/dsh");
  }
  argv.push_back("--profile");
  argv.push_back("tui");
  if (!resume_session_id.empty()) {
    argv.push_back("--resume");
    argv.push_back(resume_session_id);
  }
  return argv;
}

bool EnsureTuiProfile(std::string& error) {
  const std::string source_root = DSH_TUI_SOURCE_DIR;
  if (source_root.empty()) {
    error = "DSH_TUI_SOURCE_DIR is not compiled in; run scripts/setup-profile.sh first";
    return false;
  }

  const fs::path profile_dir = fs::path(DshHome()) / "profiles" / "tui";
  const fs::path modules_dir = profile_dir / "node_modules";
  const fs::path package_dir = fs::path(source_root) / "packages" / "dsh-tui";
  if (!fs::exists(package_dir / "package.json")) {
    error = "dsh-tui profile bundle not found at " + package_dir.string();
    return false;
  }

  std::error_code ec;
  fs::create_directories(profile_dir, ec);
  fs::create_directories(modules_dir, ec);
  if (ec) {
    error = "cannot create profile directory: " + ec.message();
    return false;
  }

  if (!fs::exists(profile_dir / "package.json")) {
    std::string manifest =
        "{\n"
        "  \"name\": \"dsh-profile-tui\",\n"
        "  \"private\": true,\n"
        "  \"dependencies\": {\n"
        "    \"dsh-tui\": \"file:" + package_dir.string() + "\"\n"
        "  },\n"
        "  \"dsh\": {\n"
        "    \"profile\": {\n"
        "      \"bundles\": [\n"
        "        \"@deepseek-ai/dsh-base\",\n"
        "        \"dsh-tui\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n";
    WriteFile(profile_dir / "package.json", manifest);
  }
  if (!fs::exists(profile_dir / "cordis.patch.yml")) {
    WriteFile(profile_dir / "cordis.patch.yml", "# User overrides for the dsh TUI profile.\n[]\n");
  }
  if (!fs::exists(profile_dir / "pnpm-workspace.yaml")) {
    WriteFile(profile_dir / "pnpm-workspace.yaml",
              "packages:\n  - .\n\nnodeLinker: hoisted\nautoInstallPeers: false\n");
  }

  const fs::path link = modules_dir / "dsh-tui";
  if (fs::exists(link) || fs::is_symlink(link)) {
    if (!fs::is_symlink(link)) {
      error = "profile node_modules/dsh-tui exists and is not a symlink";
      return false;
    }
    std::string current_target = fs::read_symlink(link, ec);
    ec.clear();
    if (current_target != package_dir.string()) {
      fs::remove(link, ec);
      ec.clear();
    }
  }
  if (!fs::exists(link)) {
    fs::create_directory_symlink(package_dir, link, ec);
    if (ec) {
      error = "cannot link profile bundle: " + ec.message();
      return false;
    }
  }
  return true;
}

BridgeProcess SpawnDeepSeekBridge(const std::string& resume_session_id,
                                  std::string& error) {
  if (std::getenv("DSH_BIN") == nullptr && !FindOnPath("npx")) {
    error = "npx not found on PATH; install Node.js/npm or set DSH_BIN";
    return {};
  }

  std::vector<std::string> argv = BuildDeepSeekLauncherArgv(resume_session_id);
  int event_pipe[2] = {-1, -1};
  int command_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (::pipe(event_pipe) != 0 || ::pipe(command_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
    error = "pipe() failed: " + std::string(std::strerror(errno));
    CloseFd(event_pipe[0]); CloseFd(event_pipe[1]);
    CloseFd(command_pipe[0]); CloseFd(command_pipe[1]);
    CloseFd(stderr_pipe[0]); CloseFd(stderr_pipe[1]);
    return {};
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    error = "fork() failed: " + std::string(std::strerror(errno));
    CloseFd(event_pipe[0]); CloseFd(event_pipe[1]);
    CloseFd(command_pipe[0]); CloseFd(command_pipe[1]);
    CloseFd(stderr_pipe[0]); CloseFd(stderr_pipe[1]);
    return {};
  }

  if (pid == 0) {
    (void)::dup2(event_pipe[1], 3);
    (void)::dup2(command_pipe[0], 4);
    (void)::dup2(stderr_pipe[1], STDERR_FILENO);
    for (int fd : {event_pipe[0], event_pipe[1], command_pipe[0], command_pipe[1],
                   stderr_pipe[0], stderr_pipe[1]}) {
      if (fd != 3 && fd != 4 && fd != STDERR_FILENO) CloseFd(fd);
    }
    (void)::setenv("DSH_TUI_PARENT", "1", 1);
    (void)::setenv("DSH_TUI_SOURCE_DIR", DSH_TUI_SOURCE_DIR, 1);
    const std::string selected_home = DshHome();
    (void)::setenv("DSH_HOME", selected_home.c_str(), 1);

    if (std::getenv("DSH_TUI_DEBUG") == nullptr) {
      int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        (void)::dup2(devnull, STDOUT_FILENO);
        if (devnull > 2) ::close(devnull);
      }
    }

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (auto& arg : argv) raw_argv.push_back(arg.data());
    raw_argv.push_back(nullptr);
    ::execvp(raw_argv[0], raw_argv.data());
    std::fprintf(stderr, "dsh_tui: execvp(%s) failed: %s\n", raw_argv[0],
                 std::strerror(errno));
    _exit(127);
  }

  CloseFd(event_pipe[1]);
  CloseFd(command_pipe[0]);
  CloseFd(stderr_pipe[1]);
  if (std::getenv("DSH_TUI_DEBUG_FDS") != nullptr) {
    std::fprintf(stderr, "dsh_tui spawn pid=%d event_fd=%d command_fd=%d stderr_fd=%d argv0=%s\n",
                 pid, event_pipe[0], command_pipe[1], stderr_pipe[0], argv[0].c_str());
  }
  BridgeProcess process;
  process.pid = pid;
  process.event_fd = event_pipe[0];
  process.command_fd = command_pipe[1];
  process.stderr_fd = stderr_pipe[0];
  return process;
}

void ReapBridgeProcess(BridgeProcess& process) {
  CloseFd(process.stderr_fd);
  if (process.pid <= 0) return;
  int status = 0;
  for (int i = 0; i < 40; ++i) {
    pid_t result = ::waitpid(process.pid, &status, WNOHANG);
    if (result == process.pid) {
      process.pid = -1;
      break;
    }
    if (result < 0 && errno != EINTR) {
      process.pid = -1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (process.pid > 0) {
    (void)::kill(process.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
      if (::waitpid(process.pid, &status, WNOHANG) == process.pid) { process.pid = -1; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

}  // namespace dsh_tui
