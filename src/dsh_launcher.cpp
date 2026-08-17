#include "dsh_launcher.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
#ifdef _WIN32
  if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    return profile;
  }
  const char* drive = std::getenv("HOMEDRIVE");
  const char* path = std::getenv("HOMEPATH");
  if (drive != nullptr && path != nullptr) return std::string(drive) + path;
#endif
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
    std::error_code temp_ec;
    fs::path temp = fs::temp_directory_path(temp_ec);
    if (!temp_ec) {
#ifdef _WIN32
      const auto process_id = static_cast<unsigned long>(::GetCurrentProcessId());
#else
      const auto process_id = static_cast<unsigned long>(::getuid());
#endif
      candidates.push_back(temp / ("dsh_tui-home-" + std::to_string(process_id)));
    }
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
    size_t end = value.find(
#ifdef _WIN32
        ';',
#else
        ':',
#endif
        begin);
    std::string directory = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!directory.empty()) {
      const fs::path candidate = fs::path(directory) / executable;
      if (fs::is_regular_file(candidate)) return true;
#ifdef _WIN32
      if (candidate.extension().empty()) {
        for (const char* extension : {".exe", ".cmd", ".bat", ".com"}) {
          if (fs::is_regular_file(candidate.string() + extension)) return true;
        }
      }
#endif
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

void CloseFd(int& fd) {
  if (fd >= 0) {
#ifdef _WIN32
    ::_close(fd);
#else
    ::close(fd);
#endif
    fd = -1;
  }
}

#ifdef _WIN32
class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
    if (const char* current = std::getenv(name); current != nullptr) {
      previous_ = current;
      had_previous_ = true;
    }
    (void)::_putenv_s(name, value.c_str());
  }
  ~ScopedEnvironment() {
    (void)::_putenv_s(name_.c_str(), had_previous_ ? previous_.c_str() : "");
  }

 private:
  std::string name_;
  std::string previous_;
  bool had_previous_ = false;
};

#endif

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
    const fs::path bin = it->path() / "node_modules" / ".bin" /
#ifdef _WIN32
                         "dsh.cmd";
#else
                         "dsh";
#endif
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
    if (fs::path(cached).extension() == ".js") argv.push_back("node");
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
        "    \"dsh-tui\": \"file:" + package_dir.generic_string() + "\"\n"
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
#ifdef _WIN32
  if (fs::exists(link, ec)) {
    fs::remove_all(link, ec);
    if (ec) {
      error = "cannot refresh profile bundle: " + ec.message();
      return false;
    }
  }
  fs::copy(package_dir, link, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) {
    error = "cannot copy profile bundle: " + ec.message();
    return false;
  }
#else
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
#endif
  return true;
}

BridgeProcess SpawnDeepSeekBridge(const std::string& resume_session_id,
                                  std::string& error) {
  if (std::getenv("DSH_BIN") == nullptr && FindCachedDeepSeekLauncher().empty() &&
      !FindOnPath("npx")) {
    error = "DeepSeek Harness launcher not found; install it with `npx @deepseek-ai/dsh` or set DSH_BIN";
    return {};
  }

  std::vector<std::string> argv = BuildDeepSeekLauncherArgv(resume_session_id);
#ifdef _WIN32
  SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const std::string pipe_prefix = "\\\\.\\pipe\\dsh-tui-" +
                                  std::to_string(::GetCurrentProcessId()) + "-" +
                                  std::to_string(::GetTickCount64());
  const std::string event_pipe_name = pipe_prefix + "-events";
  const std::string command_pipe_name = pipe_prefix + "-commands";
  HANDLE event_read = ::CreateNamedPipeA(
      event_pipe_name.c_str(), PIPE_ACCESS_INBOUND,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 65536, 65536, 0, nullptr);
  HANDLE command_write = ::CreateNamedPipeA(
      command_pipe_name.c_str(), PIPE_ACCESS_OUTBOUND,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 65536, 65536, 0, nullptr);
  HANDLE stderr_read = nullptr, stderr_write = nullptr;
  if (event_read == INVALID_HANDLE_VALUE || command_write == INVALID_HANDLE_VALUE ||
      !::CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
    error = "cannot create Windows bridge pipes: " + std::to_string(::GetLastError());
    if (event_read != INVALID_HANDLE_VALUE) ::CloseHandle(event_read);
    if (command_write != INVALID_HANDLE_VALUE) ::CloseHandle(command_write);
    return {};
  }
  ::SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
  int event_read_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(event_read), _O_RDONLY | _O_BINARY);
  int command_write_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(command_write), _O_WRONLY | _O_BINARY);
  int stderr_read_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(stderr_read), _O_RDONLY | _O_BINARY);
  if (event_read_fd < 0 || command_write_fd < 0 || stderr_read_fd < 0) {
    error = "_open_osfhandle() failed";
    CloseFd(event_read_fd); CloseFd(command_write_fd); CloseFd(stderr_read_fd);
    ::CloseHandle(stderr_write);
    return {};
  }
  std::string command_line = "cmd.exe /d /s /c \"\"" + argv[0] + "\"";
  for (size_t i = 1; i < argv.size(); ++i) command_line += " \"" + argv[i] + "\"";
  command_line += "\"";
  STARTUPINFOA startup = {sizeof(STARTUPINFOA)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = stderr_write;
  PROCESS_INFORMATION info{};
  ScopedEnvironment parent_flag("DSH_TUI_PARENT", "1");
  ScopedEnvironment source_dir("DSH_TUI_SOURCE_DIR", DSH_TUI_SOURCE_DIR);
  ScopedEnvironment selected_home("DSH_HOME", DshHome());
  ScopedEnvironment event_pipe("DSH_TUI_EVENT_PIPE", event_pipe_name);
  ScopedEnvironment command_pipe("DSH_TUI_COMMAND_PIPE", command_pipe_name);
  if (!::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
    error = "CreateProcess() failed: " + std::to_string(::GetLastError());
    CloseFd(event_read_fd); CloseFd(command_write_fd); CloseFd(stderr_read_fd);
    ::CloseHandle(stderr_write);
    return {};
  }
  ::CloseHandle(info.hThread);
  ::CloseHandle(stderr_write);
  auto poll_connection = [](HANDLE pipe, bool& connected) {
    if (connected) return;
    if (::ConnectNamedPipe(pipe, nullptr) != FALSE || ::GetLastError() == ERROR_PIPE_CONNECTED) {
      connected = true;
    }
  };
  bool event_connected = false;
  bool command_connected = false;
  for (int attempt = 0; attempt < 200 && (!event_connected || !command_connected); ++attempt) {
    poll_connection(event_read, event_connected);
    poll_connection(command_write, command_connected);
    if (::WaitForSingleObject(info.hProcess, 0) == WAIT_OBJECT_0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!event_connected || !command_connected) {
    error = "timed out connecting Windows bridge pipes";
    (void)::TerminateProcess(info.hProcess, 1);
    ::CloseHandle(info.hProcess);
    CloseFd(event_read_fd); CloseFd(command_write_fd); CloseFd(stderr_read_fd);
    return {};
  }
  DWORD pipe_mode = PIPE_READMODE_BYTE | PIPE_WAIT;
  (void)::SetNamedPipeHandleState(event_read, &pipe_mode, nullptr, nullptr);
  (void)::SetNamedPipeHandleState(command_write, &pipe_mode, nullptr, nullptr);
  BridgeProcess process;
  process.process_handle = reinterpret_cast<std::intptr_t>(info.hProcess);
  process.pid = info.dwProcessId;
  process.event_fd = event_read_fd;
  process.command_fd = command_write_fd;
  process.stderr_fd = stderr_read_fd;
  return process;
#else
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
  process.process_handle = pid;
  process.pid = pid;
  process.event_fd = event_pipe[0];
  process.command_fd = command_pipe[1];
  process.stderr_fd = stderr_pipe[0];
  return process;
#endif
}

void ReapBridgeProcess(BridgeProcess& process) {
  CloseFd(process.stderr_fd);
#ifdef _WIN32
  CloseFd(process.event_fd);
  CloseFd(process.command_fd);
  if (process.process_handle < 0) return;
  HANDLE handle = reinterpret_cast<HANDLE>(process.process_handle);
  if (::WaitForSingleObject(handle, 2000) == WAIT_TIMEOUT) {
    (void)::TerminateProcess(handle, 1);
    (void)::WaitForSingleObject(handle, 2000);
  }
  ::CloseHandle(handle);
  process.process_handle = -1;
  process.pid = 0;
  return;
#else
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
#endif
}

}  // namespace dsh_tui
