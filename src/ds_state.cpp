#include "ds_state.hpp"

#include <algorithm>

namespace dsh_tui {

namespace {

std::string ClampText(std::string text, size_t max_chars = 20000) {
  if (text.size() <= max_chars) return text;
  text.resize(max_chars);
  text += "\n…(已截断)";
  return text;
}

std::string ReasonText(const std::string& reason) {
  if (reason.empty() || reason == "completed") return {};
  if (reason == "aborted") return "已停止";
  if (reason == "blocked") return "等待输入";
  if (reason == "error") return "出错";
  if (reason == "max-tokens") return "达到输出上限";
  if (reason == "interrupted") return "已中断";
  return reason;
}

}  // namespace

DeepSeekState::DeepSeekState() {
  Add(MessageRole::Welcome,
      "DeepSeek Harness TUI\n"
      "Enter 发送消息 · Esc 停止 · Ctrl+Q 退出\n"
      "等待 dsh 桥接进程接入…");
}

void DeepSeekState::Add(MessageRole role, std::string text, bool streaming) {
  if (role == MessageRole::Assistant && streaming && !messages.empty() &&
      messages.back().role == MessageRole::Assistant && messages.back().streaming) {
    messages.back().text += text;
    messages.back().text = ClampText(messages.back().text);
    return;
  }
  messages.push_back(ChatMessage{role, ClampText(std::move(text)), "", streaming});
  while (messages.size() > 600) messages.pop_front();
}

void DeepSeekState::ResetConversation(const std::string& reason) {
  messages.clear();
  running = false;
  ask = PendingAsk{};
  approval = PendingApproval{};
  error.clear();
  stats = TokenStats{};
  todos.clear();
  bridge_log.clear();
  reasoning_effort.clear();
  preset_id.clear();
  preset_name.clear();
  permission_id.clear();
  permission_name.clear();
  if (reason.empty()) {
    Add(MessageRole::Welcome, "会话已切换。输入消息开始。");
  } else {
    Add(MessageRole::System, reason);
  }
}

void DeepSeekState::Apply(const InboundEvent& event) {
  switch (event.type) {
    case InboundEvent::Type::Hello:
      session_id = event.text;
      model = event.secondary;
      provider = event.third;
      reasoning_effort = event.reasoning_effort;
      preset_id = event.preset_id;
      cwd = event.detail;
      resumed = event.flag;
      hello_seen = true;
      ready = true;
      closed = false;
      error.clear();
      break;

    case InboundEvent::Type::Reset:
      ResetConversation(event.text);
      break;

    case InboundEvent::Type::Workspaces:
      workspaces = event.workspaces;
      break;

    case InboundEvent::Type::Sessions:
      sessions = event.sessions;
      break;

    case InboundEvent::Type::Models:
      models = event.models;
      break;

    case InboundEvent::Type::Presets:
      presets = event.presets;
      break;

    case InboundEvent::Type::Preset:
      preset_id = event.text;
      preset_name = event.secondary;
      break;

    case InboundEvent::Type::Permissions:
      permissions = event.permissions;
      break;

    case InboundEvent::Type::Commands:
      commands = event.commands;
      break;

    case InboundEvent::Type::Permission:
      permission_id = event.permission_id;
      permission_name = event.permission_name;
      break;

    case InboundEvent::Type::History:
      messages.clear();
      for (const auto& item : event.history) {
        MessageRole role = MessageRole::System;
        if (item.role == "user") role = MessageRole::User;
        else if (item.role == "assistant") role = MessageRole::Assistant;
        else if (item.role == "tool") role = MessageRole::Tool;
        ChatMessage message;
        message.role = role;
        message.text = ClampText(item.text);
        message.reasoning = ClampText(item.reasoning);
        messages.push_back(std::move(message));
      }
      if (messages.empty()) Add(MessageRole::Welcome, "会话为空。输入消息开始。");
      break;

    case InboundEvent::Type::Message:
      if (event.text == "user") {
        Add(MessageRole::User, event.secondary);
      } else if (event.text == "assistant") {
        if (!messages.empty() && messages.back().role == MessageRole::Assistant &&
            messages.back().streaming) {
          messages.back().text = event.secondary;
          messages.back().reasoning = event.detail;
          messages.back().streaming = false;
        } else {
          ChatMessage message;
          message.role = MessageRole::Assistant;
          message.text = event.secondary;
          message.reasoning = event.detail;
          messages.push_back(std::move(message));
        }
      } else {
        Add(MessageRole::System, event.secondary);
      }
      break;

    case InboundEvent::Type::Delta:
      if (event.text == "reasoning") {
        if (!messages.empty() && messages.back().role == MessageRole::Assistant &&
            messages.back().streaming) {
          messages.back().reasoning += event.secondary;
          messages.back().reasoning = ClampText(messages.back().reasoning);
        } else {
          ChatMessage message;
          message.role = MessageRole::Assistant;
          message.reasoning = event.secondary;
          message.streaming = true;
          messages.push_back(std::move(message));
        }
      } else {
        Add(MessageRole::Assistant, event.secondary, true);
      }
      break;

    case InboundEvent::Type::Tool:
      if (event.text == "call") {
        Add(MessageRole::Tool, "⚒ 调用工具 " + event.secondary +
                                   (event.detail.empty() ? "" : "\n" + event.detail));
      } else {
        Add(MessageRole::Tool, "⚒ 工具结果 " + event.secondary +
                                   (event.flag ? "（出错）" : "") +
                                   (event.detail.empty() ? "" : "\n" + event.detail));
      }
      break;

    case InboundEvent::Type::Status:
      running = event.running;
      break;

    case InboundEvent::Type::Turn: {
      std::string reason = ReasonText(event.secondary);
      if (!reason.empty()) Add(MessageRole::System, "第 " + std::to_string(event.number) + " 回合：" + reason);
      break;
    }

    case InboundEvent::Type::Todo:
      todos = event.todos;
      break;

    case InboundEvent::Type::Stats:
      stats = event.stats;
      break;

    case InboundEvent::Type::Ask:
      ask.active = !event.questions.empty();
      ask.request_id = event.request_id;
      ask.questions = event.questions;
      ask.index = 0;
      if (ask.active) Add(MessageRole::System, "❓ " + ask.questions.front().question);
      break;

    case InboundEvent::Type::BridgeLog:
      bridge_log.push_back(event.text);
      while (bridge_log.size() > 6) bridge_log.erase(bridge_log.begin());
      break;

    case InboundEvent::Type::Approval:
      approval.active = true;
      approval.request_id = event.request_id;
      approval.tool_name = event.tool_name;
      approval.reason = event.reason;
      Add(MessageRole::System, "🔐 需要批准工具 " + event.tool_name +
                                   (event.reason.empty() ? "" : "：\n" + event.reason));
      break;

    case InboundEvent::Type::Error:
      error = event.text;
      Add(MessageRole::Error, event.text);
      break;

    case InboundEvent::Type::Bye:
      closed = true;
      running = false;
      ready = false;
      break;
  }
}

const WorkspaceInfo* DeepSeekState::FindWorkspace(const std::string& id) const {
  for (const auto& workspace : workspaces) {
    if (workspace.id == id) return &workspace;
  }
  return nullptr;
}

const SessionInfo* DeepSeekState::FindSession(const std::string& id) const {
  for (const auto& session : sessions) {
    if (session.id == id) return &session;
  }
  return nullptr;
}

std::string DeepSeekState::SessionDisplayName(const std::string& id) const {
  const SessionInfo* session = FindSession(id);
  if (session == nullptr || session->title.empty()) return id;
  return session->title + "  ·  " + id.substr(0, std::min<size_t>(8, id.size()));
}

}  // namespace dsh_tui
