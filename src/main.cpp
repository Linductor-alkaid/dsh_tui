#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ftxui/component/app.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include "ds_bridge.hpp"
#include "dsh_launcher.hpp"
#include "ds_protocol.hpp"
#include "ds_state.hpp"
#include "executor/executor.hpp"

using namespace ftxui;

namespace dsh_tui {
namespace {

constexpr std::string_view kVersion = "dsh_tui 0.3.0";

void PrintUsage() {
  std::cout
      << "dsh_tui — DeepSeek Harness WebUI 风格的终端界面\n\n"
      << "直接运行 dsh_tui 时会自动初始化 profile、启动 DeepSeek Harness\n"
      << "并完成桥接；也可以显式使用以下模式：\n"
      << "  dsh_tui [--resume <id>]  # 自启 dsh，恢复指定会话\n"
      << "  dsh_tui --child          # 由 dsh-tui profile 桥接进程启动\n"
      << "  dsh_tui --demo           # 无 dsh 进程的可视化演示\n"
      << "  dsh_tui --self-test      # 无 TTY 的协议/状态自检\n"
      << "  dsh_tui --help\n\n"
      << "请勿把这里的 dsh 与 apt 的 dancer's distributed shell 混淆；\n"
      << "启动脚本使用 `npx @deepseek-ai/dsh`，不会调用 PATH 中的 /usr/bin/dsh。\n";
}

bool FdIsUsable(int fd) {
  int flags = ::fcntl(fd, F_GETFL);
  return flags >= 0 || errno != EBADF;
}

std::string Trim(std::string value) {
  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string ShortId(std::string value, size_t max_chars = 8) {
  if (value.size() <= max_chars) return value;
  return value.substr(0, max_chars) + "…";
}

std::string FormatMs(double value) {
  if (value >= 1000.0) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value / 1000.0 << "s";
    return out.str();
  }
  return std::to_string(static_cast<int64_t>(value)) + "ms";
}

std::string BridgeStatusText(const DeepSeekState& state) {
  if (state.closed) return "✖ 桥接断开";
  if (state.hello_seen) return "● 桥接已连接";
  return "◌ 桥接中…";
}

Element RenderMessage(const ChatMessage& message) {
  Color tint = Color::White;
  std::string label = "·";
  switch (message.role) {
    case MessageRole::Welcome: tint = Color::Cyan; label = "dsh"; break;
    case MessageRole::User: tint = Color::Green; label = "你"; break;
    case MessageRole::Assistant: tint = Color::White; label = "dsh"; break;
    case MessageRole::Tool: tint = Color::Yellow; label = "工具"; break;
    case MessageRole::System: tint = Color::GrayDark; label = "系统"; break;
    case MessageRole::Error: tint = Color::Red; label = "错误"; break;
  }
  Elements lines;
  lines.push_back(text(" " + label + (message.streaming ? " ▍" : "")) | bold | color(tint));
  lines.push_back(paragraph(message.text) | color(tint));
  return vbox(std::move(lines));
}

Element QuestionPanel(const DeepSeekState& state) {
  if (state.ask.active) {
    const Question& question = state.ask.questions[state.ask.index];
    Elements lines;
    if (!question.header.empty()) lines.push_back(text(question.header) | bold | color(Color::Cyan));
    lines.push_back(paragraph(question.question));
    if (!question.detail.empty()) lines.push_back(paragraph(question.detail) | dim);
    if (!question.options.empty()) {
      for (size_t i = 0; i < question.options.size(); ++i) {
        lines.push_back(hbox({
            text("  " + std::to_string(i + 1) + ". ") | color(Color::Cyan),
            text(question.options[i]),
        }));
      }
    }
    lines.push_back(text("输入选项编号或自由回答，Enter 提交") | dim);
    return window(text(" 回答 (" + std::to_string(state.ask.index + 1) + "/" +
                       std::to_string(state.ask.questions.size()) + ") "),
                  vbox(std::move(lines)));
  }
  if (state.approval.active) {
    Elements lines;
    lines.push_back(text("工具: " + state.approval.tool_name) | bold);
    if (!state.approval.reason.empty()) lines.push_back(paragraph(state.approval.reason));
    lines.push_back(text("Enter / y = 允许一次，n = 拒绝") | dim);
    return window(text(" 权限请求 "), vbox(std::move(lines)));
  }
  return emptyElement();
}

Element TodoPanel(const std::vector<TodoItem>& todos) {
  Elements lines;
  if (todos.empty()) {
    lines.push_back(text("（无）") | dim);
  } else {
    for (const auto& todo : todos) {
      std::string mark = todo.status == "completed" ? "✓" : todo.status == "in_progress" ? "▶" : "○";
      Color tint = todo.status == "completed" ? Color::Green
                                               : todo.status == "in_progress" ? Color::Yellow : Color::GrayDark;
      lines.push_back(hbox({text(mark + " ") | color(tint), paragraph(todo.content) | color(tint)}));
    }
  }
  return vbox(std::move(lines));
}

Element StatsPanel(const DeepSeekState& state) {
  const TokenStats& stats = state.stats;
  double pressure = stats.context_window > 0
                        ? 100.0 * static_cast<double>(stats.surface_tokens) /
                              static_cast<double>(stats.context_window)
                        : 0.0;

  Elements lines;
  lines.push_back(text("会话") | bold | color(Color::Cyan));
  lines.push_back(hbox({text("桥接: "),
                        text(BridgeStatusText(state)) | color(state.closed ? Color::Red : state.hello_seen ? Color::Green : Color::Yellow)}));
  if (!state.error.empty()) lines.push_back(paragraph(state.error) | color(Color::Red));
  if (!state.bridge_log.empty()) {
    lines.push_back(separatorEmpty());
    lines.push_back(text("桥接日志") | bold | color(Color::Yellow));
    size_t log_start = state.bridge_log.size() > 4 ? state.bridge_log.size() - 4 : 0;
    for (size_t i = log_start; i < state.bridge_log.size(); ++i) {
      lines.push_back(paragraph(state.bridge_log[i]) | color(Color::Yellow) | dim);
    }
  }
  lines.push_back(hbox({text("状态: "), text(state.running ? "● 运行中" : "○ 空闲") |
                                          color(state.running ? Color::Yellow : Color::Green)}));
  lines.push_back(text("ID: " + ShortId(state.session_id, 14)));
  lines.push_back(text("模型: " + state.provider + " / " + state.model));
  lines.push_back(text("思考深度: " + (state.reasoning_effort.empty() ? "默认" : state.reasoning_effort)));
  lines.push_back(text("目录: " + state.cwd));

  lines.push_back(separatorEmpty());
  lines.push_back(text("Token") | bold | color(Color::Cyan));
  lines.push_back(text("输入: " + std::to_string(stats.input_tokens)));
  lines.push_back(text("输出: " + std::to_string(stats.output_tokens)));
  lines.push_back(text("缓存读: " + std::to_string(stats.cache_read_tokens)));
  lines.push_back(text("缓存写: " + std::to_string(stats.cache_write_tokens)));
  if (stats.context_window > 0) {
    lines.push_back(text("上下文: " + std::to_string(stats.surface_tokens) + " / " +
                         std::to_string(stats.context_window) + " (" +
                         std::to_string(static_cast<int>(pressure)) + "%)"));
  }

  lines.push_back(separatorEmpty());
  lines.push_back(text("会话统计") | bold | color(Color::Cyan));
  lines.push_back(text("回合: " + std::to_string(stats.turns) + " · 步骤: " + std::to_string(stats.steps)));
  lines.push_back(text("LLM: " + FormatMs(stats.llm_ms)));
  lines.push_back(text("工具: " + FormatMs(stats.tool_ms)));
  lines.push_back(text("首 token: " + FormatMs(stats.ttft_ms)));
  lines.push_back(text("解码: " + FormatMs(stats.decode_ms)));

  lines.push_back(separatorEmpty());
  lines.push_back(text("待办") | bold | color(Color::Cyan));
  lines.push_back(TodoPanel(state.todos));
  return vbox(std::move(lines));
}

class DemoReader final : public executor::IBlockingIoWorker {
 public:
  DemoReader(executor::comm::MpscChannel<InboundEvent>* events, ftxui::App* screen)
      : events_(events), screen_(screen) {}

  void run(std::stop_token stop_token) override {
    auto send = [&](InboundEvent event) {
      if (events_ != nullptr) (void)events_->send_for(std::move(event), std::chrono::milliseconds(500));
      if (screen_ != nullptr) screen_->PostEvent(Event::Custom);
    };
    auto wait = [&](int ms) {
      for (int i = 0; i < ms / 25 && !stop_token.stop_requested(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    };

    InboundEvent workspaces;
    workspaces.type = InboundEvent::Type::Workspaces;
    workspaces.workspaces = {
        {"ws1", "/home/linductor/dsh_tui", "dsh_tui", {"s1", "s2"}},
        {"ws2", "/home/linductor/heyaki", "heyaki", {"s3"}},
    };
    send(std::move(workspaces));

    InboundEvent sessions;
    sessions.type = InboundEvent::Type::Sessions;
    sessions.sessions = {
        {"s1", "利用第三方库实现DSH的TUI", "/home/linductor/dsh_tui", "ws1"},
        {"s2", "修复 CI", "/home/linductor/dsh_tui", "ws1"},
        {"s3", "M2 联调", "/home/linductor/heyaki", "ws2"},
    };
    send(std::move(sessions));

    InboundEvent hello;
    hello.type = InboundEvent::Type::Hello;
    hello.text = "s1";
    hello.secondary = "demo-model";
    hello.third = "demo-provider";
    hello.detail = "/home/linductor/dsh_tui";
    hello.flag = true;
    send(std::move(hello));

    InboundEvent history;
    history.type = InboundEvent::Type::History;
    history.history = {
        {"user", "用第三方 TUI 组件做一个和 WebUI 相同视觉框架的 dsh 界面"},
        {"assistant", "我会把会话、工作区、状态和 token 使用都放进三栏布局。"},
    };
    send(std::move(history));

    InboundEvent stats;
    stats.type = InboundEvent::Type::Stats;
    stats.stats = TokenStats{1234, 567, 3456, 0, 1000000, 16000, 2, 5, 3000.0, 1200.0, 800.0, 2200.0};
    send(std::move(stats));

    InboundEvent todo;
    todo.type = InboundEvent::Type::Todo;
    todo.todos = {{"阅读 dsh-tui 协议", "completed"}, {"实现三栏布局", "in_progress"}, {"测试桥接", "pending"}};
    send(std::move(todo));

    InboundEvent status;
    status.type = InboundEvent::Type::Status;
    status.running = true;
    send(status);

    InboundEvent delta;
    delta.type = InboundEvent::Type::Delta;
    delta.text = "text";
    delta.secondary = "这是演示：";
    send(delta);
    wait(250);
    delta.secondary = "左侧是工作区/会话，右侧是状态面板，";
    send(delta);
    wait(250);
    delta.secondary = "中间是流式对话。";
    send(delta);
    wait(400);

    InboundEvent message;
    message.type = InboundEvent::Type::Message;
    message.text = "assistant";
    message.secondary = "这是演示：左侧是工作区/会话，右侧是状态面板，中间是流式对话。";
    send(std::move(message));

    status.running = false;
    send(status);
    wait(2500);

    InboundEvent bye;
    bye.type = InboundEvent::Type::Bye;
    bye.text = "demo finished";
    send(std::move(bye));
    if (events_ != nullptr) events_->close();
  }

  void wakeup() noexcept override {}

 private:
  executor::comm::MpscChannel<InboundEvent>* events_;
  ftxui::App* screen_;
};

int RunSelfTest() {
  std::vector<std::string> samples = {
      R"({"type":"workspaces","workspaces":[{"id":"w","path":"/tmp","title":"demo","sessionIds":["s"]}]})",
      R"({"type":"sessions","sessions":[{"id":"s","title":"标题","cwd":"/tmp","workspaceId":"w"}]})",
      R"({"type":"models","models":[{"provider":"p","id":"m","name":"Model","defaultEffort":"medium","efforts":[{"id":"low","name":"低"},{"id":"medium","name":"中"},{"id":"high","name":"高"}]}]})",
      R"({"type":"hello","sessionId":"s","model":"m","provider":"p","reasoningEffort":"high","cwd":"/tmp","resumed":true})",
      R"({"type":"history","messages":[{"role":"user","text":"你好"},{"role":"assistant","text":"你好！"}]})",
      R"({"type":"status","status":"running"})",
      R"({"type":"delta","part":"text","text":"流式"})",
      R"({"type":"message","role":"assistant","text":"流式完成"})",
      R"({"type":"stats","inputTokens":10,"outputTokens":5,"contextWindow":100,"surfaceTokens":20,"turns":1,"steps":2})",
      R"({"type":"todo","todos":[{"content":"任务一","status":"in_progress"}]})",
      R"({"type":"ask","requestId":"r1","questions":[{"id":"q1","header":"确认","question":"继续吗？","options":["是","否"]}]})",
      R"({"type":"approval","requestId":"r2","toolName":"bash","reason":"执行命令"})",
      R"({"type":"bye","reason":"ok"})",
  };
  DeepSeekState state;
  for (const auto& sample : samples) {
    auto json = Json::parse(sample);
    if (!json.has_value()) { std::cerr << "json parse failed\n"; return 1; }
    auto event = ParseInboundEvent(*json);
    if (!event.has_value()) { std::cerr << "event parse failed\n"; return 1; }
    state.Apply(*event);
  }
  if (state.workspaces.size() != 1 || state.sessions.size() != 1 || !state.resumed ||
      state.models.size() != 1 || state.models[0].efforts.size() != 3 ||
      state.reasoning_effort != "high" ||
      state.stats.context_window != 100 || state.todos.size() != 1 ||
      !state.ask.active || !state.approval.active) {
    std::cerr << "state assertions failed\n";
    return 1;
  }
  OutboundCommand command;
  command.type = "prompt";
  command.text = "a\nb";
  std::string wire = OutboundJson(command);
  if (wire.find("\\n") == std::string::npos || Json::parse(wire) == std::nullopt) {
    std::cerr << "outbound encoding failed\n";
    return 1;
  }
  std::cout << "dsh_tui self-test ok\n";
  return 0;
}

int RunBridgeLoop(int event_fd, int command_fd, const std::string& launch_error = {},
                  bool retry_available = false, bool* retry_requested = nullptr,
                  int stderr_fd = -1) {
  if (std::getenv("DSH_TUI_DEBUG_FDS") != nullptr) {
    std::fprintf(stderr, "RunBridgeLoop event_fd=%d command_fd=%d stderr_fd=%d\n",
                 event_fd, command_fd, stderr_fd);
  }
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

  executor::comm::ChannelOptions channel_options;
  channel_options.capacity = 4096;
  channel_options.drop_policy = executor::comm::DropPolicy::RejectNewest;
  channel_options.name = "dsh-tui-events";
  executor::comm::MpscChannel<InboundEvent> events(channel_options);

  DeepSeekState state;
  std::string input_text;
  if (!launch_error.empty()) {
    state.closed = true;
    state.error = launch_error;
    state.Add(MessageRole::Error, "桥接启动失败: " + launch_error);
  }

  auto screen = ftxui::App::Fullscreen();
  screen.ForceHandleCtrlC(true);

  executor::WorkerHandle worker_handle;
  if (event_fd >= 0) {
    auto worker = std::make_unique<BridgeReader>(event_fd, &events, &screen, stderr_fd);
    executor::BlockingWorkerSpec spec;
    spec.name = "dsh-tui-bridge-reader";
    spec.config.thread_name = "dsh-tui-bridge";
    spec.worker = std::move(worker);
    worker_handle = executor.start_worker(std::move(spec));
    if (!worker_handle.started()) {
      state.closed = true;
      state.error = worker_handle.start_result().message;
      state.Add(MessageRole::Error, "桥接读取线程启动失败: " + state.error);
      if (std::getenv("DSH_TUI_DEBUG_FDS") != nullptr) {
        std::fprintf(stderr, "bridge worker start failed: %s\n", state.error.c_str());
      }
    }
  }

  std::vector<std::string> workspace_entries;
  std::vector<std::string> session_entries;
  std::vector<std::string> model_entries;
  std::vector<std::string> reasoning_entries;
  int workspace_selected = 0;
  int session_selected = 0;
  int model_selected = 0;
  int reasoning_selected = 0;

  auto RebuildWorkspaceMenu = [&] {
    workspace_entries.clear();
    for (const auto& workspace : state.workspaces) {
      workspace_entries.push_back(workspace.title.empty() ? workspace.path : workspace.title);
    }
    if (workspace_selected >= static_cast<int>(workspace_entries.size())) {
      workspace_selected = workspace_entries.empty() ? 0 : static_cast<int>(workspace_entries.size()) - 1;
    }
  };

  auto RebuildSessionMenu = [&] {
    session_entries.clear();
    if (workspace_selected >= 0 && workspace_selected < static_cast<int>(state.workspaces.size())) {
      const auto& workspace = state.workspaces[workspace_selected];
      for (const auto& id : workspace.session_ids) {
        const SessionInfo* session = state.FindSession(id);
        session_entries.push_back(session == nullptr ? ShortId(id, 16) : state.SessionDisplayName(id));
      }
    } else if (state.workspaces.empty()) {
      for (const auto& session : state.sessions) {
        session_entries.push_back(state.SessionDisplayName(session.id));
      }
    }
    if (session_entries.empty()) session_entries.push_back("（无会话）");
    if (session_selected >= static_cast<int>(session_entries.size())) session_selected = 0;
  };

  auto RebuildModelMenu = [&] {
    model_entries.clear();
    for (const auto& model : state.models) {
      model_entries.push_back((model.name.empty() ? model.id : model.name) + "  (" + model.provider + ")");
    }
    if (model_entries.empty()) model_entries.push_back("（读取模型中…）");
    if (model_selected >= static_cast<int>(model_entries.size())) model_selected = 0;
    for (size_t i = 0; i < state.models.size(); ++i) {
      if (state.models[i].provider == state.provider && state.models[i].id == state.model) {
        model_selected = static_cast<int>(i);
        break;
      }
    }
  };
  auto RebuildReasoningMenu = [&] {
    reasoning_entries.clear();
    reasoning_entries.push_back("默认（跟随模型）");
    if (model_selected >= 0 && model_selected < static_cast<int>(state.models.size())) {
      for (const auto& effort : state.models[model_selected].efforts) {
        reasoning_entries.push_back(effort.name.empty() ? effort.id : effort.name);
      }
    }
    if (reasoning_selected >= static_cast<int>(reasoning_entries.size())) reasoning_selected = 0;
    if (!state.reasoning_effort.empty() &&
        model_selected >= 0 && model_selected < static_cast<int>(state.models.size())) {
      const auto& efforts = state.models[model_selected].efforts;
      for (size_t i = 0; i < efforts.size(); ++i) {
        if (efforts[i].id == state.reasoning_effort) {
          reasoning_selected = static_cast<int>(i + 1);
          break;
        }
      }
    } else {
      reasoning_selected = 0;
    }
  };
  RebuildModelMenu();
  RebuildReasoningMenu();

  MenuOption workspace_option = MenuOption::Vertical();
  workspace_option.on_change = [&] { RebuildSessionMenu(); };
  workspace_option.on_enter = [&] { /* selection updates session list */ };
  auto workspace_menu = Menu(&workspace_entries, &workspace_selected, workspace_option);

  MenuOption session_option = MenuOption::Vertical();
  session_option.on_enter = [&] {
    std::string session_id;
    if (workspace_selected >= 0 && workspace_selected < static_cast<int>(state.workspaces.size())) {
      const auto& workspace = state.workspaces[workspace_selected];
      if (session_selected >= 0 && session_selected < static_cast<int>(workspace.session_ids.size())) {
        session_id = workspace.session_ids[session_selected];
      }
    } else if (state.workspaces.empty() &&
               session_selected >= 0 && session_selected < static_cast<int>(state.sessions.size())) {
      session_id = state.sessions[session_selected].id;
    }
    if (session_id.empty()) return;
    OutboundCommand command;
    command.type = "resume-session";
    command.text = session_id;
    SendCommand(command_fd, command);
    input_text.clear();
  };
  auto session_menu = Menu(&session_entries, &session_selected, session_option);

  MenuOption model_option = MenuOption::Vertical();
  model_option.on_change = [&] { RebuildReasoningMenu(); };
  model_option.on_enter = [&] {
    if (model_selected < 0 || model_selected >= static_cast<int>(state.models.size())) return;
    const ModelInfo& model = state.models[model_selected];
    OutboundCommand command;
    command.type = "set-model";
    command.text = model.provider + "|" + model.id;
    SendCommand(command_fd, command);
  };
  auto model_menu = Menu(&model_entries, &model_selected, model_option);

  MenuOption reasoning_option = MenuOption::Vertical();
  reasoning_option.on_enter = [&] {
    OutboundCommand command;
    command.type = "set-reasoning";
    if (reasoning_selected <= 0) {
      command.text = "default";
    } else if (model_selected >= 0 && model_selected < static_cast<int>(state.models.size())) {
      const auto& efforts = state.models[model_selected].efforts;
      size_t index = static_cast<size_t>(reasoning_selected - 1);
      if (index < efforts.size()) command.text = efforts[index].id;
    }
    if (command.text.empty()) return;
    SendCommand(command_fd, command);
  };
  auto reasoning_menu = Menu(&reasoning_entries, &reasoning_selected, reasoning_option);

  auto Send = [&](const OutboundCommand& command) {
    if (!SendCommand(command_fd, command)) {
      state.Add(MessageRole::Error, "无法写入 dsh 桥接进程，连接可能已断开。");
    }
  };

  auto DrainEvents = [&] {
    InboundEvent event;
    while (events.try_receive(event)) {
      bool was_closed = state.closed;
      state.Apply(event);
      if (!was_closed && state.closed &&
          (event.type == InboundEvent::Type::Bye || event.type == InboundEvent::Type::Error)) {
        std::string reason = state.error.empty()
                                 ? (state.bridge_log.empty() ? "无桥接日志" : state.bridge_log.back())
                                 : state.error;
        state.Add(MessageRole::Error, "桥接已断开：" + reason);
      }
      if (event.type == InboundEvent::Type::Workspaces || event.type == InboundEvent::Type::Sessions) {
        RebuildWorkspaceMenu();
        RebuildSessionMenu();
      }
      if (event.type == InboundEvent::Type::Models || event.type == InboundEvent::Type::Hello) {
        RebuildModelMenu();
        RebuildReasoningMenu();
        if (event.type == InboundEvent::Type::Hello) {
          for (size_t i = 0; i < state.models.size(); ++i) {
            if (state.models[i].provider == state.provider && state.models[i].id == state.model) {
              model_selected = static_cast<int>(i);
              break;
            }
          }
        }
      }
      if (event.type == InboundEvent::Type::Bye) {
        state.closed = true;
        // Interactive terminals keep the TUI open so the status bar can show
        // the disconnected state; non-interactive test runners exit so bridge
        // tests do not hang after the transport closes.
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) screen.Exit();
      }
    }
    if (events.is_closed() && events.empty() && !state.closed) {
      state.closed = true;
      std::string reason = state.error.empty()
                               ? (state.bridge_log.empty() ? "无桥接日志" : state.bridge_log.back())
                               : state.error;
      state.Add(MessageRole::Error, "桥接已断开：" + reason);
      if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) screen.Exit();
    }
  };

  auto AnswerCurrentQuestion = [&] {
    if (!state.ask.active || state.ask.index >= state.ask.questions.size()) return;
    const Question& question = state.ask.questions[state.ask.index];
    std::string answer = Trim(input_text);

    OutboundCommand command;
    command.type = "answer";
    command.request_id = state.ask.request_id;
    command.item_id = question.id;

    if (!question.options.empty()) {
      bool matched = false;
      if (!answer.empty()) {
        char* end = nullptr;
        long choice = std::strtol(answer.c_str(), &end, 10);
        if (end != answer.c_str() && *end == '\0' && choice >= 1 &&
            static_cast<size_t>(choice) <= question.options.size()) {
          command.selected = {question.options[static_cast<size_t>(choice - 1)]};
          matched = true;
        } else {
          for (const auto& option : question.options) {
            if (option == answer) { command.selected = {option}; matched = true; break; }
          }
        }
      }
      if (!matched) {
        if (answer.empty()) command.selected = {question.options.front()};
        else command.custom = answer;
      }
    } else if (!answer.empty()) {
      command.custom = answer;
    }

    ++state.ask.index;
    if (state.ask.index >= state.ask.questions.size()) state.ask.active = false;
    Send(command);
  };

  auto AnswerApproval = [&] {
    if (!state.approval.active) return;
    std::string answer = Trim(input_text);
    OutboundCommand command;
    command.type = "approval";
    command.request_id = state.approval.request_id;
    command.text = (answer == "n" || answer == "N" || answer == "no" ||
                    answer == "拒绝" || answer == "reject") ? "rejected" : "allowed-once";
    state.approval.active = false;
    Send(command);
  };

  auto NewSessionInWorkspace = [&] {
    OutboundCommand command;
    command.type = "new-session";
    if (workspace_selected >= 0 && workspace_selected < static_cast<int>(state.workspaces.size())) {
      command.text = state.workspaces[workspace_selected].id;
    } else {
      command.text = "";
    }
    Send(command);
    input_text.clear();
  };

  bool stick_to_bottom = true;
  double scroll_anchor = 1.0;

  auto Submit = [&] {
    if (state.closed || !state.hello_seen) return;
    if (state.approval.active) AnswerApproval();
    else if (state.ask.active) AnswerCurrentQuestion();
    else {
      std::string prompt = Trim(input_text);
      if (prompt.empty()) return;
      OutboundCommand command;
      command.type = "prompt";
      command.text = std::move(prompt);
      Send(command);
    }
    input_text.clear();
    stick_to_bottom = true;
    scroll_anchor = 1.0;
  };

  InputOption input_option = InputOption::Spacious();
  input_option.multiline = false;
  input_option.on_enter = Submit;
  auto input_component = Input(&input_text, "输入消息，Enter 发送", input_option);
  // Intercept history-scrolling keys before the text input consumes them.
  input_component |= CatchEvent([&](Event event) {
    if (event == Event::Home) { stick_to_bottom = false; scroll_anchor = 0.0; return true; }
    if (event == Event::End) { stick_to_bottom = true; scroll_anchor = 1.0; return true; }
    if (event == Event::PageUp) { stick_to_bottom = false; scroll_anchor = std::max(0.0, scroll_anchor - 0.12); return true; }
    if (event == Event::PageDown) {
      scroll_anchor = std::min(1.0, scroll_anchor + 0.12);
      if (scroll_anchor >= 0.999) { stick_to_bottom = true; scroll_anchor = 1.0; }
      return true;
    }
    return false;
  });
  auto new_session_button = Button("＋ 新建会话", NewSessionInWorkspace, ButtonOption::Ascii());

  auto sidebar_container = Container::Vertical({workspace_menu, new_session_button, session_menu, model_menu, reasoning_menu});
  auto main_container = Container::Vertical({input_component});

  // User-resizable panes. The terminal can still hide a rail automatically on
  // narrow windows; widening it brings the last non-zero size back.
  int sidebar_width = 34;
  int status_width = 38;
  bool sidebar_visible = true;
  bool status_visible = true;

  auto BuildSidebar = [&] {
    Elements sidebar;
    sidebar.push_back(text("工作区") | bold | color(Color::Cyan));
    if (state.workspaces.empty()) {
      sidebar.push_back(text("（等待 dsh 工作区数据…）") | dim);
    } else {
      sidebar.push_back(workspace_menu->Render() | yframe);
      sidebar.push_back(text(" " + state.workspaces[workspace_selected].path) | dim);
    }
    sidebar.push_back(separatorEmpty());
    sidebar.push_back(new_session_button->Render());
    sidebar.push_back(separatorEmpty());
    sidebar.push_back(text("会话") | bold | color(Color::Cyan));
    sidebar.push_back(session_menu->Render() | yframe);
    sidebar.push_back(separatorEmpty());
    sidebar.push_back(text("模型") | bold | color(Color::Cyan));
    sidebar.push_back(model_menu->Render() | yframe);
    sidebar.push_back(separatorEmpty());
    sidebar.push_back(text("思考深度") | bold | color(Color::Cyan));
    sidebar.push_back(reasoning_menu->Render() | yframe);
    sidebar.push_back(filler());
    return vbox(std::move(sidebar)) | yframe | border;
  };

  auto sidebar_pane = Renderer(sidebar_container, [&] { return BuildSidebar(); });

  auto status_pane = Renderer([&] {
    return StatsPanel(state) | yframe | border;
  });

  auto BuildMain = [&] {
    Elements messages;
    for (size_t i = 0; i < state.messages.size(); ++i) {
      messages.push_back(RenderMessage(state.messages[i]));
      if (i + 1 < state.messages.size()) messages.push_back(separatorEmpty());
    }
    if (messages.empty()) messages.push_back(text("（暂无消息）") | dim | center);

    const float anchor = stick_to_bottom ? 1.0f : static_cast<float>(scroll_anchor);
    Element history = vbox(std::move(messages)) | focusPositionRelative(0.0f, anchor) |
                      yframe | vscroll_indicator | flex;

    Element question_panel = QuestionPanel(state);
    Elements main_lines;
    if (status_width > 0) {
      main_lines.push_back(hbox({
          text(" DeepSeek Harness ") | bold | color(Color::Cyan),
          filler(),
          text(BridgeStatusText(state)) |
              color(state.closed ? Color::Red : state.hello_seen ? Color::Green : Color::Yellow),
          filler(),
          text(state.running ? "● 运行中" : "○ 空闲") |
              color(state.running ? Color::Yellow : Color::Green),
          filler(),
          text(state.session_id.empty() ? "未连接" : "session " + ShortId(state.session_id)) | dim,
      }));
    } else {
      std::string compact = BridgeStatusText(state);
      if (!state.model.empty()) compact += " · " + state.model;
      if (!state.reasoning_effort.empty()) compact += " · 思考 " + state.reasoning_effort;
      main_lines.push_back(hbox({
          text(" DeepSeek Harness ") | bold | color(Color::Cyan),
          filler(),
          text(compact) |
              color(state.closed ? Color::Red : state.hello_seen ? Color::Green : Color::Yellow),
          filler(),
          text(state.session_id.empty() ? "未连接" : "session " + ShortId(state.session_id)) | dim,
      }));
    }
    main_lines.push_back(separator());
    main_lines.push_back(history);
    if (state.ask.active || state.approval.active) {
      main_lines.push_back(separator());
      main_lines.push_back(question_panel);
    }
    main_lines.push_back(separator());
    main_lines.push_back(hbox({
        text("❯ ") | bold | color(Color::Cyan),
        input_component->Render() | flex,
    }));
    main_lines.push_back(hbox({
        text(std::string("Enter 发送 · Esc 停止 · Ctrl+N 新建 · PgUp/PgDn/Home/End 历史") +
                 (retry_available ? " · Ctrl+R 重连" : "") + " · Ctrl+Q 退出") | dim,
        filler(),
        text(BridgeStatusText(state)) |
            color(state.closed ? Color::Red : state.hello_seen ? Color::Green : Color::Yellow),
        text("  ·  dsh_tui " + std::string(kVersion.substr(8))) | dim,
    }));
    return vbox(std::move(main_lines)) | border | flex;
  };

  auto main_pane = Renderer(main_container, [&] { return BuildMain(); });

  // [sidebar | splitter | main | splitter | status]
  auto center = main_pane;
  ResizableSplitOption sidebar_option;
  sidebar_option.main = Maybe(sidebar_pane, &sidebar_visible);
  sidebar_option.back = center;
  sidebar_option.direction = Direction::Left;
  sidebar_option.main_size = &sidebar_width;
  sidebar_option.min = 0;
  sidebar_option.max = 60;
  sidebar_option.separator_func = [&] {
    return sidebar_visible ? separatorDouble() | color(Color::Cyan) : emptyElement();
  };
  center = ResizableSplit(std::move(sidebar_option));

  ResizableSplitOption status_option;
  status_option.main = Maybe(status_pane, &status_visible);
  status_option.back = center;
  status_option.direction = Direction::Right;
  status_option.main_size = &status_width;
  status_option.min = 0;
  status_option.max = 60;
  status_option.separator_func = [&] {
    return status_visible ? separatorDouble() | color(Color::Cyan) : emptyElement();
  };
  center = ResizableSplit(std::move(status_option));

  auto renderer = Renderer(center, [&] {
    const int width = screen.dimx();
    // Auto-hide rails below the thresholds, but only touch the sizes on a
    // visibility transition so a user's drag is never overwritten frame to
    // frame.
    const bool want_sidebar = width >= 110;
    const bool want_status = width >= 150;
    const bool was_sidebar = sidebar_visible;
    const bool was_status = status_visible;
    if (want_sidebar) {
      if (!was_sidebar) sidebar_width = 34;
    } else {
      sidebar_width = 0;
    }
    if (want_status) {
      if (!was_status) status_width = 38;
    } else {
      status_width = 0;
    }
    sidebar_visible = want_sidebar;
    status_visible = want_status;
    return center->Render() | border;
  });
  auto main_component = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Custom) { DrainEvents(); return true; }
    if (event == Event::CtrlQ) {
      OutboundCommand command; command.type = "quit"; Send(command); screen.Exit(); return true;
    }
    if (event == Event::CtrlC || event == Event::Escape) {
      if (state.running) { OutboundCommand command; command.type = "cancel"; Send(command); }
      return true;
    }
    if (event == Event::CtrlN) { NewSessionInWorkspace(); return true; }
    if (event == Event::CtrlR && retry_available && (state.closed || !state.hello_seen)) {
      if (retry_requested != nullptr) *retry_requested = true;
      screen.Exit();
      return true;
    }
    if (event == Event::PageUp) {
      stick_to_bottom = false;
      scroll_anchor = std::max(0.0, scroll_anchor - 0.12);
      return true;
    }
    if (event == Event::PageDown) {
      scroll_anchor = std::min(1.0, scroll_anchor + 0.12);
      if (scroll_anchor >= 0.999) { stick_to_bottom = true; scroll_anchor = 1.0; }
      return true;
    }
    if (event == Event::Home) {
      stick_to_bottom = false;
      scroll_anchor = 0.0;
      return true;
    }
    if (event == Event::End) {
      stick_to_bottom = true;
      scroll_anchor = 1.0;
      return true;
    }
    return false;
  });

  DrainEvents();
  input_component->TakeFocus();
  screen.Loop(main_component);

  if (worker_handle.started()) {
    worker_handle.request_stop();
    worker_handle.stop();
  }
  executor.shutdown(true);
  return 0;
}

int RunChildMode() {
  if (!FdIsUsable(kEventFd) || !FdIsUsable(kCommandFd)) {
    std::cerr << "dsh_tui: child mode requires protocol pipes on fd 3 and fd 4.\n"
              << "Run it through `npx @deepseek-ai/dsh --profile tui`, or use --demo / --self-test.\n";
    return 2;
  }
  return RunBridgeLoop(kEventFd, kCommandFd);
}

int RunStandaloneMode(const std::string& resume_session_id) {
  bool retry_requested = false;
  int code = 0;
  do {
    retry_requested = false;
    std::string error;
    if (!EnsureTuiProfile(error)) {
      code = RunBridgeLoop(-1, -1, error, true, &retry_requested);
      continue;
    }
    BridgeProcess process = SpawnDeepSeekBridge(resume_session_id, error);
    if (process.pid <= 0) {
      code = RunBridgeLoop(-1, -1, error, true, &retry_requested);
      continue;
    }
    code = RunBridgeLoop(process.event_fd, process.command_fd, "", true,
                         &retry_requested, process.stderr_fd);
    ReapBridgeProcess(process);
  } while (retry_requested);
  return code;
}

int RunDemoMode() {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::cout << "dsh_tui demo (no TTY):\n"
              << "工作区: dsh_tui (/home/linductor/dsh_tui)\n"
              << "会话: 利用第三方库实现DSH的TUI\n"
              << "[你] 用第三方 TUI 组件做一个和 WebUI 相同视觉框架的 dsh 界面\n"
              << "[dsh] 我会把会话、工作区、状态和 token 使用都放进三栏布局。\n";
    return 0;
  }

  executor::Executor executor;
  executor::ExecutorConfig executor_config;
  executor_config.min_threads = 1;
  executor_config.max_threads = 2;
  if (!executor.initialize(executor_config)) { std::cerr << "executor init failed\n"; return 2; }

  executor::comm::ChannelOptions options;
  options.capacity = 256;
  options.name = "dsh-tui-demo";
  executor::comm::MpscChannel<InboundEvent> events(options);
  DeepSeekState state;
  auto screen = ftxui::App::Fullscreen();
  screen.ForceHandleCtrlC(true);

  auto worker = std::make_unique<DemoReader>(&events, &screen);
  executor::BlockingWorkerSpec spec;
  spec.name = "dsh-tui-demo";
  spec.config.thread_name = "dsh-tui-demo";
  spec.worker = std::move(worker);
  executor::WorkerHandle handle = executor.start_worker(std::move(spec));
  if (!handle.started()) { std::cerr << handle.start_result().message << "\n"; return 2; }

  auto renderer = Renderer([&] {
    InboundEvent event;
    while (events.try_receive(event)) {
      state.Apply(event);
      if (event.type == InboundEvent::Type::Bye) screen.Exit();
    }
    Elements messages;
    size_t start = state.messages.size() > 40 ? state.messages.size() - 40 : 0;
    for (size_t i = start; i < state.messages.size(); ++i) messages.push_back(RenderMessage(state.messages[i]));
    return hbox({
        vbox({text("工作区") | bold, text(state.workspaces.empty() ? "-" : state.workspaces[0].title)}) |
            border | size(WIDTH, EQUAL, 30),
        separator(),
        vbox({text(" DeepSeek Harness demo ") | bold | center | color(Color::Cyan),
              separator(),
              vbox(std::move(messages)) | vscroll_indicator | yframe | flex,
              separator(),
              text("Ctrl+Q 退出") | dim | center}) | border | flex,
        separator(),
        StatsPanel(state) | border | size(WIDTH, EQUAL, 38),
    }) | border;
  });
  auto component = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Custom) {
      InboundEvent message;
      while (events.try_receive(message)) {
        state.Apply(message);
        if (message.type == InboundEvent::Type::Bye) screen.Exit();
      }
      return true;
    }
    if (event == Event::CtrlQ || event == Event::Escape) { screen.Exit(); return true; }
    return false;
  });
  screen.Loop(component);
  handle.request_stop(); handle.stop(); executor.shutdown(true);
  return 0;
}

}  // namespace

int Main(int argc, char** argv) {
  bool child_mode = false;
  bool demo_mode = false;
  bool self_test = false;
  std::string resume_session_id;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--child") child_mode = true;
    else if (arg == "--demo") demo_mode = true;
    else if (arg == "--self-test") self_test = true;
    else if (arg == "--resume" && i + 1 < argc) resume_session_id = argv[++i];
    else if (arg == "--help" || arg == "-h") { PrintUsage(); return 0; }
    else if (arg == "--version" || arg == "-V") { std::cout << kVersion << "\n"; return 0; }
    else { std::cerr << "dsh_tui: unknown argument: " << arg << "\n\n"; PrintUsage(); return 2; }
  }
  if (self_test) return RunSelfTest();
  if (demo_mode) return RunDemoMode();
  if (child_mode) return RunChildMode();
  return RunStandaloneMode(resume_session_id);
}

}  // namespace dsh_tui

int main(int argc, char** argv) {
  return dsh_tui::Main(argc, argv);
}
