#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "ds_protocol.hpp"

namespace dsh_tui {

enum class MessageRole { Welcome, User, Assistant, Tool, System, Error };

struct ChatMessage {
  MessageRole role = MessageRole::System;
  std::string text;
  bool streaming = false;
};

struct PendingAsk {
  bool active = false;
  std::string request_id;
  std::vector<Question> questions;
  size_t index = 0;
};

struct PendingApproval {
  bool active = false;
  std::string request_id;
  std::string tool_name;
  std::string reason;
};

class DeepSeekState {
 public:
  bool ready = false;
  bool closed = false;
  bool running = false;
  bool hello_seen = false;
  bool resumed = false;

  std::string session_id;
  std::string model;
  std::string provider;
  std::string reasoning_effort;
  std::string cwd;
  std::string error;

  std::vector<WorkspaceInfo> workspaces;
  std::vector<SessionInfo> sessions;
  std::vector<ModelInfo> models;
  std::vector<PresetInfo> presets;
  std::string preset_id;
  std::string preset_name;
  std::deque<ChatMessage> messages;
  TokenStats stats;
  std::vector<TodoItem> todos;
  std::vector<std::string> bridge_log;
  PendingAsk ask;
  PendingApproval approval;

  size_t visible_messages = 120;

  DeepSeekState();
  void Apply(const InboundEvent& event);
  void Add(MessageRole role, std::string text, bool streaming = false);
  void ResetConversation(const std::string& reason);

  const WorkspaceInfo* FindWorkspace(const std::string& id) const;
  const SessionInfo* FindSession(const std::string& id) const;
  std::string SessionDisplayName(const std::string& id) const;
};

}  // namespace dsh_tui
