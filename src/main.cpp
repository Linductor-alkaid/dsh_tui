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
#include <set>
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
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/box.hpp"

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

enum class SlashSubmitKind { Execute, Fill, Prompt };
struct SlashSubmitDecision {
  SlashSubmitKind kind = SlashSubmitKind::Prompt;
  std::string line;
  std::string fill;
};

std::vector<size_t> RankSlashCommands(const std::vector<CommandInfo>& commands,
                                      const std::string& query) {
  std::vector<size_t> matches;
  std::string lower = query;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower.empty()) {
    for (size_t i = 0; i < commands.size(); ++i) matches.push_back(i);
    return matches;
  }
  auto is_subsequence = [](const std::string& q, const std::string& value) {
    size_t pos = 0;
    for (char c : value) if (pos < q.size() && c == q[pos]) ++pos;
    return pos == q.size();
  };
  std::vector<std::pair<int, size_t>> ranked;
  for (size_t i = 0; i < commands.size(); ++i) {
    const std::string& candidate = commands[i].name;
    bool prefix = candidate.substr(0, lower.size()) == lower;
    if (!prefix && !is_subsequence(lower, candidate)) continue;
    ranked.push_back({(prefix ? 0 : 1000) + static_cast<int>(candidate.size()), i});
  }
  std::sort(ranked.begin(), ranked.end());
  for (const auto& [score, index] : ranked) matches.push_back(index);
  return matches;
}

SlashSubmitDecision DecideSlashSubmit(const std::string& input,
                                      const std::vector<CommandInfo>& commands,
                                      int selected_match) {
  SlashSubmitDecision decision;
  if (input.empty() || input[0] != '/') return decision;
  std::string name = input.substr(1);
  size_t space = name.find_first_of(" \t\r\n");
  if (space != std::string::npos) name.resize(space);

  const CommandInfo* exact = nullptr;
  for (const auto& command : commands) if (command.name == name) { exact = &command; break; }

  if (exact != nullptr) {
    std::string after = input.substr(exact->name.size() + 1);
    bool no_separator = after.empty();
    bool separator_only =
        !after.empty() && after.find_first_not_of(" \t\r\n") == std::string::npos;
    if (no_separator && !exact->hint.empty()) {
      decision.kind = SlashSubmitKind::Fill;
      decision.fill = "/" + exact->name + " ";
      return decision;
    }
    if (((no_separator || separator_only) && exact->hint.empty()) ||
        (!no_separator && !exact->hint.empty())) {
      decision.kind = SlashSubmitKind::Execute;
      decision.line = input;
      return decision;
    }
    decision.kind = SlashSubmitKind::Prompt;
    decision.line = input;
    return decision;
  }

  auto matches = RankSlashCommands(commands, name);
  if (!matches.empty()) {
    if (selected_match < 0 || selected_match >= static_cast<int>(matches.size())) selected_match = 0;
    const CommandInfo& suggestion = commands[matches[selected_match]];
    if (suggestion.hint.empty()) {
      decision.kind = SlashSubmitKind::Execute;
      decision.line = "/" + suggestion.name;
    } else {
      decision.kind = SlashSubmitKind::Fill;
      decision.fill = "/" + suggestion.name + " ";
    }
    return decision;
  }

  decision.kind = SlashSubmitKind::Prompt;
  decision.line = input;
  return decision;
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

Element RenderMessage(const ChatMessage& message, size_t index = 0,
                        const std::set<size_t>* expanded_reasoning = nullptr,
                        size_t active_reasoning = std::string::npos,
                        ftxui::Box* reasoning_click_box = nullptr) {
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
  if (!message.reasoning.empty()) {
    const bool expanded = expanded_reasoning != nullptr && expanded_reasoning->count(index) != 0;
    const bool active = active_reasoning == index;
    Element reasoning_header = hbox({
        text(expanded ? "  ▾ 思考过程" : "  ▸ 思考过程") |
            bold | color(active ? Color::Cyan : Color::GrayDark),
        text("  " + std::to_string(message.reasoning.size()) + " 字") | dim,
        active ? text("  ←") | color(Color::Cyan) : text(""),
        filler(),
    });
    if (reasoning_click_box != nullptr) reasoning_header = reasoning_header | reflect(*reasoning_click_box);
    lines.push_back(reasoning_header);
    if (expanded) {
      lines.push_back(paragraph(message.reasoning) | dim | border);
    }
  }
  lines.push_back(paragraph(message.text) | color(tint));
  return vbox(std::move(lines));
}

Element BuildCommandPaletteElement(const std::vector<CommandInfo>& commands,
                                      const std::vector<size_t>& matches,
                                      int selected,
                                      std::vector<ftxui::Box>& boxes) {
  if (matches.empty()) {
    return window(text(" 命令 "), text("未知命令") | color(Color::Red));
  }
  boxes.assign(matches.size(), ftxui::Box{});
  Elements rows;
  for (size_t i = 0; i < matches.size(); ++i) {
    const CommandInfo& command = commands[matches[i]];
    const bool is_selected = static_cast<int>(i) == selected;
    Element row = hbox({
        text(is_selected ? "❯ /" + command.name : "  /" + command.name) |
            bold | color(is_selected ? Color::Cyan : Color::White),
        text(command.hint.empty() ? "" : " " + command.hint) | dim,
        filler(),
    });
    row = row | reflect(boxes[i]);
    rows.push_back(row);
    rows.push_back(paragraph("    " + command.description) | dim);
  }
  return window(text(" 命令 "), vbox(std::move(rows)));
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
        {"user", "用第三方 TUI 组件做一个和 WebUI 相同视觉框架的 dsh 界面", ""},
        {"assistant", "我会把会话、工作区、状态和 token 使用都放进三栏布局。", "这里是一段被折叠的思考过程，按 Ctrl+E 可以展开。"},
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
      R"({"type":"presets","presets":[{"id":"standard","name":"标准模式","description":"完整"},{"id":"code","name":"PTC 模式","description":"Code Mode"}]})",
      R"({"type":"permissions","permissions":[{"id":"read-only","name":"read-only","description":""},{"id":"workspace-write","name":"workspace-write","description":""},{"id":"danger-full-access","name":"danger-full-access","description":""}]})",
      R"({"type":"commands","commands":[{"name":"compact","description":"Compact older conversation history","hint":""},{"name":"goal","description":"set or view the goal","hint":"[objective]"}]})",
      R"({"type":"permission","id":"workspace-write","name":"workspace-write"})",
      R"({"type":"preset","id":"code","name":"PTC 模式"})",
      R"({"type":"hello","sessionId":"s","model":"m","provider":"p","reasoningEffort":"high","presetId":"code","cwd":"/tmp","resumed":true})",
      R"({"type":"history","messages":[{"role":"user","text":"你好"},{"role":"assistant","text":"你好！","reasoning":"思考过程"}]})",
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
      state.presets.size() != 2 || state.permissions.size() != 3 ||
      state.commands.size() != 2 || state.commands[0].name != "compact" ||
      state.commands[1].name != "goal" ||
      state.permission_id != "workspace-write" || state.preset_id != "code" ||
      state.preset_name != "PTC 模式" || state.reasoning_effort != "high" ||
      state.messages.empty() ||
      std::none_of(state.messages.begin(), state.messages.end(), [](const auto& m) {
        return m.reasoning == "思考过程";
      }) ||
      state.stats.context_window != 100 || state.todos.size() != 1 ||
      !state.ask.active || !state.approval.active) {
    std::cerr << "state assertions failed\n";
    return 1;
  }
  std::vector<CommandInfo> slash_commands = {
      {"compact", "Compact older conversation history", ""},
      {"feedback", "record feedback", "<text>"},
      {"goal", "set or view the goal", "[objective]"},
  };
  std::vector<ftxui::Box> palette_boxes;
  ftxui::Element palette =
      BuildCommandPaletteElement(slash_commands, {0, 1, 2}, 0, palette_boxes);
  ftxui::Screen palette_screen(70, 10);
  ftxui::Render(palette_screen, palette);
  std::string palette_text = palette_screen.ToString();
  if (palette_text.find("/goal") == std::string::npos ||
      palette_text.find("set or view the goal") == std::string::npos) {
    std::cerr << "command palette rendering assertions failed\n";
    return 1;
  }

  if (DecideSlashSubmit("/goal", slash_commands, 0).kind != SlashSubmitKind::Fill ||
      DecideSlashSubmit("/goal", slash_commands, 0).fill != "/goal " ||
      DecideSlashSubmit("/goal ", slash_commands, 0).kind != SlashSubmitKind::Execute ||
      DecideSlashSubmit("/goal clear", slash_commands, 0).kind != SlashSubmitKind::Execute ||
      DecideSlashSubmit("/compact", slash_commands, 0).kind != SlashSubmitKind::Execute ||
      DecideSlashSubmit("/compact extra", slash_commands, 0).kind != SlashSubmitKind::Prompt ||
      DecideSlashSubmit("/unknown", slash_commands, 0).kind != SlashSubmitKind::Prompt ||
      RankSlashCommands(slash_commands, "g").size() != 1 ||
      RankSlashCommands(slash_commands, "g")[0] != 2) {
    std::cerr << "slash decision assertions failed\n";
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
  std::string input_placeholder = "输入消息，Enter 发送";
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
  std::vector<std::string> preset_entries;
  std::vector<std::string> permission_entries;
  int workspace_selected = 0;
  int session_selected = 0;
  int model_selected = 0;
  int reasoning_selected = 0;
  int preset_selected = 0;
  int permission_selected = 0;

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
    if (!state.session_id.empty() && !state.workspaces.empty() &&
        workspace_selected >= 0 && workspace_selected < static_cast<int>(state.workspaces.size())) {
      const auto& ids = state.workspaces[workspace_selected].session_ids;
      for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == state.session_id) { session_selected = static_cast<int>(i); break; }
      }
    } else if (!state.session_id.empty() && state.workspaces.empty()) {
      for (size_t i = 0; i < state.sessions.size(); ++i) {
        if (state.sessions[i].id == state.session_id) { session_selected = static_cast<int>(i); break; }
      }
    }
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
  auto RebuildPresetMenu = [&] {
    preset_entries.clear();
    for (const auto& preset : state.presets) {
      preset_entries.push_back(preset.name.empty() ? preset.id : preset.name);
    }
    if (preset_entries.empty()) preset_entries.push_back("（读取模式中…）");
    if (preset_selected >= static_cast<int>(preset_entries.size())) preset_selected = 0;
    for (size_t i = 0; i < state.presets.size(); ++i) {
      if (state.presets[i].id == state.preset_id) { preset_selected = static_cast<int>(i); break; }
    }
  };
  auto RebuildPermissionMenu = [&] {
    permission_entries.clear();
    for (const auto& permission : state.permissions) {
      std::string label = permission.name;
      if (permission.id == "read-only") label = "只读 (read-only)";
      else if (permission.id == "workspace-write") label = "工作区写入 (workspace-write)";
      else if (permission.id == "danger-full-access") label = "完全访问 (full access)";
      if (label.empty()) label = permission.id;
      permission_entries.push_back(label);
    }
    if (permission_entries.empty()) permission_entries.push_back("（读取权限中…）");
    if (permission_selected >= static_cast<int>(permission_entries.size())) permission_selected = 0;
    for (size_t i = 0; i < state.permissions.size(); ++i) {
      if (state.permissions[i].id == state.permission_id) { permission_selected = static_cast<int>(i); break; }
    }
  };
  RebuildModelMenu();
  RebuildReasoningMenu();
  RebuildPresetMenu();
  RebuildPermissionMenu();

  MenuOption workspace_option = MenuOption::Vertical();
  workspace_option.on_change = [&] { RebuildSessionMenu(); };
  workspace_option.on_enter = [&] { /* selection updates session list */ };
  auto workspace_menu = Menu(&workspace_entries, &workspace_selected, workspace_option);

  auto ResumeSelectedSession = [&] {
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

  MenuOption session_option = MenuOption::Vertical();
  session_option.on_change = [&] { ResumeSelectedSession(); };
  session_option.on_enter = [&] { ResumeSelectedSession(); };
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

  MenuOption preset_option = MenuOption::Vertical();
  preset_option.on_enter = [&] {
    if (preset_selected < 0 || preset_selected >= static_cast<int>(state.presets.size())) return;
    OutboundCommand command;
    command.type = "set-preset";
    command.text = state.presets[preset_selected].id;
    SendCommand(command_fd, command);
  };
  auto preset_menu = Menu(&preset_entries, &preset_selected, preset_option);

  MenuOption permission_option = MenuOption::Vertical();
  permission_option.on_enter = [&] {
    if (permission_selected < 0 || permission_selected >= static_cast<int>(state.permissions.size())) return;
    OutboundCommand command;
    command.type = "set-permission";
    command.text = state.permissions[permission_selected].id;
    SendCommand(command_fd, command);
  };
  auto permission_menu = Menu(&permission_entries, &permission_selected, permission_option);

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
      if (event.type == InboundEvent::Type::Presets || event.type == InboundEvent::Type::Preset) {
        RebuildPresetMenu();
      }
      if (event.type == InboundEvent::Type::Permissions || event.type == InboundEvent::Type::Permission) {
        RebuildPermissionMenu();
      }
      if (event.type == InboundEvent::Type::Models || event.type == InboundEvent::Type::Hello) {
        RebuildModelMenu();
        RebuildReasoningMenu();
        if (event.type == InboundEvent::Type::Hello) {
          RebuildWorkspaceMenu();
          RebuildSessionMenu();
          RebuildPresetMenu();
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
  std::set<size_t> expanded_reasoning;
  size_t active_reasoning = std::string::npos;
  std::vector<ftxui::Box> reasoning_click_boxes;
  std::vector<size_t> command_matches;
  int command_match_selected = -1;
  std::string last_command_query;
  std::vector<ftxui::Box> command_palette_boxes;
  bool slash_permission_popup = false;
  int slash_permission_selected = -1;
  std::vector<ftxui::Box> slash_permission_boxes;

  auto MoveActiveReasoning = [&](int direction) {
    if (state.messages.empty()) return;
    size_t cursor = active_reasoning;
    if (cursor == std::string::npos) cursor = direction > 0 ? 0 : state.messages.size() - 1;
    for (size_t step = 0; step < state.messages.size(); ++step) {
      cursor = (cursor + state.messages.size() + direction) % state.messages.size();
      if (!state.messages[cursor].reasoning.empty()) {
        active_reasoning = cursor;
        return;
      }
    }
    active_reasoning = std::string::npos;
  };

  auto ToggleActiveReasoning = [&] {
    if (active_reasoning == std::string::npos || active_reasoning >= state.messages.size() ||
        state.messages[active_reasoning].reasoning.empty()) {
      // Select the most recent thinking block when none is selected.
      for (size_t i = state.messages.size(); i > 0; --i) {
        if (!state.messages[i - 1].reasoning.empty()) {
          active_reasoning = i - 1;
          break;
        }
      }
      if (active_reasoning == std::string::npos) return;
    }
    if (expanded_reasoning.count(active_reasoning) != 0) expanded_reasoning.erase(active_reasoning);
    else expanded_reasoning.insert(active_reasoning);
  };

  auto SlashActive = [&] { return !input_text.empty() && input_text[0] == '/'; };
  auto SlashPaletteActive = [&] {
    return SlashActive() && input_text.find_first_of(" \t\r\n") == std::string::npos;
  };

  auto CommandNameFromInput = [&]() -> std::string {
    if (!SlashActive()) return {};
    std::string rest = input_text.substr(1);
    size_t space = rest.find_first_of(" \t\r\n");
    if (space == std::string::npos) return rest;
    return rest.substr(0, space);
  };

  auto RefreshCommandMatches = [&] {
    command_matches.clear();
    command_palette_boxes.clear();
    if (!SlashPaletteActive()) {
      command_match_selected = -1;
      last_command_query.clear();
      return;
    }
    std::string name = CommandNameFromInput();
    command_matches = RankSlashCommands(state.commands, name);
    if (!command_matches.empty()) {
      if (last_command_query == name) {
        if (command_match_selected < 0 ||
            command_match_selected >= static_cast<int>(command_matches.size())) {
          command_match_selected = 0;
        }
      } else {
        command_match_selected = 0;
      }
    } else {
      command_match_selected = -1;
    }
    last_command_query = name;
  };

  auto PermissionLabelFor = [&](const PermissionPresetInfo& permission) {
    std::string label = permission.name;
    if (permission.id == "read-only") label = "只读 (read-only)";
    else if (permission.id == "workspace-write") label = "工作区写入 (workspace-write)";
    else if (permission.id == "danger-full-access") label = "完全访问 (full access)";
    return label.empty() ? permission.id : label;
  };

  auto UpdateInputPlaceholder = [&] {
    if (SlashPaletteActive()) {
      input_placeholder = "搜索命令…";
      return;
    }
    if (SlashActive()) {
      std::string name = CommandNameFromInput();
      for (const auto& command : state.commands) {
        if (command.name == name && !command.hint.empty()) {
          input_placeholder = command.hint;
          return;
        }
      }
    }
    input_placeholder = "输入消息，Enter 发送";
  };

  auto Submit = [&] {
    if (state.closed || !state.hello_seen) return;
    if (state.approval.active) AnswerApproval();
    else if (state.ask.active) AnswerCurrentQuestion();
    else if (SlashActive()) {
      RefreshCommandMatches();
      if (slash_permission_popup) {
        if (slash_permission_selected >= 0 &&
            slash_permission_selected < static_cast<int>(state.permissions.size())) {
          OutboundCommand command;
          command.type = "run-command";
          command.text = "/permission " + state.permissions[slash_permission_selected].id;
          Send(command);
        }
        slash_permission_popup = false;
        input_text.clear();
        stick_to_bottom = true;
        scroll_anchor = 1.0;
        return;
      }
      SlashSubmitDecision decision = DecideSlashSubmit(input_text, state.commands,
                                                       command_match_selected);
      if (decision.kind == SlashSubmitKind::Fill) {
        input_text = decision.fill;
        if (CommandNameFromInput() == "permission") {
          slash_permission_popup = true;
          slash_permission_selected = -1;
          for (size_t i = 0; i < state.permissions.size(); ++i) {
            if (state.permissions[i].id == state.permission_id) slash_permission_selected = static_cast<int>(i);
          }
          if (slash_permission_selected < 0 && !state.permissions.empty()) slash_permission_selected = 0;
        }
        RefreshCommandMatches();
        return;
      }
      OutboundCommand command;
      if (decision.kind == SlashSubmitKind::Execute) {
        command.type = "run-command";
        command.text = decision.line.empty() ? input_text : decision.line;
      } else {
        command.type = "prompt";
        command.text = Trim(decision.line.empty() ? input_text : decision.line);
      }
      Send(command);
    } else {
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
  auto input_component = Input(&input_text, &input_placeholder, input_option);
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
    if (event == Event::CtrlE) { ToggleActiveReasoning(); return true; }
    if (event == Event::ArrowUpCtrl) { MoveActiveReasoning(-1); return true; }
    if (event == Event::ArrowDownCtrl) { MoveActiveReasoning(1); return true; }
    if (event.is_mouse() && event.mouse().button == Mouse::WheelUp) {
      stick_to_bottom = false;
      scroll_anchor = std::max(0.0, scroll_anchor - 0.08);
      return true;
    }
    if (event.is_mouse() && event.mouse().button == Mouse::WheelDown) {
      scroll_anchor = std::min(1.0, scroll_anchor + 0.08);
      if (scroll_anchor >= 0.999) { stick_to_bottom = true; scroll_anchor = 1.0; }
      return true;
    }
    if (slash_permission_popup && (event == Event::Tab || event == Event::ArrowUp || event == Event::ArrowDown) &&
        !state.permissions.empty()) {
      int size = static_cast<int>(state.permissions.size());
      int delta = event == Event::ArrowUp ? -1 : event == Event::ArrowDown ? 1 : 1;
      if (slash_permission_selected < 0) slash_permission_selected = 0;
      else slash_permission_selected = (slash_permission_selected + size + delta) % size;
      return true;
    }
    if (event == Event::Tab && SlashPaletteActive() && !command_matches.empty()) {
      if (command_match_selected < 0) command_match_selected = 0;
      else command_match_selected = (command_match_selected + 1) % static_cast<int>(command_matches.size());
      const CommandInfo& suggestion = state.commands[command_matches[command_match_selected]];
      if (suggestion.hint.empty()) {
        OutboundCommand command;
        command.type = "run-command";
        command.text = "/" + suggestion.name;
        Send(command);
        input_text.clear();
        stick_to_bottom = true;
        scroll_anchor = 1.0;
      } else {
        input_text = "/" + suggestion.name + " ";
        if (suggestion.name == "permission") {
          slash_permission_popup = true;
          slash_permission_selected = -1;
          for (size_t j = 0; j < state.permissions.size(); ++j) {
            if (state.permissions[j].id == state.permission_id) slash_permission_selected = static_cast<int>(j);
          }
          if (slash_permission_selected < 0 && !state.permissions.empty()) slash_permission_selected = 0;
        }
        RefreshCommandMatches();
      }
      return true;
    }
    if ((event == Event::ArrowUp || event == Event::ArrowDown) &&
        SlashPaletteActive() && !command_matches.empty()) {
      int delta = event == Event::ArrowUp ? -1 : 1;
      int size = static_cast<int>(command_matches.size());
      command_match_selected = (command_match_selected + size + delta) % size;
      return true;
    }
    return false;
  });
  auto new_session_button = Button("＋ 新建会话", NewSessionInWorkspace, ButtonOption::Ascii());

  auto sidebar_container = Container::Vertical({workspace_menu, new_session_button, session_menu, preset_menu, permission_menu, model_menu, reasoning_menu});
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
    sidebar.push_back(separatorEmpty());
    sidebar.push_back(text("模式") | bold | color(Color::Cyan));
    sidebar.push_back(preset_menu->Render() | yframe);
    sidebar.push_back(separatorEmpty());
    sidebar.push_back(text("权限") | bold | color(Color::Cyan));
    sidebar.push_back(permission_menu->Render() | yframe);
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
    if (active_reasoning == std::string::npos) {
      for (size_t i = state.messages.size(); i > 0; --i) {
        if (!state.messages[i - 1].reasoning.empty()) { active_reasoning = i - 1; break; }
      }
    }
    reasoning_click_boxes.assign(state.messages.size(), ftxui::Box{});
    Elements messages;
    for (size_t i = 0; i < state.messages.size(); ++i) {
      messages.push_back(RenderMessage(state.messages[i], i, &expanded_reasoning,
                                       active_reasoning, &reasoning_click_boxes[i]));
      if (i + 1 < state.messages.size()) messages.push_back(separatorEmpty());
    }
    if (messages.empty()) messages.push_back(text("（暂无消息）") | dim | center);

    const float anchor = stick_to_bottom ? 1.0f : static_cast<float>(scroll_anchor);
    Element history = vbox(std::move(messages)) | focusPositionRelative(0.0f, anchor) |
                      yframe | vscroll_indicator | flex;

    RefreshCommandMatches();
    if (slash_permission_popup &&
        !(SlashActive() && CommandNameFromInput() == "permission")) {
      slash_permission_popup = false;
      slash_permission_selected = -1;
    }
    Element permission_popup = emptyElement();
    if (slash_permission_popup) {
      slash_permission_boxes.assign(state.permissions.size(), ftxui::Box{});
      Elements permission_rows;
      for (size_t i = 0; i < state.permissions.size(); ++i) {
        const auto& permission = state.permissions[i];
        const bool selected = static_cast<int>(i) == slash_permission_selected;
        Element row = hbox({
            text(selected ? "❯ " : "  ") | color(selected ? Color::Cyan : Color::White),
            text(PermissionLabelFor(permission)) | (selected ? bold : nothing),
            filler(),
        });
        row = row | reflect(slash_permission_boxes[i]);
        permission_rows.push_back(row);
      }
      permission_popup = window(text(" 权限预设 "), vbox(std::move(permission_rows)));
    }
    Element command_palette = emptyElement();
    if (SlashPaletteActive()) {
      command_palette = BuildCommandPaletteElement(state.commands, command_matches,
                                                   command_match_selected,
                                                   command_palette_boxes);
    }

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
      if (!state.preset_name.empty()) compact += " · " + state.preset_name;
      if (!state.permission_name.empty()) compact += " · " + state.permission_name;
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
    if (slash_permission_popup) {
      main_lines.push_back(separator());
      main_lines.push_back(permission_popup);
    }
    if (SlashPaletteActive()) {
      main_lines.push_back(separator());
      main_lines.push_back(command_palette);
    }
    if (state.ask.active || state.approval.active) {
      main_lines.push_back(separator());
      main_lines.push_back(question_panel);
    }
    UpdateInputPlaceholder();
    main_lines.push_back(separator());
    main_lines.push_back(hbox({
        text("❯ ") | bold | color(Color::Cyan),
        input_component->Render() | flex,
    }));
    main_lines.push_back(hbox({
        text(std::string("Enter 发送 · Esc 停止 · Ctrl+N 新建 · PgUp/PgDn 历史 · 点击/Ctrl+E 思考") +
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
    if (event.is_mouse() && event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed) {
      if (slash_permission_popup) {
        for (size_t i = 0; i < slash_permission_boxes.size() && i < state.permissions.size(); ++i) {
          if (slash_permission_boxes[i].Contain(event.mouse().x, event.mouse().y)) {
            slash_permission_selected = static_cast<int>(i);
            OutboundCommand command;
            command.type = "run-command";
            command.text = "/permission " + state.permissions[i].id;
            Send(command);
            slash_permission_popup = false;
            input_text.clear();
            stick_to_bottom = true;
            scroll_anchor = 1.0;
            return true;
          }
        }
      }
      if (SlashPaletteActive()) {
        for (size_t i = 0; i < command_palette_boxes.size() && i < command_matches.size(); ++i) {
          if (command_palette_boxes[i].Contain(event.mouse().x, event.mouse().y)) {
            const CommandInfo& command = state.commands[command_matches[i]];
            if (command.hint.empty()) {
              OutboundCommand run;
              run.type = "run-command";
              run.text = "/" + command.name;
              Send(run);
              input_text.clear();
              stick_to_bottom = true;
              scroll_anchor = 1.0;
            } else {
              input_text = "/" + command.name + " ";
              if (command.name == "permission") {
                slash_permission_popup = true;
                slash_permission_selected = -1;
                for (size_t j = 0; j < state.permissions.size(); ++j) {
                  if (state.permissions[j].id == state.permission_id) slash_permission_selected = static_cast<int>(j);
                }
                if (slash_permission_selected < 0 && !state.permissions.empty()) slash_permission_selected = 0;
              }
              RefreshCommandMatches();
            }
            return true;
          }
        }
      }
      for (size_t i = 0; i < reasoning_click_boxes.size() && i < state.messages.size(); ++i) {
        if (!state.messages[i].reasoning.empty() &&
            reasoning_click_boxes[i].Contain(event.mouse().x, event.mouse().y)) {
          active_reasoning = i;
          if (expanded_reasoning.count(i) != 0) expanded_reasoning.erase(i);
          else expanded_reasoning.insert(i);
          return true;
        }
      }
    }
    if (event == Event::Custom) { DrainEvents(); return true; }
    if (event == Event::CtrlQ) {
      OutboundCommand command; command.type = "quit"; Send(command); screen.Exit(); return true;
    }
    if (event == Event::Escape && slash_permission_popup) {
      slash_permission_popup = false;
      slash_permission_selected = -1;
      input_text = "/permission";
      RefreshCommandMatches();
      return true;
    }
    if (event == Event::Escape && SlashPaletteActive()) {
      input_text.clear();
      RefreshCommandMatches();
      return true;
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
