# dsh_tui

用第三方 TUI 组件给 **DeepSeek Harness（dsh）** 做的终端界面。视觉框架对齐
WebUI：左侧工作区/会话选择，中间流式对话，右侧状态、token、统计和待办面板。

- UI：`third_party/FTXUI`
- 异步桥接：`third_party/executor`（Blocking I/O worker + `comm::MpscChannel`）
- dsh 接入：`packages/dsh-tui`（profile bundle，桥接 Node 与原生前端）

> 注意：apt 的 `dsh` 是 dancer's distributed shell，二者同名。本项目**不会**调用
> PATH 中的裸 `dsh`，启动脚本显式使用 `npx @deepseek-ai/dsh`。

## 架构

```
npx @deepseek-ai/dsh --profile tui
 ├─ @deepseek-ai/dsh-base        (agent / session / tools / approval)
 └─ packages/dsh-tui
      ├─ startup.js              解析 --resume / --binary / --help
      └─ index.js                创建/恢复 Agent，spawn 原生前端
           fd3 ───────────────▶ dsh_tui (C++) 读 JSON-lines 事件
           fd4 ◀─────────────── dsh_tui 写 prompt/resume/new-session/answer
```

`dsh` 的原生前端把 stdin/stdout 留给终端，协议走额外 fd3/fd4，因此 FTXUI
全屏绘制与桥接协议互不干扰。前端侧由 executor 的 Blocking I/O worker 把
fd3 的事件投递到 `MpscChannel`，FTXUI 主循环消费渲染。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/dsh_tui --self-test
```

## 启动

直接运行即可。`dsh_tui` 会：

1. 自动初始化 `$DSH_HOME/profiles/tui`（默认 `~/.dsh/profiles/tui`）；
2. 通过 `npx @deepseek-ai/dsh`（或 `$DSH_BIN`）启动 DeepSeek Harness；
3. 在 fd3/fd4 上完成桥接，用户只会在状态栏看到 `◌ 桥接中… → ● 桥接已连接`。

```bash
./build/dsh_tui
./build/dsh_tui --resume <session-id>
./scripts/run-tui.sh
```

`dsh`/`npx` 的 stdout/stderr 会被静默，避免污染 TUI；需要诊断桥接进程时：

```bash
DSH_TUI_DEBUG=1 ./build/dsh_tui
```

也可以沿用 profile 启动方式（此时 dsh 反向 spawn 原生前端）：

```bash
./scripts/setup-profile.sh
npx @deepseek-ai/dsh --profile tui
```

`setup-profile.sh` 会在 `$DSH_HOME/profiles/tui`（默认 `~/.dsh/profiles/tui`）
写入 profile，并把 `node_modules/dsh-tui` 链接到本仓库 `packages/dsh-tui`。
如需指定 Harness home 或已安装的 launcher：

```bash
DSH_HOME=/tmp/dsh-home ./scripts/setup-profile.sh
DSH_LAUNCHER="/path/to/deepseek-dsh-bin" ./scripts/run-tui.sh
```

前端二进制查找顺序：`--binary` → `$DSH_TUI_BINARY` → `./build/dsh_tui` →
`PATH` 中的 `dsh_tui`。

## 功能

- 工作区列表与历史会话列表（读取 `$DSH_HOME/storages/workspace.json`）
- 模式选择：标准模式 / PTC 模式 / 极简模式 / 创造模式（agent presets）
- 权限设置：只读 / 工作区写入 / 完全访问（sandbox + approval 预设）
- 模型选择与思考深度选择（读取 dsh llm 目录并写入默认模型）
- 新建会话 / `--resume <id>` 恢复会话 / 运行中切换会话
- 流式文本与 reasoning 展示、工具调用卡片
- `ask_user_question` 与 approval 交互
- 状态面板：模型、cwd、token、上下文窗口、回合/步骤、LLM/工具/首 token 耗时、待办
- Esc / Ctrl+C 停止当前回合，Ctrl+N 新建会话，Ctrl+Q 退出
- 窗口大小可用鼠标拖动：会话区与工作区、状态栏之间的分隔线可拖拽调整
- 窄终端自适应：优先保证会话区，状态栏/工作区按宽度隐藏
- 会话历史 PgUp/PgDn 滚动，新消息默认吸底显示

## 目录

```text
src/                 原生前端（FTXUI 三栏布局、fd3/fd4 协议、executor worker）
packages/dsh-tui/    DeepSeek Harness profile bundle（零 JS 运行时依赖）
scripts/             构建/启动脚本
third_party/FTXUI    FTXUI（git submodule）
third_party/executor executor（git submodule）
```

## Git

```bash
git submodule update --init --recursive   # 首次 clone 后拉取第三方库
git log --oneline --decorate --all
```
