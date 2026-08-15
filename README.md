# dsh_tui

用第三方库给 **dsh**（DeepSeek Harness CLI）做的一个终端 TUI。

- UI 绘制：`third_party/FTXUI`
- 异步桥接读取/生命周期：`third_party/executor`（Blocking I/O worker + `comm::MpscChannel`）
- dsh 侧接入：仓库内 `packages/dsh-tui` profile bundle

## 架构

```
终端
 ├─ dsh --profile tui                (dsh launcher / Node)
 │    ├─ @deepseek-ai/dsh-base       (agent、session、tools、approval…)
 │    └─ packages/dsh-tui            (本仓库 profile bundle)
 │         ├─ dsh-tui/startup       解析 --resume / --binary / --help
 │         └─ dsh-tui               创建/恢复 Agent，spawn 原生前端
 │              fd3 ───────────────▶ dsh_tui (C++) 读事件
 │              fd4 ◀─────────────── dsh_tui 写 prompt/cancel/answer/quit
 └─ dsh_tui --child
      ├─ FTXUI 绘制会话、流式输出、工具调用、提问/审批
      └─ executor Blocking I/O worker 把 fd3 JSON-lines 投递到 MPSC channel
```

`dsh` 的原生前端把 stdin/stdout 留给终端，协议走额外的 fd3/fd4，因此 FTXUI
的全屏绘制和桥接协议互不干扰。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/dsh_tui --self-test   # 无 TTY 也可跑
```

也可以用快捷脚本：

```bash
./scripts/run-tui.sh
```

它会按需构建 `build/dsh_tui`、初始化 dsh 的 `tui` profile，然后启动
`dsh --profile tui`。

## 首次安装 dsh TUI profile

`dsh` 的 profile 位于 `$DSH_HOME/profiles/<name>`（默认 `~/.dsh/profiles`）。
脚本会创建 `tui` profile，并把 `node_modules/dsh-tui` 链接到本仓库的
`packages/dsh-tui`：

```bash
./scripts/setup-profile.sh

# 若当前目录不在 dsh 安装上下文中，可指定 DSH_HOME：
DSH_HOME=/tmp/dsh-home ./scripts/setup-profile.sh
```

之后启动：

```bash
dsh --profile tui                     # 新会话
dsh --profile tui --resume <id>       # 恢复会话
dsh --profile tui --binary /path/dsh_tui
```

前端二进制查找顺序：

1. `--binary`
2. `$DSH_TUI_BINARY`
3. `./build/dsh_tui`（仓库构建目录）
4. `PATH` 中的 `dsh_tui`

## 操作

| 按键 | 功能 |
| --- | --- |
| `Enter` | 发送消息 / 回答问题 / 提交审批 |
| `Esc` / `Ctrl+C` | 停止当前回合（向 agent 发送 cancel） |
| `PgUp` / `PgDn` | 调整显示的历史消息数量 |
| `Ctrl+Q` | 退出并关闭 dsh |
| `--resume <id>` | 恢复一个持久化 session |

工具调用、`ask_user_question` 和 approval 请求都会显示在 TUI 中并可直接回答。

## 目录

```text
src/                 C++ 前端（FTXUI 渲染、fd3/fd4 协议、executor worker）
packages/dsh-tui/    dsh profile bundle（无第三方 JS 运行时依赖）
scripts/             profile 初始化与一键运行脚本
third_party/FTXUI    FTXUI
third_party/executor executor
```
