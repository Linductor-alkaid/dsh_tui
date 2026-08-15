#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ftxui/component/app.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include "dsh_config.hpp"
#include "dsh_runner.hpp"
#include "executor/executor.hpp"
#include "output_state.hpp"

using namespace ftxui;

namespace dsh_tui {
namespace {

constexpr std::string_view kVersion = "dsh_tui 0.2.0";

void PrintUsage() {
  std::cout
      << "dsh_tui — FTXUI + executor powered TUI for dancer's shell (dsh)\n\n"
      << "Usage:\n"
      << "  dsh_tui [--dsh /usr/bin/dsh]\n"
      << "  dsh_tui --run-once --machines node1,node2 --command 'hostname'\n"
      << "  dsh_tui --self-test\n"
      << "  dsh_tui --help\n\n"
      << "The UI builds a `dsh` command line (machine/group/all/file target,\n"
      << "rsh/ssh options, concurrent/wait execution) and streams per-host output.\n";
}

std::string Trim(std::string value) {
  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string JoinOptions(const std::vector<std::string>& options) {
  std::string joined;
  for (size_t i = 0; i < options.size(); ++i) {
    if (i != 0) joined.push_back(',');
    joined += options[i];
  }
  return joined;
}

std::vector<std::string> SplitOptions(std::string value) {
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

Element RenderHostSection(const std::string& host, const HostOutput& host_output,
                          size_t tail_lines) {
  Elements lines;
  size_t start = host_output.lines.size() > tail_lines ? host_output.lines.size() - tail_lines : 0;
  if (start > 0) {
    lines.push_back(text("  … " + std::to_string(start) + " 行更早输出") | dim);
  }
  for (size_t i = start; i < host_output.lines.size(); ++i) {
    lines.push_back(paragraph(host_output.lines[i]));
  }
  return vbox({
      hbox({
          text("▸ " + host) | bold | color(Color::Cyan),
          text("  " + std::to_string(host_output.lines.size()) + " 行") | dim,
      }),
      vbox(std::move(lines)),
  });
}

Element RenderOutputPanel(const RunOutput& output, size_t tail_lines, size_t max_hosts) {
  Elements body;
  if (!output.completed && !output.running && output.command.empty()) {
    body.push_back(text("在下方输入命令，选择目标机器后执行。") | dim | center);
  } else {
    if (!output.target_summary.empty()) {
      body.push_back(hbox({
          text("目标: ") | dim,
          text(output.target_summary) | color(Color::Green),
          filler(),
          text(output.command.empty() ? "" : "命令: " + output.command) | dim,
      }));
      body.push_back(separatorEmpty());
    }

    if (!output.spawn_error.empty()) {
      body.push_back(text(output.spawn_error) | color(Color::Red));
    }

    size_t shown = 0;
    for (const auto& [host, host_output] : output.hosts) {
      if (shown++ >= max_hosts) {
        body.push_back(text("… 其余主机输出未显示（共 " +
                            std::to_string(output.hosts.size()) + " 台）") | dim);
        break;
      }
      body.push_back(RenderHostSection(host, host_output, tail_lines));
      body.push_back(separatorEmpty());
    }

    if (!output.stderr_lines.empty()) {
      Elements errors;
      size_t start = output.stderr_lines.size() > tail_lines
                         ? output.stderr_lines.size() - tail_lines
                         : 0;
      if (start > 0) errors.push_back(text("  … 更早 stderr") | dim);
      for (size_t i = start; i < output.stderr_lines.size(); ++i) {
        errors.push_back(paragraph(output.stderr_lines[i]) | color(Color::Red));
      }
      body.push_back(text("stderr / dsh 诊断") | bold | color(Color::Red));
      body.push_back(vbox(std::move(errors)));
    }

    if (output.running) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - output.started_at).count();
      body.push_back(text("执行中… " + std::to_string(elapsed) + "s") | color(Color::Yellow));
    } else if (output.completed) {
      Color status_color = output.exit_code == 0 ? Color::Green : Color::Red;
      body.push_back(text("退出码 " + std::to_string(output.exit_code) +
                          (output.signaled ? " (signal)" : "")) | bold | color(status_color));
    }
  }
  return vbox(std::move(body)) | yframe | flex;
}

int RunOnce(const DshInvocation& invocation) {
  if (invocation.command.empty()) {
    std::cerr << "dsh_tui: --command is required for --run-once\n";
    return 2;
  }
  std::string error;
  DshProcess process = SpawnDsh(invocation, error);
  if (process.pid <= 0) {
    std::cerr << "dsh_tui: " << error << "\n";
    return 1;
  }

  auto set_nonblocking = [](int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  };
  set_nonblocking(process.stdout_fd);
  set_nonblocking(process.stderr_fd);

  int exit_code = 1;
  bool child_done = false;
  bool stdout_eof = false;
  bool stderr_eof = false;
  while (true) {
    if (!child_done) {
      int status = 0;
      pid_t result = ::waitpid(process.pid, &status, WNOHANG);
      if (result == process.pid) {
        child_done = true;
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                      : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
      }
    }

    pollfd descriptors[2] = {
        {stdout_eof ? -1 : process.stdout_fd, static_cast<short>(POLLIN | POLLHUP), 0},
        {stderr_eof ? -1 : process.stderr_fd, static_cast<short>(POLLIN | POLLHUP), 0},
    };
    int ready = ::poll(descriptors, 2, child_done ? 120 : 100);
    if (ready < 0 && errno != EINTR) break;

    for (int index = 0; index < 2; ++index) {
      int fd = index == 0 ? process.stdout_fd : process.stderr_fd;
      bool& eof = index == 0 ? stdout_eof : stderr_eof;
      if (fd < 0 || eof) continue;
      char buffer[8192];
      for (;;) {
        ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
          if (index == 0) std::fwrite(buffer, 1, static_cast<size_t>(count), stdout);
          else std::fwrite(buffer, 1, static_cast<size_t>(count), stderr);
          continue;
        }
        if (count == 0) eof = true;
        break;
      }
    }
    std::fflush(stdout);
    std::fflush(stderr);

    if (child_done && stdout_eof && stderr_eof) break;
  }

  ::close(process.stdout_fd);
  ::close(process.stderr_fd);
  return exit_code;
}

int RunSelfTest() {
  DshInvocation invocation;
  invocation.dsh_binary = "/usr/bin/dsh";
  invocation.target_mode = TargetMode::Machines;
  invocation.machines = "node1, user@node2";
  invocation.remote_shell = "ssh";
  invocation.remote_shell_options = {"BatchMode=yes", "ConnectTimeout=5"};
  invocation.run_mode = RunMode::ForkLimit;
  invocation.fork_limit = 8;
  invocation.show_machine_names = true;
  invocation.command = "hostname";

  std::vector<std::string> expected = {
      "/usr/bin/dsh", "-m", "node1, user@node2", "-r", "ssh",
      "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
      "-c", "-F", "8", "-M", "--", "hostname"};
  if (BuildDshArgv(invocation) != expected) {
    std::cerr << "argv assertion failed\n";
    return 1;
  }

  auto hosts = ResolveHosts(invocation);
  if (hosts.size() != 2 || hosts[0] != "node1" || hosts[1] != "user@node2") {
    std::cerr << "host split assertion failed\n";
    return 1;
  }

  RunOutput output;
  output.Start(invocation, hosts);
  RunnerEvent event;
  event.type = RunnerEvent::Type::StdoutLine;
  event.text = "node1: linux";
  output.Apply(event);
  event.text = "user@node2: linux";
  output.Apply(event);
  event.type = RunnerEvent::Type::StderrLine;
  event.text = "warning";
  output.Apply(event);
  event.type = RunnerEvent::Type::ProcessExit;
  event.exit_code = 0;
  output.Apply(event);

  if (!output.completed || output.exit_code != 0 || output.hosts.size() != 2 ||
      output.hosts["node1"].lines.size() != 1 || output.stderr_lines.size() != 1) {
    std::cerr << "output state assertion failed\n";
    return 1;
  }

  std::cout << "dsh_tui self-test ok\n";
  return 0;
}

int RunTui(const std::string& dsh_binary) {
  const DshConfigDefaults defaults = LoadDshConfig();

  executor::Executor executor;
  executor::ExecutorConfig executor_config;
  executor_config.min_threads = 1;
  executor_config.max_threads = 2;
  executor_config.queue_capacity = 256;
  executor_config.enable_monitoring = true;
  if (!executor.initialize(executor_config)) {
    std::cerr << "dsh_tui: failed to initialize executor\n";
    return 2;
  }

  // Form state.
  std::string command;
  std::string machines;
  std::string group;
  std::string file;
  std::string remote_shell = defaults.remote_shell;
  std::string remote_options = JoinOptions(defaults.remote_shell_options);
  std::string fork_limit_text = defaults.fork_limit > 0 ? std::to_string(defaults.fork_limit) : "";
  int target_mode = static_cast<int>(TargetMode::Machines);
  int run_mode = defaults.wait_shell ? static_cast<int>(RunMode::Wait)
                                     : defaults.fork_limit > 0
                                           ? static_cast<int>(RunMode::ForkLimit)
                                           : static_cast<int>(RunMode::Concurrent);
  bool show_machine_names = true;
  bool verbose = defaults.verbose;

  std::vector<std::string> target_entries = {"机器列表 (-m)", "组 (-g)", "全部 (-a)", "文件 (-f)"};
  std::vector<std::string> run_entries = {"并发 (-c)", "顺序等待 (-w)", "并发+限制 (-F)"};

  RunOutput output;
  std::vector<std::string> preview_hosts;
  std::string preview_key;
  std::unique_ptr<executor::comm::MpscChannel<RunnerEvent>> run_events;
  executor::WorkerHandle run_handle;

  auto screen = ftxui::App::Fullscreen();
  screen.ForceHandleCtrlC(true);

  auto StopRun = [&] {
    if (run_handle.started()) run_handle.request_stop();
  };

  auto DrainRunEvents = [&] {
    if (!run_events) return;
    RunnerEvent event;
    while (run_events->try_receive(event)) {
      output.Apply(event);
    }
    if (run_events->is_closed() && run_events->empty() && output.running) {
      output.running = false;
      output.completed = true;
      if (output.exit_code < 0) output.exit_code = 130;
    }
  };

  auto RunAction = [&] {
    if (output.running) return;

    if (run_handle.started()) {
      run_handle.request_stop();
      run_handle.stop();
      run_handle = executor::WorkerHandle{};
    }

    DshInvocation invocation;
    invocation.dsh_binary = dsh_binary;
    invocation.target_mode = static_cast<TargetMode>(target_mode);
    invocation.machines = machines;
    invocation.group = group;
    invocation.file = file;
    invocation.remote_shell = Trim(remote_shell);
    invocation.remote_shell_options = SplitOptions(remote_options);
    invocation.run_mode = static_cast<RunMode>(run_mode);
    invocation.show_machine_names = show_machine_names;
    invocation.verbose = verbose;
    invocation.command = Trim(command);
    if (!invocation.remote_shell.empty() && invocation.remote_shell != "ssh" &&
        invocation.remote_shell != "rsh" && invocation.remote_shell[0] == '-') {
      invocation.remote_shell.clear();
    }

    int fork_limit = 0;
    if (invocation.run_mode == RunMode::ForkLimit) {
      try {
        fork_limit = std::max(0, std::stoi(Trim(fork_limit_text)));
      } catch (...) {
        fork_limit = 0;
      }
    }
    invocation.fork_limit = fork_limit;

    std::string problem;
    if (invocation.command.empty()) problem = "命令不能为空";
    else if (invocation.target_mode == TargetMode::Machines && Trim(machines).empty())
      problem = "机器列表不能为空";
    else if (invocation.target_mode == TargetMode::Group && Trim(group).empty())
      problem = "组名不能为空";
    else if (invocation.target_mode == TargetMode::File && Trim(file).empty())
      problem = "机器文件路径不能为空";
    if (!problem.empty()) {
      output.Clear();
      output.command = invocation.command;
      output.target_summary = TargetSummary(invocation);
      output.completed = true;
      output.exit_code = 1;
      output.spawn_error = problem;
      return;
    }

    auto hosts = ResolveHosts(invocation);
    output.Start(invocation, hosts);

    std::string spawn_error;
    DshProcess process = SpawnDsh(invocation, spawn_error);
    if (process.pid <= 0) {
      output.MarkSpawnError(spawn_error.empty() ? "dsh 启动失败" : spawn_error);
      return;
    }

    executor::comm::ChannelOptions options;
    options.capacity = 8192;
    options.drop_policy = executor::comm::DropPolicy::RejectNewest;
    options.name = "dsh-run-output";
    run_events = std::make_unique<executor::comm::MpscChannel<RunnerEvent>>(options);

    executor::BlockingWorkerSpec spec;
    spec.name = "dsh-run-reader";
    spec.config.thread_name = "dsh-run-reader";
    spec.worker = std::make_unique<DshProcessWorker>(process, run_events.get(), &screen);
    run_handle = executor.start_worker(std::move(spec));
    if (!run_handle.started()) {
      output.MarkSpawnError("dsh 输出读取线程启动失败: " + run_handle.start_result().message);
      run_handle = executor::WorkerHandle{};
      run_events.reset();
    }
  };

  InputOption command_option = InputOption::Spacious();
  command_option.multiline = false;
  command_option.on_enter = RunAction;
  auto input_command = Input(&command, "远程命令，例如：hostname; uptime", command_option);

  InputOption single_line;
  single_line.multiline = false;
  auto input_machines = Input(&machines, "node1,node2 或 user@node1", single_line);
  auto input_group = Input(&group, "组名", single_line);
  auto input_file = Input(&file, "~/machines.list", single_line);
  auto input_shell = Input(&remote_shell, "ssh / rsh / 可执行文件路径", single_line);
  auto input_options = Input(&remote_options, "BatchMode=yes,ConnectTimeout=5（逗号分隔，逐个传给 -o）", single_line);
  auto input_fork = Input(&fork_limit_text, "例如 16", single_line);

  auto target_radio = Radiobox(&target_entries, &target_mode);
  auto run_radio = Radiobox(&run_entries, &run_mode);
  auto checkbox_names = Checkbox("按主机分行显示输出 (-M)", &show_machine_names);
  auto checkbox_verbose = Checkbox("verbose (-v)", &verbose);
  auto button_run = Button("执行 (Ctrl+R)", RunAction);

  auto form = Container::Vertical({
      target_radio,
      input_machines,
      input_group,
      input_file,
      input_shell,
      input_options,
      run_radio,
      input_fork,
      checkbox_names,
      checkbox_verbose,
  });
  auto bottom = Container::Horizontal({input_command, button_run});
  auto root = Container::Vertical({form, bottom});

  size_t tail_lines = 12;

  auto renderer = Renderer(root, [&] {
    Element target_input;
    switch (static_cast<TargetMode>(target_mode)) {
      case TargetMode::Machines:
        target_input = hbox({text("机器列表: "), input_machines->Render() | flex});
        break;
      case TargetMode::Group:
        target_input = hbox({text("组名: "), input_group->Render() | flex});
        break;
      case TargetMode::All:
        target_input = hbox({text("全部机器 (-a)"), filler()});
        break;
      case TargetMode::File:
        target_input = hbox({text("机器文件: "), input_file->Render() | flex});
        break;
    }

    Elements form_lines;
    form_lines.push_back(text("目标") | bold | color(Color::Cyan));
    form_lines.push_back(target_radio->Render() | yframe);
    form_lines.push_back(target_input);
    form_lines.push_back(separatorEmpty());
    form_lines.push_back(text("远程 shell") | bold | color(Color::Cyan));
    form_lines.push_back(hbox({text("shell: "), input_shell->Render() | flex}));
    form_lines.push_back(hbox({text("选项: "), input_options->Render() | flex}));
    form_lines.push_back(separatorEmpty());
    form_lines.push_back(text("执行模式") | bold | color(Color::Cyan));
    form_lines.push_back(run_radio->Render() | yframe);
    if (static_cast<RunMode>(run_mode) == RunMode::ForkLimit) {
      form_lines.push_back(hbox({text("fork 上限: "), input_fork->Render() | flex}));
    }
    form_lines.push_back(separatorEmpty());
    form_lines.push_back(checkbox_names->Render());
    form_lines.push_back(checkbox_verbose->Render());
    form_lines.push_back(filler());

    Element form_panel = vbox(std::move(form_lines)) | border;

    auto status = output.running
                      ? "运行中"
                      : output.completed ? "已完成" : "就绪";
    Color status_color = output.running ? Color::Yellow
                                        : output.completed
                                              ? (output.exit_code == 0 ? Color::Green : Color::Red)
                                              : Color::Cyan;

    Element header = hbox({
        text(" dsh TUI ") | bold | color(Color::Cyan),
        text("  " + dsh_binary) | dim,
        filler(),
        text(status) | bold | color(status_color),
    });

    std::string current_preview_key =
        std::to_string(target_mode) + "|" + machines + "|" + group + "|" + file;
    if (current_preview_key != preview_key) {
      DshInvocation preview_invocation;
      preview_invocation.target_mode = static_cast<TargetMode>(target_mode);
      preview_invocation.machines = machines;
      preview_invocation.group = group;
      preview_invocation.file = file;
      preview_hosts = ResolveHosts(preview_invocation);
      preview_key = current_preview_key;
    }

    Elements preview;
    preview.push_back(text("目标主机预览 (" + std::to_string(preview_hosts.size()) + " 台)") | bold);
    if (preview_hosts.empty()) {
      preview.push_back(text("（未找到主机列表；dsh 会报告错误）") | dim);
    } else {
      std::string joined;
      for (size_t i = 0; i < preview_hosts.size() && i < 16; ++i) {
        if (i != 0) joined += ", ";
        joined += preview_hosts[i];
      }
      if (preview_hosts.size() > 16) joined += " …";
      preview.push_back(paragraph(joined));
    }

    Element output_panel = vbox({
        hbox({
            text("执行结果") | bold,
            filler(),
            text("PgUp/PgDn 显示行数: " + std::to_string(tail_lines)) | dim,
        }),
        separator(),
        RenderOutputPanel(output, tail_lines, 12),
    }) | border | flex;

    Element main_area = hbox({
        form_panel | size(WIDTH, EQUAL, 42),
        separator(),
        vbox({
            vbox(std::move(preview)) | border,
            output_panel | flex,
        }) | flex,
    }) | flex;

    Element command_row = hbox({
        text("命令: ") | bold,
        input_command->Render() | flex,
        button_run->Render(),
    });

    return vbox({
        header,
        separator(),
        main_area,
        separator(),
        command_row,
        hbox({
            text("Enter 执行  ·  Tab 切换字段  ·  Esc/Ctrl+C 停止  ·  Ctrl+Q 退出") | dim,
            filler(),
            text("dsh_tui " + std::string(kVersion.substr(8))) | dim,
        }),
    }) | border;
  });

  auto main_component = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Custom) {
      DrainRunEvents();
      return true;
    }
    if (event == Event::CtrlR) {
      RunAction();
      return true;
    }
    if (event == Event::CtrlQ) {
      StopRun();
      screen.Exit();
      return true;
    }
    if (event == Event::CtrlC || event == Event::Escape) {
      if (output.running) StopRun();
      return true;
    }
    if (event == Event::PageUp) {
      tail_lines = std::min<size_t>(tail_lines + 10, 200);
      return true;
    }
    if (event == Event::PageDown) {
      tail_lines = tail_lines > 10 ? tail_lines - 10 : 5;
      return true;
    }
    return false;
  });

  input_command->TakeFocus();
  screen.Loop(main_component);

  StopRun();
  if (run_handle.started()) run_handle.stop();
  executor.shutdown(true);
  return 0;
}

}  // namespace

int Main(int argc, char** argv) {
  DshInvocation invocation;
  invocation.dsh_binary = DefaultDshBinary();
  bool self_test = false;
  bool run_once = false;

  auto take_value = [&](int& index) -> std::string {
    if (index + 1 >= argc) {
      std::cerr << "dsh_tui: " << argv[index] << " needs a value\n";
      std::exit(2);
    }
    return argv[++index];
  };

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--dsh") {
      invocation.dsh_binary = take_value(i);
    } else if (arg == "--run-once") {
      run_once = true;
    } else if (arg == "--machines") {
      invocation.target_mode = TargetMode::Machines;
      invocation.machines = take_value(i);
    } else if (arg == "--group") {
      invocation.target_mode = TargetMode::Group;
      invocation.group = take_value(i);
    } else if (arg == "--all") {
      invocation.target_mode = TargetMode::All;
    } else if (arg == "--file") {
      invocation.target_mode = TargetMode::File;
      invocation.file = take_value(i);
    } else if (arg == "--command") {
      invocation.command = take_value(i);
    } else if (arg == "--shell") {
      invocation.remote_shell = take_value(i);
    } else if (arg == "--shell-options") {
      invocation.remote_shell_options = SplitOptions(take_value(i));
    } else if (arg == "--wait") {
      invocation.run_mode = RunMode::Wait;
    } else if (arg == "--fork") {
      invocation.run_mode = RunMode::ForkLimit;
      try { invocation.fork_limit = std::max(0, std::stoi(take_value(i))); } catch (...) {}
    } else if (arg == "--no-names") {
      invocation.show_machine_names = false;
    } else if (arg == "--verbose") {
      invocation.verbose = true;
    } else if (arg == "--self-test") {
      self_test = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else if (arg == "--version" || arg == "-V") {
      std::cout << kVersion << "\n";
      return 0;
    } else {
      std::cerr << "dsh_tui: unknown argument: " << arg << "\n\n";
      PrintUsage();
      return 2;
    }
  }

  if (self_test) return RunSelfTest();
  if (run_once) return RunOnce(invocation);
  return RunTui(invocation.dsh_binary);
}

}  // namespace dsh_tui

int main(int argc, char** argv) {
  return dsh_tui::Main(argc, argv);
}
