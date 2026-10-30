/** Extract the authored Tiramisu, then check the rendered and executable examples. */
import { compile } from "../apps/website/node_modules/@timeleap/tiramisu/dist/index.js";
import { readdir, readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("../", import.meta.url));
const directory = `${root}apps/website/src/content/native`;
const pages: Record<string, { language: string; source: string }[]> = {};
const value = (n: any): string =>
  typeof n === "string"
    ? n
    : Array.isArray(n)
      ? n.map(value).join("")
      : n
        ? value(n.shards ?? n.children ?? n.value ?? [])
        : "";
function walk(n: any, blocks: (typeof pages)[string]) {
  if (!n || typeof n !== "object") return;
  if (n.functionName === "codeblock") {
    const args = n.parameters.parameters;
    blocks!.push({
      language: value(
        args.find((a: any) => a.name === "language")?.value,
      ).trim(),
      source: value(args.find((a: any) => !a.name)?.value).trim() + "\n",
    });
  }
  for (const child of Object.values(n)) {
    if (typeof child === "object") {
      if (Array.isArray(child)) child.forEach((v) => walk(v, blocks));
      else walk(child, blocks);
    }
  }
}
for (const file of (await readdir(directory)).sort()) {
  if (!file.endsWith(".tiramisu")) continue;
  const blocks: (typeof pages)[string] = [];
  walk(compile(await readFile(`${directory}/${file}`, "utf8")), blocks);
  pages[file.slice(0, -9)] = blocks;
}
const proc = Bun.spawn(
  [
    process.env.PYTHON ?? "python3",
    `${root}scripts/verify-docs.py`,
    ...process.argv.slice(2),
  ],
  { cwd: root, stdin: "pipe", stdout: "inherit", stderr: "inherit" },
);
proc.stdin.write(JSON.stringify(pages));
proc.stdin.end();
process.exit(await proc.exited);
