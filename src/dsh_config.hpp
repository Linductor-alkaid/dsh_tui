#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dsh_tui {

enum class TargetMode { Machines, Group, All, File };
enum class RunMode { Concurrent, Wait, ForkLimit };

struct DshConfigDefaults {
  std::string remote_shell = "ssh";
  std::vector<std::string> remote_shell_options;
  int fork_limit = 0;
  bool wait_shell = false;
  bool show_machine_names = true;
  bool verbose = false;
};

struct DshInvocation {
  std::string dsh_binary = "/usr/bin/dsh";
  TargetMode target_mode = TargetMode::Machines;
  std::string machines;
  std::string group;
  std::string file;
  std::string remote_shell = "ssh";
  std::vector<std::string> remote_shell_options;
  RunMode run_mode = RunMode::Concurrent;
  int fork_limit = 0;
  bool show_machine_names = true;
  bool verbose = false;
  std::string command;
};

DshConfigDefaults LoadDshConfig();
std::vector<std::string> SplitMachines(std::string_view value);
std::vector<std::string> ReadMachineFile(const std::string& path);
std::vector<std::string> ResolveHosts(const DshInvocation& invocation);
std::vector<std::string> BuildDshArgv(const DshInvocation& invocation);
std::string TargetSummary(const DshInvocation& invocation);
std::string DefaultDshBinary();

}  // namespace dsh_tui
