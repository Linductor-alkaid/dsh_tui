import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { createReadStream, existsSync, readFileSync, writeSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

import { DSH_TUI_STARTUP_SERVICE } from "./startup.js";

export const name = "dsh-tui";
export const inject = [DSH_TUI_STARTUP_SERVICE, "userQuestions"];

const PACKAGE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");

function dshHome() {
  return process.env.DSH_HOME || join(homedir(), ".dsh");
}

/** Locate the native frontend. `--binary`/`DSH_TUI_BINARY` wins, then the
 * repo build layout (packages/dsh-tui -> <repo>/build/dsh_tui), then PATH. */
function resolveBinary(configured) {
  const executable = process.platform === "win32" ? "dsh_tui.exe" : "dsh_tui";
  const candidates = [];
  if (configured) candidates.push(configured);
  if (process.env.DSH_TUI_BINARY) candidates.push(process.env.DSH_TUI_BINARY);
  candidates.push(join(process.cwd(), "build", executable));
  candidates.push(join(PACKAGE_ROOT, "..", "..", "build", executable));
  candidates.push(executable);
  for (const candidate of candidates) {
    if (!candidate) continue;
    if (candidate.includes("/") || candidate.includes("\\")) {
      if (existsSync(candidate)) return resolve(candidate);
    } else {
      return candidate; // spawn searches PATH
    }
  }
  throw new Error(
    "dsh-tui: cannot find the dsh_tui frontend binary. Build it first (`cmake -S . -B build && cmake --build build -j`) or set DSH_TUI_BINARY."
  );
}

function readJsonFile(path, fallback) {
  try {
    return JSON.parse(readFileSync(path, "utf8"));
  } catch {
    return fallback;
  }
}

function workspaceSnapshot() {
  const storage = readJsonFile(join(dshHome(), "storages", "workspace.json"), {});
  const cache = readJsonFile(join(dshHome(), "storages", "session_projcache.json"), {});
  const workspaces = [];
  const sessions = [];
  const table = storage?.tables?.workspaces ?? {};
  for (const [id, workspace] of Object.entries(table)) {
    const sessionIds = Array.isArray(workspace?.sessionIds) ? workspace.sessionIds : [];
    workspaces.push({
      id,
      path: workspace?.path ?? "",
      title: workspace?.title ?? workspace?.path ?? id,
      sessionIds
    });
    for (const sessionId of sessionIds) {
      const cached = cache?.tables?.sessions?.[sessionId];
      sessions.push({
        id: sessionId,
        title: cached?.rows?.title?.val ?? sessionId,
        cwd: workspace?.path ?? process.cwd(),
        workspaceId: id
      });
    }
  }
  return { workspaces, sessions };
}

function contentToText(content) {
  const parts = [];
  const visit = (blocks) => {
    for (const block of blocks ?? []) {
      if (!block || typeof block !== "object") continue;
      if (block.type === "text") parts.push(block.text ?? "");
      else if (block.type === "reasoning") parts.push(`\n[思考] ${block.text ?? ""}\n`);
      else if (block.type === "tool-call") parts.push(`\n[调用工具 ${block.name ?? ""}] ${block.arguments ?? ""}\n`);
      else if (block.type === "tool-result") {
        if (block.isError) parts.push("\n[工具出错]\n");
        visit(block.content);
      } else if (typeof block.text === "string") {
        parts.push(block.text);
      }
    }
  };
  visit(content);
  return parts.join("");
}

function truncate(text, limit = 6000) {
  if (text.length <= limit) return text;
  return `${text.slice(0, limit)}\n…(truncated)`;
}

function installModelSelection(agentCtx, selection) {
  const disposeAssembly = agentCtx.on("system-prompt/assemble", async (_assembly, _context, next) => {
    const selected = selection.current;
    const assembled = await next();
    selection.assembled = selected;
    if (selected === undefined) return assembled;
    return {
      ...assembled,
      variables: { ...assembled.variables, provider: selected.provider, model: selected.model }
    };
  });
  const disposeRequest = agentCtx.on("agent/request", async (_payload, next) => {
    const resolved = await next();
    const selected = selection.assembled;
    if (selected === undefined) return resolved;
    const { reasoningEffort: _inheritedEffort, ...withoutInheritedEffort } = resolved;
    return {
      ...withoutInheritedEffort,
      provider: selected.provider,
      model: selected.model,
      ...(selected.reasoningEffort === undefined ? {} : { reasoningEffort: selected.reasoningEffort })
    };
  });
  return () => { disposeAssembly(); disposeRequest(); };
}

function messageSummary(message) {
  return truncate(contentToText(message.content), 6000);
}

function surfaceItemFor(event) {
  switch (event.type) {
    case "user/message":
      return { role: "user", text: messageSummary(event.data) };
    case "assistant/message":
      return { role: "assistant", text: messageSummary(event.data.message) };
    case "tool/call":
      return { role: "tool", text: `调用工具 ${event.data.name}\n${truncate(event.data.arguments, 4000)}` };
    case "tool/result": {
      const detail = messageSummary(event.data.message);
      return { role: "tool", text: `工具结果${event.data.error ? "（出错）" : ""}\n${detail}` };
    }
    default:
      return undefined;
  }
}

function historyPayload(events, boundary) {
  const messages = [];
  for (const event of events) {
    if (event.seq >= boundary) break;
    const item = surfaceItemFor(event);
    if (item) messages.push(item);
  }
  if (messages.length > 400) {
    return [{ role: "system", text: "…(仅显示最近 400 条历史消息)" }, ...messages.slice(-400)];
  }
  return messages;
}

export function apply(ctx, config) {
  const appExit = ctx.get("appExit");
  if (typeof appExit !== "function") {
    throw new Error("dsh-tui: the launcher must provide ctx.appExit before the tree mounts");
  }

  let child;
  let eventsOut;
  let commandLines;
  let liveHandle;
  let liveAgent;
  let sessions;
  let shuttingDown = false;
  let disposed = false;
  let switching = false;
  const selectionRef = { current: undefined, assembled: undefined };
  const closeListeners = [];
  const pendingQuestions = new Map();
  const pendingApprovals = new Map();
  const pendingPrompts = [];
  const usageTotals = {
    inputTokens: 0,
    outputTokens: 0,
    cacheReadTokens: 0,
    cacheWriteTokens: 0
  };
  let contextWindow = 0;

  const post = (payload) => {
    if (!eventsOut || eventsOut.destroyed || !eventsOut.writable) return false;
    try {
      eventsOut.write(`${JSON.stringify(payload)}\n`);
      return true;
    } catch {
      return false;
    }
  };

  const postStats = () => {
    if (!liveAgent) return;
    let projection;
    try {
      projection = ctx.get("sessionProjections")?.snapshot(liveAgent.session)?.values?.sessionStats?.val;
    } catch {
      projection = undefined;
    }
    post({
      type: "stats",
      inputTokens: usageTotals.inputTokens,
      outputTokens: usageTotals.outputTokens,
      cacheReadTokens: usageTotals.cacheReadTokens,
      cacheWriteTokens: usageTotals.cacheWriteTokens,
      contextWindow,
      surfaceTokens: projection?.surfaceTokens ?? 0,
      turns: projection?.turns ?? 0,
      steps: projection?.steps ?? 0,
      llmMs: projection?.llmMs ?? 0,
      toolMs: projection?.toolMs ?? 0,
      ttftMs: projection?.ttftMs ?? 0,
      decodeMs: projection?.decodeMs ?? 0
    });
  };

  const postModels = async () => {
    let llm;
    try {
      llm = ctx.get("llm");
    } catch {
      return;
    }
    if (!llm) return;
    try {
      const providers = llm.listProviders?.() ?? [];
      const models = [];
      for (const provider of providers) {
        const listed = (await llm.listModels?.(provider.id)) ?? [];
        for (const model of listed) {
          let resolved;
          try {
            resolved = await llm.resolveModel?.(provider.id, model.id);
          } catch {
            resolved = undefined;
          }
          models.push({
            provider: provider.id,
            id: model.id,
            name: model.name ?? model.id,
            defaultEffort: resolved?.reasoning?.defaultEffort ?? "",
            efforts: (resolved?.reasoning?.efforts ?? []).map((effort) => ({
              id: effort.id,
              name: effort.name ?? effort.id,
              description: effort.description ?? ""
            }))
          });
        }
      }
      post({ type: "models", models });
    } catch {
      // The model catalog is advisory; the conversation remains usable.
    }
  };

  const postWorkspaces = () => {
    const snapshot = workspaceSnapshot();
    post({ type: "workspaces", workspaces: snapshot.workspaces });
    post({ type: "sessions", sessions: snapshot.sessions });
  };

  const settlePendingInteractions = (error) => {
    for (const pending of [...pendingQuestions.values()]) {
      pending.onAbort?.();
      pending.reject(error);
    }
    pendingQuestions.clear();
    for (const pending of [...pendingApprovals.values()]) {
      pending.onAbort?.();
      pending.resolve("cancelled");
    }
    pendingApprovals.clear();
  };

  const shutdown = async (code) => {
    if (shuttingDown) return;
    shuttingDown = true;
    for (const close of closeListeners.splice(0)) {
      try { close(); } catch { /* already removed */ }
    }
    settlePendingInteractions(new Error("dsh-tui frontend disconnected"));
    try { commandLines?.close(); } catch { /* noop */ }
    try { eventsOut?.end(); } catch { /* noop */ }
    if (child && child.exitCode === null && child.signalCode === null) child.kill("SIGTERM");
    if (liveHandle?.agent) {
      try { await sessions?.flush(liveHandle.agent.session); } catch { /* noop */ }
    }
    if (liveHandle) {
      try { await liveHandle.dispose(); } catch { /* noop */ }
      liveHandle = undefined;
    }
    liveAgent = undefined;
    if (!disposed) appExit(code);
  };

  const switchSession = async (options) => {
    if (switching || shuttingDown) return;
    switching = true;
    try {
      if (liveHandle?.agent) {
        try { await sessions?.flush(liveHandle.agent.session); } catch { /* noop */ }
      }
      if (liveHandle) {
        await liveHandle.dispose();
        liveHandle = undefined;
      }
      liveAgent = undefined;
      usageTotals.inputTokens = 0;
      usageTotals.outputTokens = 0;
      usageTotals.cacheReadTokens = 0;
      usageTotals.cacheWriteTokens = 0;
      contextWindow = 0;

      selectionRef.current = ctx.get("agentDefaultModel")?.currentSelection();
      const agentOptions = {
        provider: selectionRef.current?.provider,
        model: selectionRef.current?.model
      };
      if (options.resumeSessionId) {
        liveHandle = await ctx.get("agents").resume({
          resumeSessionId: String(options.resumeSessionId),
          agentOptions,
          setup: (agentCtx) => { installModelSelection(agentCtx, selectionRef); }
        });
      } else {
        liveHandle = await ctx.get("agents").create({
          sessionId: `session-${randomUUID()}`,
          meta: { cwd: options.cwd ?? process.cwd() },
          agentOptions,
          setup: (agentCtx) => { installModelSelection(agentCtx, selectionRef); }
        });
      }
      liveAgent = liveHandle.agent;

      post({ type: "reset", reason: options.resumeSessionId ? "正在恢复会话…" : "正在创建新会话…" });
      postWorkspaces();
      post({
        type: "history",
        messages: historyPayload(liveAgent.session.events, liveAgent.session.firstLiveSeq)
      });
      post({
        type: "hello",
        sessionId: liveAgent.session.id,
        provider: selectionRef.current?.provider ?? "",
        model: selectionRef.current?.model ?? "",
        reasoningEffort: selectionRef.current?.reasoningEffort ?? "",
        cwd: liveAgent.session.header?.cwd ?? process.cwd(),
        resumed: Boolean(options.resumeSessionId)
      });
      post({ type: "status", status: liveAgent.status });
      postStats();

      for (const prompt of pendingPrompts.splice(0)) {
        liveAgent.followup(prompt);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      console.error(`dsh-tui: ${message}`);
      post({ type: "error", message });
    } finally {
      switching = false;
    }
  };

  const onCommand = async (raw) => {
    let command;
    try {
      command = JSON.parse(raw);
    } catch {
      return;
    }
    switch (command.type) {
      case "prompt": {
        const text = String(command.text ?? "").trim();
        if (!text) return;
        const message = {
          id: `msg-${randomUUID()}`,
          role: "user",
          content: [{ type: "text", text }],
          source: { kind: "user" }
        };
        if (liveAgent && !switching) liveAgent.followup(message);
        else pendingPrompts.push(message);
        break;
      }
      case "cancel":
        liveAgent?.cancel({ kind: "user" }, { keepInbox: true });
        break;
      case "resume-session": {
        const sessionId = String(command.text ?? "");
        if (sessionId && liveAgent?.session?.id !== sessionId) {
          const snapshot = workspaceSnapshot();
          const session = snapshot.sessions.find((item) => item.id === sessionId);
          await switchSession({ resumeSessionId: sessionId, cwd: session?.cwd });
        }
        break;
      }
      case "set-model": {
        const [provider, model] = String(command.text ?? "").split("|");
        if (provider && model) {
          selectionRef.current = {
            provider,
            model,
            ...(selectionRef.current?.reasoningEffort ? { reasoningEffort: selectionRef.current.reasoningEffort } : {})
          };
          try {
            await ctx.get("agentDefaultModel")?.saveSelection(selectionRef.current);
          } catch {
            // The current process-wide selection still applies for live agents.
          }
          if (liveAgent) {
            post({
              type: "hello",
              sessionId: liveAgent.session.id,
              provider,
              model,
              reasoningEffort: selectionRef.current?.reasoningEffort ?? "",
              cwd: liveAgent.session.header?.cwd ?? process.cwd(),
              resumed: Boolean(config.resumeSessionId)
            });
          }
        }
        break;
      }
      case "set-reasoning": {
        const effort = String(command.text ?? "");
        selectionRef.current = {
          ...(selectionRef.current ?? {}),
          ...(effort === "" || effort === "default" ? {} : { reasoningEffort: effort })
        };
        if (effort !== "" && effort !== "default") {
          selectionRef.current.reasoningEffort = effort;
        } else {
          delete selectionRef.current.reasoningEffort;
        }
        try {
          await ctx.get("agentDefaultModel")?.saveSelection(selectionRef.current);
        } catch {
          // The process-wide selection already applies to the live agent.
        }
        if (liveAgent) {
          post({
            type: "hello",
            sessionId: liveAgent.session.id,
            provider: selectionRef.current?.provider ?? "",
            model: selectionRef.current?.model ?? "",
            reasoningEffort: selectionRef.current?.reasoningEffort ?? "",
            cwd: liveAgent.session.header?.cwd ?? process.cwd(),
            resumed: Boolean(config.resumeSessionId)
          });
        }
        break;
      }
      case "new-session": {
        const snapshot = workspaceSnapshot();
        const workspaceId = String(command.text ?? "");
        const workspace = snapshot.workspaces.find((item) => item.id === workspaceId);
        await switchSession({ cwd: workspace?.path ?? process.cwd() });
        break;
      }
      case "answer": {
        const pending = pendingQuestions.get(String(command.requestId ?? ""));
        if (!pending) break;
        const questionId = String(command.questionId ?? pending.nextQuestionId ?? "");
        const answer = {
          id: questionId,
          selected: Array.isArray(command.selected) ? command.selected.map(String) : [],
          ...(typeof command.custom === "string" && command.custom !== "" ? { custom: command.custom } : {})
        };
        pending.answers.set(questionId, answer);
        if (pending.questionIds.every((id) => pending.answers.has(id))) {
          pendingQuestions.delete(String(command.requestId ?? ""));
          pending.onAbort?.();
          pending.resolve({ answers: pending.questionIds.map((id) => pending.answers.get(id)) });
        }
        break;
      }
      case "approval": {
        const pending = pendingApprovals.get(String(command.requestId ?? ""));
        if (!pending) break;
        pendingApprovals.delete(String(command.requestId ?? ""));
        pending.onAbort?.();
        pending.resolve(command.text === "rejected" ? "rejected" : "allowed-once");
        break;
      }
      case "quit":
        await shutdown(0);
        break;
      default:
        break;
    }
  };

  const wireCommandLines = (commandsIn) => {
    commandsIn?.on?.("error", () => void shutdown(1));
    commandLines = createInterface({ input: commandsIn, crlfDelay: Infinity });
    commandLines.on("line", (line) => void onCommand(line));
    commandLines.on("close", () => void shutdown(0));
  };

  // Parent mode: dsh_tui itself forked this dsh process with fd 3 (events,
  // child -> parent) and fd 4 (commands, parent -> child). The native frontend
  // is already the process that launched us, so do not spawn a second one.
  const attachParentTransport = () => {
    eventsOut = {
      destroyed: false,
      writable: true,
      write(chunk) {
        try {
          writeSync(3, chunk);
          return true;
        } catch {
          this.destroyed = true;
          this.writable = false;
          void shutdown(1);
          return false;
        }
      },
      end() {
        this.destroyed = true;
        this.writable = false;
      }
    };
    const commandsIn = createReadStream(null, { fd: 4, autoClose: false });
    wireCommandLines(commandsIn);
  };

  const attachChild = (binary) => {
    child = spawn(binary, ["--child"], {
      cwd: process.cwd(),
      env: { ...process.env, DSH_TUI_CHILD: "1" },
      stdio: ["inherit", "inherit", "inherit", "pipe", "pipe"]
    });
    eventsOut = child.stdio[3];
    const commandsIn = child.stdio[4];
    eventsOut?.on("error", () => void shutdown(1));
    wireCommandLines(commandsIn);
    child.once("error", (error) => {
      console.error(`dsh-tui: ${error instanceof Error ? error.message : String(error)}`);
      void shutdown(1);
    });
    child.once("exit", (code, signal) => {
      void shutdown(signal === null ? (code ?? 0) : 1);
    });
  };

  const projectEvent = (session, event) => {
    if (!liveAgent || session !== liveAgent.session) return;
    switch (event.type) {
      case "user/message":
        post({ type: "message", role: "user", text: messageSummary(event.data) });
        break;
      case "assistant/chunk": {
        const chunk = event.data.chunk;
        if (chunk.type === "text-delta") post({ type: "delta", part: "text", text: chunk.text });
        else if (chunk.type === "reasoning-delta") post({ type: "delta", part: "reasoning", text: chunk.text });
        break;
      }
      case "assistant/message": {
        const usage = event.data.usage;
        if (usage) {
          usageTotals.inputTokens += usage.inputTokens ?? 0;
          usageTotals.outputTokens += usage.outputTokens ?? 0;
          usageTotals.cacheReadTokens += usage.cacheReadTokens ?? 0;
          usageTotals.cacheWriteTokens += usage.cacheWriteTokens ?? 0;
        }
        post({ type: "message", role: "assistant", text: messageSummary(event.data.message) });
        postStats();
        break;
      }
      case "tool/call":
        post({ type: "tool", phase: "call", name: event.data.name, detail: truncate(event.data.arguments, 4000) });
        break;
      case "tool/result":
        post({
          type: "tool",
          phase: "result",
          name: event.data.message?.source?.callId ?? "",
          detail: truncate(messageSummary(event.data.message), 6000),
          isError: Boolean(event.data.error)
        });
        break;
      case "turn/end":
        post({ type: "turn", phase: "end", turn: event.data.turn, reason: event.data.reason?.kind ?? "" });
        break;
      case "todo/write":
        post({
          type: "todo",
          todos: (event.data.todos ?? []).map((todo) => ({
            content: todo.content,
            status: todo.status
          }))
        });
        break;
      case "request/context":
        contextWindow = event.data.contextWindow ?? contextWindow;
        postStats();
        break;
      default:
        break;
    }
  };

  const projectStatus = (payload) => {
    if (!liveAgent || payload.agent !== liveAgent) return;
    post({ type: "status", status: payload.status });
  };

  const askUserQuestions = (request) => new Promise((resolvePromise, rejectPromise) => {
    const requestId = randomUUID();
    let settled = false;
    const finish = (fn, value) => {
      if (settled) return;
      settled = true;
      pendingQuestions.delete(requestId);
      request.signal?.removeEventListener("abort", onAbort);
      fn(value);
    };
    const onAbort = () => finish(rejectPromise, new Error("ask_user_question was aborted"));
    request.signal?.addEventListener("abort", onAbort, { once: true });
    const questionIds = request.questions.map((question) => question.id);
    pendingQuestions.set(requestId, {
      questionIds,
      nextQuestionId: questionIds[0] ?? "",
      answers: new Map(),
      resolve: (value) => finish(resolvePromise, value),
      reject: (error) => finish(rejectPromise, error),
      onAbort: () => request.signal?.removeEventListener("abort", onAbort)
    });
    post({
      type: "ask",
      requestId,
      questions: request.questions.map((question) => ({
        id: question.id,
        header: question.header ?? "",
        question: question.question,
        detail: question.detail ?? "",
        multiSelect: Boolean(question.multiSelect),
        options: (question.options ?? []).map((option) => option.label)
      }))
    });
  });

  const askApproval = (request) => new Promise((resolvePromise) => {
    const requestId = randomUUID();
    let settled = false;
    const finish = (value) => {
      if (settled) return;
      settled = true;
      pendingApprovals.delete(requestId);
      request.signal?.removeEventListener("abort", onAbort);
      resolvePromise(value);
    };
    const onAbort = () => finish("cancelled");
    request.signal?.addEventListener("abort", onAbort, { once: true });
    pendingApprovals.set(requestId, {
      resolve: finish,
      onAbort: () => request.signal?.removeEventListener("abort", onAbort)
    });
    post({
      type: "approval",
      requestId,
      toolName: request.toolName,
      reason: request.reason ?? ""
    });
  });

  const run = async () => {
    try {
      await ctx.get("loader")?.await();
      sessions = ctx.get("sessions");
      const agents = ctx.get("agents");
      const defaultModel = ctx.get("agentDefaultModel");
      if (!agents || !sessions || !defaultModel) return;

      const closeUserQuestions = typeof ctx.userQuestions?.registerProvider === "function"
        ? ctx.userQuestions.registerProvider({ ask: askUserQuestions })
        : () => {};
      closeListeners.push(closeUserQuestions);

      const offApproval = ctx.on("approval/request", (request, next) => {
        if (!liveAgent || request.agent !== liveAgent) return next();
        return askApproval(request);
      });
      closeListeners.push(offApproval);
      closeListeners.push(ctx.on("session/event", projectEvent));
      closeListeners.push(ctx.on("agent/status", projectStatus));

      await switchSession(config.resumeSessionId ? { resumeSessionId: config.resumeSessionId } : {});
      if (liveAgent) {
        if (process.env.DSH_TUI_PARENT === "1") {
          attachParentTransport();
        } else {
          const binary = resolveBinary(config.binaryPath);
          attachChild(binary);
        }
        postWorkspaces();
        void postModels();
        post({
          type: "history",
          messages: historyPayload(liveAgent.session.events, liveAgent.session.firstLiveSeq)
        });
        post({
          type: "hello",
          sessionId: liveAgent.session.id,
          provider: selectionRef.current?.provider ?? "",
          model: selectionRef.current?.model ?? "",
          reasoningEffort: selectionRef.current?.reasoningEffort ?? "",
          cwd: liveAgent.session.header?.cwd ?? process.cwd(),
          resumed: Boolean(config.resumeSessionId)
        });
        post({ type: "status", status: liveAgent.status });
        postStats();
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      console.error(`dsh-tui: ${message}`);
      await shutdown(1);
    }
  };

  void run();

  return () => {
    disposed = true;
    return shutdown(0);
  };
}
