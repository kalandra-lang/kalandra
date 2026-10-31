import { readdir, stat } from "node:fs/promises";
import path from "node:path";

// Windows VS Code now keeps application resources in a commit-versioned folder.
// Invoke its JS CLI with Electron directly, preserving argv paths with spaces.
export async function windowsCliEntry(executable) {
  const root = path.dirname(executable);
  const versions = (await readdir(root, { withFileTypes: true }))
    .filter((entry) => entry.isDirectory() && /^[a-f0-9]{10,40}$/.test(entry.name))
    .map((entry) => entry.name);
  const entries = [];
  for (const version of ["", ...versions]) {
    const candidate = path.join(root, version, "resources/app/out/cli.js");
    try {
      if ((await stat(candidate)).isFile()) entries.push(candidate);
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
  }
  if (entries.length !== 1)
    throw new Error(`Expected one Windows VS Code CLI entry, found ${entries.length}`);
  return entries[0];
}
