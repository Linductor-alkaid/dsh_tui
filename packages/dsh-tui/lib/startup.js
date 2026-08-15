// Dependency-free on purpose: dsh resolves this bundle through a profile
// node_modules symlink whose real path is the repository checkout, so static
// imports of @deepseek-ai/* would not see dsh's installation fallback.

export const name = "dsh-tui-startup";
export const inject = ["cmdlineArgs"];
export const DSH_TUI_STARTUP_SERVICE = "dshTuiStartup";

const HELP = `Usage: npx @deepseek-ai/dsh --profile tui [options]

Open the DeepSeek Harness terminal UI.

Options:
  --resume <session>   resume a persisted session id
  --binary <path>      path to the dsh_tui frontend executable
  -h, --help           show this help

Examples:
  npx @deepseek-ai/dsh --profile tui
  npx @deepseek-ai/dsh --profile tui --resume <session>
`;

export function apply(ctx) {
  const args = ctx.get("cmdlineArgs");
  const exit = ctx.get("appExit");
  if (args === undefined || typeof exit !== "function") {
    throw new Error(
      "dsh-tui-startup: the launcher must provide ctx.cmdlineArgs and ctx.appExit before the tree mounts"
    );
  }

  const argv = [...args.get()];
  const options = {};

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === "-h" || arg === "--help") {
      process.stdout.write(HELP);
      exit(0);
      return;
    }
    if (arg === "--resume" || arg === "--binary") {
      const value = argv[index + 1];
      if (value === undefined || value.startsWith("--")) {
        process.stderr.write(`dsh --profile tui: ${arg} needs a value\n`);
        exit(2);
        return;
      }
      options[arg.slice(2)] = value;
      index += 1;
      continue;
    }
    process.stderr.write(`dsh --profile tui: unknown argument ${JSON.stringify(arg)}\n\n${HELP}`);
    exit(2);
    return;
  }

  ctx.provide(DSH_TUI_STARTUP_SERVICE, {
    resumeSessionId: options.resume,
    binaryPath: options.binary
  });
}
