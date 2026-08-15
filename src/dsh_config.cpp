#include "dsh_config.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dsh_tui {

namespace {

std::string Trim(std::string value) {
  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> SplitCommaList(std::string_view value) {
  std::vector<std::string> result;
  std::string current;
  for (char c : value) {
    if (c == ',') {
      std::string item = Trim(current);
      if (!item.empty()) result.push_back(std::move(item));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  std::string item = Trim(current);
  if (!item.empty()) result.push_back(std::move(item));
  return result;
}

std::string HomeDirectory() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return home;
  }
  return {};
}

void ApplyConfigFile(const std::filesystem::path& path, DshConfigDefaults& config) {
  std::ifstream input(path);
  if (!input) return;
  std::string line;
  while (std::getline(input, line)) {
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    std::string key = Trim(line.substr(0, equals));
    std::string value = Trim(line.substr(equals + 1));
    if (key == "remoteshell") {
      if (!value.empty()) config.remote_shell = value;
    } else if (key == "remoteshellopt") {
      if (!value.empty()) config.remote_shell_options = SplitCommaList(value);
    } else if (key == "forklimit") {
      try { config.fork_limit = std::max(0, std::stoi(value)); } catch (...) {}
    } else if (key == "waitshell") {
      config.wait_shell = value == "1";
    } else if (key == "showmachinenames") {
      config.show_machine_names = value == "1";
    } else if (key == "verbose") {
      config.verbose = value == "1";
    }
  }
}

}  // namespace

std::string DefaultDshBinary() {
  if (std::filesystem::exists("/usr/bin/dsh")) return "/usr/bin/dsh";
  return "dsh";
}

DshConfigDefaults LoadDshConfig() {
  DshConfigDefaults config;
  // System defaults first, then the per-user override file.
  ApplyConfigFile("/etc/dsh/dsh.conf", config);
  const std::string home = HomeDirectory();
  if (!home.empty()) {
    ApplyConfigFile(std::filesystem::path(home) / ".dsh" / "dsh.conf", config);
  }
  if (config.remote_shell.empty()) config.remote_shell = "ssh";
  return config;
}

std::vector<std::string> SplitMachines(std::string_view value) {
  return SplitCommaList(value);
}

std::vector<std::string> ReadMachineFile(const std::string& path) {
  std::vector<std::string> hosts;
  std::ifstream input(path);
  if (!input) return hosts;
  std::string line;
  while (std::getline(input, line)) {
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::string host = Trim(line);
    if (!host.empty()) hosts.push_back(std::move(host));
  }
  return hosts;
}

std::vector<std::string> ResolveHosts(const DshInvocation& invocation) {
  switch (invocation.target_mode) {
    case TargetMode::Machines:
      return SplitMachines(invocation.machines);
    case TargetMode::File:
      return ReadMachineFile(invocation.file);
    case TargetMode::Group: {
      if (invocation.group.empty()) return {};
      const std::string home = HomeDirectory();
      if (!home.empty()) {
        auto hosts = ReadMachineFile((std::filesystem::path(home) / ".dsh" / "group" /
                                      invocation.group).string());
        if (!hosts.empty()) return hosts;
      }
      return ReadMachineFile((std::filesystem::path("/etc/dsh/group") / invocation.group).string());
    }
    case TargetMode::All: {
      const std::string home = HomeDirectory();
      if (!home.empty()) {
        auto hosts = ReadMachineFile((std::filesystem::path(home) / ".dsh" /
                                      "machines.list").string());
        if (!hosts.empty()) return hosts;
      }
      return ReadMachineFile("/etc/dsh/machines.list");
    }
  }
  return {};
}

std::vector<std::string> BuildDshArgv(const DshInvocation& invocation) {
  std::vector<std::string> argv;
  argv.push_back(invocation.dsh_binary);

  switch (invocation.target_mode) {
    case TargetMode::Machines:
      if (!Trim(invocation.machines).empty()) {
        argv.push_back("-m");
        argv.push_back(Trim(invocation.machines));
      }
      break;
    case TargetMode::Group:
      if (!Trim(invocation.group).empty()) {
        argv.push_back("-g");
        argv.push_back(Trim(invocation.group));
      }
      break;
    case TargetMode::All:
      argv.push_back("-a");
      break;
    case TargetMode::File:
      if (!Trim(invocation.file).empty()) {
        argv.push_back("-f");
        argv.push_back(Trim(invocation.file));
      }
      break;
  }

  if (!Trim(invocation.remote_shell).empty()) {
    argv.push_back("-r");
    argv.push_back(Trim(invocation.remote_shell));
  }
  for (const auto& option : invocation.remote_shell_options) {
    std::string cleaned = Trim(option);
    if (!cleaned.empty()) {
      argv.push_back("-o");
      argv.push_back(std::move(cleaned));
    }
  }

  if (invocation.run_mode == RunMode::Wait) {
    argv.push_back("-w");
  } else {
    argv.push_back("-c");
  }
  if (invocation.run_mode == RunMode::ForkLimit && invocation.fork_limit > 0) {
    argv.push_back("-F");
    argv.push_back(std::to_string(invocation.fork_limit));
  }

  argv.push_back(invocation.show_machine_names ? "-M" : "-H");
  if (invocation.verbose) argv.push_back("-v");
  argv.push_back("--");
  argv.push_back(invocation.command);
  return argv;
}

std::string TargetSummary(const DshInvocation& invocation) {
  switch (invocation.target_mode) {
    case TargetMode::Machines: return "machines: " + invocation.machines;
    case TargetMode::Group: return "group: " + invocation.group;
    case TargetMode::All: return "all";
    case TargetMode::File: return "file: " + invocation.file;
  }
  return {};
}

}  // namespace dsh_tui
