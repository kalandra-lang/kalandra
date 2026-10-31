import { runTests } from "@vscode/test-electron";
import {
  access,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  writeFile,
} from "node:fs/promises";
import { constants } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { windowsCliEntry } from "./windows-cli.mjs";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repo = path.resolve(root, "../../..");
const server =
  process.env.KALANDRA_LSP ||
  path.join(
    repo,
    "build/native/native/kalandra-lsp" +
      (process.platform === "win32" ? ".exe" : ""),
  );
const executable = process.env.KALANDRA_VSCODE_EXECUTABLE;
if (!executable || !path.isAbsolute(executable))
  throw new Error(
    "Set KALANDRA_VSCODE_EXECUTABLE to an installed VS Code executable (no automatic downloads).",
  );
await access(server, constants.X_OK);
await access(executable, constants.X_OK);
await import("./package.mjs");
const base = path.join(repo, "build/native/editor");
const scratch = await mkdtemp(path.join(base, "host-"));
const archive = path.join(base, "kalandra-native.vsix");
const unpacked = path.join(scratch, "unpacked");
const workspace = path.join(scratch, "workspace");
const other = path.join(scratch, "other");
const profile = path.join(scratch, "profile");
const extensions = path.join(scratch, "extensions");
for (const folder of [
  workspace,
  other,
  path.join(profile, "User"),
  extensions,
  unpacked,
])
  await mkdir(folder, { recursive: true });
const extract = Bun.spawn(
  [
    process.platform === "win32" ? "python" : "python3",
    "-c",
    `import sys,zipfile\nwith zipfile.ZipFile(sys.argv[1]) as z:\n names=z.namelist()\n assert 'extension/dist/extension.cjs' in names\n assert 'extension/dist/THIRD_PARTY_NOTICES.txt' in names\n assert not any('/node_modules/' in p or '/test/' in p or '/src/' in p or '/scripts/' in p for p in names)\n assert len(names) <= 12, names\n z.extractall(sys.argv[2])`,
    archive,
    unpacked,
  ],
  { stdout: "inherit", stderr: "inherit" },
);
if ((await extract.exited) !== 0)
  throw new Error("Packaged artifact validation failed");
const cli =
  process.env.KALANDRA_VSCODE_CLI ||
  path.resolve(path.dirname(executable), "../Resources/app/bin/code");
const cliCommand =
  process.platform === "win32"
    ? [
        executable,
        await windowsCliEntry(executable),
      ]
    : [cli];
if (process.platform !== "win32") await access(cli, constants.X_OK);
const install = Bun.spawn(
  [
    ...cliCommand,
    "--user-data-dir",
    profile,
    "--extensions-dir",
    extensions,
    "--install-extension",
    archive,
    "--force",
  ],
  {
    stdout: "inherit",
    stderr: "inherit",
    env:
      process.platform === "win32"
        ? { ...process.env, ELECTRON_RUN_AS_NODE: "1" }
        : process.env,
  },
);
if ((await install.exited) !== 0)
  throw new Error("Isolated VSIX installation failed");
const manifest = JSON.parse(
  await readFile(path.join(root, "package.json"), "utf8"),
);
const installed = (await readdir(extensions)).find(
  (entry) =>
    entry === `${manifest.publisher}.${manifest.name}-${manifest.version}`,
);
if (!installed)
  throw new Error("VSIX was not installed in the isolated extension directory");
await writeFile(
  path.join(workspace, "library.k"),
  "export fn double(value:int)->int:\n  value*2\n",
);
await writeFile(
  path.join(workspace, "main.k"),
  'from "./library.k" import double\nfn main()->int:\n  answer=double(21)\n  answer\n',
);
await writeFile(path.join(workspace, "completion.k"), "fn main():\n  0\n");
await writeFile(
  path.join(workspace, "completion-library.k"),
  "export type Box:\n  value:int\n  other:int\n" +
    "export enum Choice:\n  Some:\n    value:int\n    count:int\n  Other\n" +
    "export fn make()->fn(int,int)->int:\n  fn(a:int,b:int):a+b\n" +
    "export error ParseError:\n  Invalid\n" +
    "export type Problem = ParseError | TaskError\n",
);
await writeFile(
  path.join(workspace, "caller.k"),
  'from "./library.k" import double\nfn caller()->int:\n  double(7)\n',
);
await writeFile(
  path.join(other, "outsider.k"),
  'from "../workspace/library.k" import double\nfn outside()->int:\n  double(5)\n',
);
await writeFile(
  path.join(workspace, "kalandra.toml"),
  'format=1\n[project]\nentry="main.k"\n[packages]\nmath="library.k"\n',
);
await writeFile(
  path.join(profile, "User/settings.json"),
  JSON.stringify({
    "kalandra.native.serverPath": server,
    "telemetry.telemetryLevel": "off",
    "workbench.startupEditor": "none",
    "update.mode": "none",
    "extensions.autoCheckUpdates": false,
    "extensions.autoUpdate": false,
    "security.workspace.trust.enabled": false,
    "files.autoSave": "off",
    "workbench.enableExperiments": false,
  }),
);
const workspaceFile = path.join(scratch, "integration.code-workspace");
await writeFile(
  workspaceFile,
  JSON.stringify({ folders: [{ path: workspace }, { path: other }] }),
);
const resultPath = path.join(scratch, "result.json");
await runTests({
  vscodeExecutablePath: executable,
  extensionDevelopmentPath: path.join(extensions, installed),
  extensionTestsPath: path.join(root, "test/editor.cjs"),
  launchArgs: [
    workspaceFile,
    "--user-data-dir",
    profile,
    "--extensions-dir",
    extensions,
    "--disable-extensions",
    "--skip-welcome",
    "--skip-release-notes",
    "--disable-workspace-trust",
    "--disable-updates",
    "--disable-gpu",
  ],
  extensionTestsEnv: {
    KALANDRA_EDITOR_WORKSPACE: workspace,
    KALANDRA_EDITOR_OTHER: other,
    KALANDRA_EDITOR_RESULT: resultPath,
  },
});
const result = JSON.parse(await readFile(resultPath, "utf8"));
result.installedVsix = true;
await writeFile(
  path.join(base, "qualification.json"),
  JSON.stringify(result, null, 2) + "\n",
);
console.log(
  `Native editor qualification: ${result.checks.length} checks passed in VS Code ${result.vscode}.`,
);
console.log(`Result: ${resultPath}`);
