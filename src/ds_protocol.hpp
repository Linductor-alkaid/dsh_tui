#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace dsh_tui {

inline constexpr int kEventFd = 3;    // dsh bridge -> dsh_tui
inline constexpr int kCommandFd = 4;  // dsh_tui -> dsh bridge

struct WorkspaceInfo {
  std::string id;
  std::string path;
  std::string title;
  std::vector<std::string> session_ids;
};

struct PermissionPresetInfo {
  std::string id;
  std::string name;
  std::string description;
};

struct PresetInfo {
  std::string id;
  std::string name;
  std::string description;
};

struct ReasoningEffortInfo {
  std::string id;
  std::string name;
  std::string description;
};

struct ModelInfo {
  std::string provider;
  std::string id;
  std::string name;
  std::string default_effort;
  std::vector<ReasoningEffortInfo> efforts;
};

struct SessionInfo {
  std::string id;
  std::string title;
  std::string cwd;
  std::string workspace_id;
};

struct HistoryMessage {
  std::string role;  // user | assistant | tool | system
  std::string text;
  std::string reasoning;
};

struct Question {
  std::string id;
  std::string header;
  std::string question;
  std::string detail;
  bool multi_select = false;
  std::vector<std::string> options;
};

struct TokenStats {
  int64_t input_tokens = 0;
  int64_t output_tokens = 0;
  int64_t cache_read_tokens = 0;
  int64_t cache_write_tokens = 0;
  int64_t context_window = 0;
  int64_t surface_tokens = 0;
  int64_t turns = 0;
  int64_t steps = 0;
  double llm_ms = 0.0;
  double tool_ms = 0.0;
  double ttft_ms = 0.0;
  double decode_ms = 0.0;
};

struct TodoItem {
  std::string content;
  std::string status;  // pending | in_progress | completed
};

struct InboundEvent {
  enum class Type {
    Hello,
    Reset,
    Workspaces,
    Sessions,
    Models,
    Presets,
    Preset,
    Permissions,
    Permission,
    History,
    Message,
    Delta,
    Tool,
    Status,
    Turn,
    Todo,
    Stats,
    Ask,
    Approval,
    BridgeLog,
    Error,
    Bye,
  };

  Type type = Type::Error;
  std::string text;
  std::string secondary;
  std::string third;
  std::string detail;
  std::string reasoning_effort;
  std::string preset_id;
  bool flag = false;
  bool running = false;
  int64_t number = 0;

  std::vector<WorkspaceInfo> workspaces;
  std::vector<SessionInfo> sessions;
  std::vector<ModelInfo> models;
  std::vector<PresetInfo> presets;
  std::vector<PermissionPresetInfo> permissions;
  std::string permission_id;
  std::string permission_name;
  std::vector<HistoryMessage> history;
  std::vector<Question> questions;
  std::vector<TodoItem> todos;
  std::string request_id;
  std::string tool_name;
  std::string reason;
  TokenStats stats;
};

std::optional<InboundEvent> ParseInboundEvent(const Json& json);

struct OutboundCommand {
  std::string type;
  std::string text;
  std::string request_id;
  std::string item_id;
  std::vector<std::string> selected;
  std::string custom;
};

std::string OutboundJson(const OutboundCommand& command);

}  // namespace dsh_tui
