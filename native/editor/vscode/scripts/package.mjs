import { mkdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const output = path.resolve(
  root,
  "../../..",
  "build/native/editor/kalandra-native.vsix",
);
await import("./build.mjs");
await mkdir(path.dirname(output), { recursive: true });
const command = Bun.spawn(
  [
    process.execPath,
    path.join(root, "node_modules/@vscode/vsce/vsce"),
    "package",
    "--no-dependencies",
    "--out",
    output,
  ],
  {
    cwd: root,
    stdin: "ignore",
    stdout: "inherit",
    stderr: "inherit",
  },
);
if ((await command.exited) !== 0) throw new Error("VSIX packaging failed");
console.log(output);
