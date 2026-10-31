import { test, expect } from "bun:test";
import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { windowsCliEntry } from "../scripts/windows-cli.mjs";

test("find legacy and versioned Windows CLI layouts without splitting paths", async () => {
  for (const version of ["", "2c33b50b16"]) {
    const root = await mkdtemp(path.join(tmpdir(), "VS Code CLI "));
    try {
      const cli = path.join(root, version, "resources/app/out/cli.js");
      await mkdir(path.dirname(cli), { recursive: true });
      await writeFile(cli, "");
      expect(await windowsCliEntry(path.join(root, "Code.exe"))).toBe(cli);
    } finally {
      await rm(root, { recursive: true });
    }
  }
});

test("reject absent and ambiguous Windows CLI entries", async () => {
  const root = await mkdtemp(path.join(tmpdir(), "VS Code CLI "));
  try {
    const executable = path.join(root, "Code.exe");
    await expect(windowsCliEntry(executable)).rejects.toThrow("found 0");
    for (const version of ["2c33b50b16", "abcdef1234"]) {
      const cli = path.join(root, version, "resources/app/out/cli.js");
      await mkdir(path.dirname(cli), { recursive: true });
      await writeFile(cli, "");
    }
    await expect(windowsCliEntry(executable)).rejects.toThrow("found 2");
  } finally {
    await rm(root, { recursive: true });
  }
});
