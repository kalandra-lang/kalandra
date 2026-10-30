import { mkdir, readFile, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
await mkdir(path.join(root, "dist"), { recursive: true });
const build = await Bun.build({
  entrypoints: [path.join(root, "src/extension.cjs")],
  outdir: path.join(root, "dist"),
  naming: "extension.cjs",
  target: "node",
  format: "cjs",
  external: ["vscode"],
  minify: false,
});
if (!build.success)
  throw new AggregateError(build.logs, "Extension bundle failed");

// Ship complete notices for the runtime dependency closure. Build/test-only
// tools stay outside the VSIX and its dependency notice list.
const seen = new Set();
const notices = [];
async function visit(name, from) {
  const require = createRequire(path.join(from, "package.json"));
  let folder = path.dirname(require.resolve(name));
  let metadata;
  for (;;) {
    try {
      const candidate = JSON.parse(
        await readFile(path.join(folder, "package.json"), "utf8"),
      );
      if (candidate.name === name) {
        metadata = candidate;
        break;
      }
    } catch {}
    const parent = path.dirname(folder);
    if (parent === folder)
      throw new Error(`Cannot locate dependency metadata: ${name}`);
    folder = parent;
  }
  const key = `${name}@${metadata.version}`;
  if (seen.has(key)) return;
  seen.add(key);
  let license;
  for (const file of [
    "LICENSE",
    "LICENSE.md",
    "LICENSE.txt",
    "license",
    "license.md",
    "LICENSE-MIT",
  ]) {
    try {
      license = await readFile(path.join(folder, file), "utf8");
      break;
    } catch {}
  }
  if (!license) throw new Error(`Missing runtime license: ${key}`);
  notices.push(`${key} (${metadata.license})\n${license.trim()}\n`);
  for (const dependency of Object.keys(metadata.dependencies || {}).sort())
    await visit(dependency, folder);
}
const manifest = JSON.parse(
  await readFile(path.join(root, "package.json"), "utf8"),
);
for (const name of Object.keys(manifest.dependencies).sort())
  await visit(name, root);
await writeFile(
  path.join(root, "dist/THIRD_PARTY_NOTICES.txt"),
  notices.join("\n---\n\n"),
);
console.log(
  `Bundled native editor client and ${seen.size} runtime dependency notices.`,
);
