import { expect, test } from "bun:test";
import { readFile, readdir } from "node:fs/promises";
import { compileTiramisu, toMarkdown } from "@tiramisu-docs/core";

const content = new URL("../apps/website/src/content/native/", import.meta.url);

test("docs keep navigation in lists and prose links inline", () => {
  const source = `Read the link { url = "/docs/tutorial", "tutorial" } first.

list {
  link { url = "/docs/arrays", "Arrays" },
  link { url = "/docs/tensors", "Tensors" }
}
`;
  const { svelte } = compileTiramisu(source);
  expect(svelte).toContain(
    '<p>Read the <a href="/docs/tutorial">tutorial</a> first.</p>',
  );
  expect(svelte).toContain(
    '<ul><li><a href="/docs/arrays">Arrays</a></li><li><a href="/docs/tensors">Tensors</a></li></ul>',
  );
  expect(toMarkdown(source)).toContain(
    "- [Arrays](/docs/arrays)\n- [Tensors](/docs/tensors)",
  );
});

test("ordered list export preserves numbering and inline formatting", () => {
  expect(
    toMarkdown(
      'list { type = "ordered", bold { "First" }, link { url = "/docs/tutorial", "Next" } }',
    ),
  ).toContain("1. **First**\n2. [Next](/docs/tutorial)");
});

test("every docs page separates standalone links and preserves them in Markdown", async () => {
  for (const file of await readdir(content)) {
    if (!file.endsWith(".tiramisu")) continue;
    const source = await readFile(new URL(file, content), "utf8");
    const { svelte } = compileTiramisu(source);
    for (const paragraph of svelte.matchAll(/<p>([\s\S]*?)<\/p>/g)) {
      expect(paragraph[1], file).not.toMatch(/<\/a>\s*<a\b/);
      expect(paragraph[1], file).not.toMatch(/<(?:ul|ol)\b/);
    }
    const markdown = toMarkdown(source);
    for (const match of source.matchAll(
      /^\s*link \{ url = "([^"]+)", "([^"]+)" \}/gm,
    )) {
      expect(markdown, `${file}: ${match[2]}`).toContain(
        `[${match[2]}](${match[1]})`,
      );
    }
  }
});
