#include "ds_protocol.hpp"

namespace dsh_tui {

namespace {

std::string StringField(const Json& json, std::string_view key) {
  return json.at(key).as_string();
}

bool BoolField(const Json& json, std::string_view key, bool fallback = false) {
  const Json* value = json.find(key);
  return value == nullptr ? fallback : value->as_bool();
}

int64_t IntField(const Json& json, std::string_view key) {
  const Json* value = json.find(key);
  return value == nullptr ? 0 : value->as_integer();
}

double NumberField(const Json& json, std::string_view key) {
  const Json* value = json.find(key);
  return value == nullptr ? 0.0 : value->as_number();
}

std::vector<std::string> StringArrayField(const Json& json, std::string_view key) {
  std::vector<std::string> result;
  const Json* value = json.find(key);
  if (value == nullptr || !value->is_array()) return result;
  for (const auto& item : value->as_array()) {
    if (item.is_string()) result.push_back(item.as_string());
  }
  return result;
}

TokenStats ParseStats(const Json& json) {
  TokenStats stats;
  stats.input_tokens = IntField(json, "inputTokens");
  stats.output_tokens = IntField(json, "outputTokens");
  stats.cache_read_tokens = IntField(json, "cacheReadTokens");
  stats.cache_write_tokens = IntField(json, "cacheWriteTokens");
  stats.context_window = IntField(json, "contextWindow");
  stats.surface_tokens = IntField(json, "surfaceTokens");
  stats.turns = IntField(json, "turns");
  stats.steps = IntField(json, "steps");
  stats.llm_ms = NumberField(json, "llmMs");
  stats.tool_ms = NumberField(json, "toolMs");
  stats.ttft_ms = NumberField(json, "ttftMs");
  stats.decode_ms = NumberField(json, "decodeMs");
  return stats;
}

}  // namespace

std::optional<InboundEvent> ParseInboundEvent(const Json& json) {
  InboundEvent event;
  std::string type = StringField(json, "type");

  if (type == "hello") {
    event.type = InboundEvent::Type::Hello;
    event.text = StringField(json, "sessionId");
    event.secondary = StringField(json, "model");
    event.third = StringField(json, "provider");
    event.detail = StringField(json, "cwd");
    event.reasoning_effort = StringField(json, "reasoningEffort");
    event.preset_id = StringField(json, "presetId");
    event.flag = BoolField(json, "resumed");
    return event;
  }
  if (type == "reset") {
    event.type = InboundEvent::Type::Reset;
    event.text = StringField(json, "reason");
    return event;
  }
  if (type == "workspaces") {
    event.type = InboundEvent::Type::Workspaces;
    const Json* workspaces = json.find("workspaces");
    if (workspaces != nullptr && workspaces->is_array()) {
      for (const auto& workspace : workspaces->as_array()) {
        WorkspaceInfo item;
        item.id = StringField(workspace, "id");
        item.path = StringField(workspace, "path");
        item.title = StringField(workspace, "title");
        item.session_ids = StringArrayField(workspace, "sessionIds");
        event.workspaces.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "sessions") {
    event.type = InboundEvent::Type::Sessions;
    const Json* sessions = json.find("sessions");
    if (sessions != nullptr && sessions->is_array()) {
      for (const auto& session : sessions->as_array()) {
        SessionInfo item;
        item.id = StringField(session, "id");
        item.title = StringField(session, "title");
        item.cwd = StringField(session, "cwd");
        item.workspace_id = StringField(session, "workspaceId");
        event.sessions.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "presets") {
    event.type = InboundEvent::Type::Presets;
    const Json* presets = json.find("presets");
    if (presets != nullptr && presets->is_array()) {
      for (const auto& preset : presets->as_array()) {
        PresetInfo item;
        item.id = StringField(preset, "id");
        item.name = StringField(preset, "name");
        item.description = StringField(preset, "description");
        event.presets.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "commands") {
    event.type = InboundEvent::Type::Commands;
    const Json* commands = json.find("commands");
    if (commands != nullptr && commands->is_array()) {
      for (const auto& command : commands->as_array()) {
        CommandInfo item;
        item.name = StringField(command, "name");
        item.description = StringField(command, "description");
        item.hint = StringField(command, "hint");
        event.commands.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "permissions") {
    event.type = InboundEvent::Type::Permissions;
    const Json* permissions = json.find("permissions");
    if (permissions != nullptr && permissions->is_array()) {
      for (const auto& permission : permissions->as_array()) {
        PermissionPresetInfo item;
        item.id = StringField(permission, "id");
        item.name = StringField(permission, "name");
        item.description = StringField(permission, "description");
        event.permissions.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "permission") {
    event.type = InboundEvent::Type::Permission;
    event.permission_id = StringField(json, "id");
    event.permission_name = StringField(json, "name");
    return event;
  }
  if (type == "preset") {
    event.type = InboundEvent::Type::Preset;
    event.text = StringField(json, "id");
    event.secondary = StringField(json, "name");
    return event;
  }
  if (type == "models") {
    event.type = InboundEvent::Type::Models;
    const Json* models = json.find("models");
    if (models != nullptr && models->is_array()) {
      for (const auto& model : models->as_array()) {
        ModelInfo item;
        item.provider = StringField(model, "provider");
        item.id = StringField(model, "id");
        item.name = StringField(model, "name");
        item.default_effort = StringField(model, "defaultEffort");
        const Json* efforts = model.find("efforts");
        if (efforts != nullptr && efforts->is_array()) {
          for (const auto& effort : efforts->as_array()) {
            ReasoningEffortInfo info;
            info.id = StringField(effort, "id");
            info.name = StringField(effort, "name");
            info.description = StringField(effort, "description");
            item.efforts.push_back(std::move(info));
          }
        }
        event.models.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "history") {
    event.type = InboundEvent::Type::History;
    const Json* messages = json.find("messages");
    if (messages != nullptr && messages->is_array()) {
      for (const auto& message : messages->as_array()) {
        HistoryMessage item;
        item.role = StringField(message, "role");
        item.text = StringField(message, "text");
        item.reasoning = StringField(message, "reasoning");
        if (item.role.empty()) item.role = "system";
        event.history.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "message") {
    event.type = InboundEvent::Type::Message;
    event.text = StringField(json, "role");
    event.secondary = StringField(json, "text");
    event.detail = StringField(json, "reasoning");
    return event;
  }
  if (type == "delta") {
    event.type = InboundEvent::Type::Delta;
    event.text = StringField(json, "part");
    event.secondary = StringField(json, "text");
    return event;
  }
  if (type == "tool") {
    event.type = InboundEvent::Type::Tool;
    event.text = StringField(json, "phase");  // call | result
    event.secondary = StringField(json, "name");
    event.detail = StringField(json, "detail");
    event.flag = BoolField(json, "isError");
    return event;
  }
  if (type == "status") {
    event.type = InboundEvent::Type::Status;
    event.running = StringField(json, "status") == "running";
    return event;
  }
  if (type == "turn") {
    event.type = InboundEvent::Type::Turn;
    event.text = StringField(json, "phase");
    event.secondary = StringField(json, "reason");
    event.number = IntField(json, "turn");
    return event;
  }
  if (type == "todo") {
    event.type = InboundEvent::Type::Todo;
    const Json* todos = json.find("todos");
    if (todos != nullptr && todos->is_array()) {
      for (const auto& todo : todos->as_array()) {
        TodoItem item;
        item.content = StringField(todo, "content");
        item.status = StringField(todo, "status");
        event.todos.push_back(std::move(item));
      }
    }
    if (event.todos.empty() && !StringField(json, "text").empty()) {
      event.todos.push_back(TodoItem{StringField(json, "text"), "pending"});
    }
    return event;
  }
  if (type == "stats") {
    event.type = InboundEvent::Type::Stats;
    event.stats = ParseStats(json);
    return event;
  }
  if (type == "ask") {
    event.type = InboundEvent::Type::Ask;
    event.request_id = StringField(json, "requestId");
    const Json* questions = json.find("questions");
    if (questions != nullptr && questions->is_array()) {
      for (const auto& question : questions->as_array()) {
        Question item;
        item.id = StringField(question, "id");
        item.header = StringField(question, "header");
        item.question = StringField(question, "question");
        item.detail = StringField(question, "detail");
        item.multi_select = BoolField(question, "multiSelect");
        item.options = StringArrayField(question, "options");
        event.questions.push_back(std::move(item));
      }
    }
    return event;
  }
  if (type == "approval") {
    event.type = InboundEvent::Type::Approval;
    event.request_id = StringField(json, "requestId");
    event.tool_name = StringField(json, "toolName");
    event.reason = StringField(json, "reason");
    return event;
  }
  if (type == "bridge-log") {
    event.type = InboundEvent::Type::BridgeLog;
    event.text = StringField(json, "text");
    return event;
  }
  if (type == "error") {
    event.type = InboundEvent::Type::Error;
    event.text = StringField(json, "message");
    return event;
  }
  if (type == "bye") {
    event.type = InboundEvent::Type::Bye;
    event.text = StringField(json, "reason");
    return event;
  }
  return std::nullopt;
}

std::string OutboundJson(const OutboundCommand& command) {
  Json::Object object;
  object["type"] = command.type;
  if (!command.text.empty()) object["text"] = command.text;
  if (!command.request_id.empty()) object["requestId"] = command.request_id;
  if (!command.item_id.empty()) object["questionId"] = command.item_id;
  if (!command.selected.empty() || !command.custom.empty()) {
    Json::Array selected;
    for (const auto& item : command.selected) selected.emplace_back(item);
    object["selected"] = std::move(selected);
    if (!command.custom.empty()) object["custom"] = command.custom;
  }
  return Json(std::move(object)).dump();
}

}  // namespace dsh_tui
