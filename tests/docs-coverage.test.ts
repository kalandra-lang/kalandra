import { expect, test } from "bun:test";
import { readFile, readdir } from "node:fs/promises";
import { compile } from "../apps/website/node_modules/@timeleap/tiramisu/dist/index.js";
import api from "./fixtures/public-api.json";

const root = new URL("../", import.meta.url);
const content = new URL("apps/website/src/content/native/", root);
const read = (path: string) => readFile(new URL(path, root), "utf8");

test("every authored documentation page has an execution-plan entry", async () => {
  const pages = (await readdir(content))
    .filter((file) => file.endsWith(".tiramisu"))
    .map((file) => file.slice(0, -9));
  const plan = JSON.parse(await read("tests/fixtures/docs-examples.json"));
  expect(pages.sort()).toEqual(Object.keys(plan).sort());
});

test("every compiler-provided callable has a signature and public reference entry", async () => {
  const semantics = await read("native/lib/Semantics.cpp");
  const start = semantics.search(/\binvoke\(Frame\s*&frame\s*,/);
  expect(start).toBeGreaterThan(0);
  const end = semantics.indexOf("    const auto id = concreteTarget", start);
  expect(end).toBeGreaterThan(start);
  const dispatch = semantics.slice(start, end);
  const names = new Set(
    [...dispatch.matchAll(/callee == "([\w.]+)"/g)].map((m) => m[1]!),
  );
  const numeric = await read("native/include/kalandra/NumericType.h");
  for (const match of numeric.matchAll(
    /NumericDescription\{NumericType::\w+,\s*"\w+",\s*NumericFamily::\w+,\s*\d+,\s*"(\w+)"/g,
  ))
    names.add(match[1]!);
  expect(names.size).toBeGreaterThan(20);
  expect([...names].sort()).toEqual(api.map((entry) => entry.name).sort());
  expect(new Set(api.map((entry) => entry.name)).size).toBe(api.length);
  const index = await readFile(new URL("library.tiramisu", content), "utf8");
  for (const entry of api) {
    const page = await readFile(
      new URL(`${entry.page}.tiramisu`, content),
      "utf8",
    );
    expect(page).toContain(`h2 { "${entry.name}" }`);
    for (const signature of entry.signature.split("\n")) {
      expect(page).toContain(`code { ${JSON.stringify(signature)} }`);
    }
    const anchor = entry.name.toLowerCase().replace(/[^\w\s-]/g, "");
    expect(index).toContain(
      `url = "/docs/${entry.page}#${anchor}", "${entry.name} —`,
    );
  }
});

test("public documentation links resolve to authored pages and heading anchors", async () => {
  const value = (n: any): string =>
    typeof n === "string"
      ? n
      : Array.isArray(n)
        ? n.map(value).join("")
        : n
          ? value(n.shards ?? n.children ?? n.value ?? [])
          : "";
  const pages = new Map<string, { headings: Set<string>; links: string[] }>();
  for (const file of await readdir(content)) {
    if (!file.endsWith(".tiramisu")) continue;
    const headings = new Set<string>();
    const links: string[] = [];
    function walk(n: any) {
      if (!n || typeof n !== "object") return;
      if (n.functionName === "link" || /^h[1-6]$/.test(n.functionName ?? "")) {
        const args = n.parameters.parameters;
        if (n.functionName === "link")
          links.push(value(args.find((a: any) => a.name === "url")?.value));
        else
          headings.add(
            value(args.find((a: any) => !a.name)?.value)
              .trim()
              .toLowerCase()
              .replace(/[^\w\s-]/g, "")
              .replace(/\s+/g, "-")
              .replace(/-+/g, "-")
              .trim(),
          );
      }
      for (const child of Object.values(n)) {
        if (Array.isArray(child)) child.forEach(walk);
        else if (child && typeof child === "object") walk(child);
      }
    }
    walk(compile(await readFile(new URL(file, content), "utf8")));
    pages.set(
      file === "index.tiramisu" ? "/docs" : `/docs/${file.slice(0, -9)}`,
      { headings, links },
    );
  }
  for (const [page, { links }] of pages)
    for (const link of links) {
      if (!link.startsWith("/docs")) continue;
      const [path, anchor] = link.split("#");
      expect(pages.has(path!), `${page} links to missing page ${link}`).toBe(
        true,
      );
      if (anchor)
        expect(
          pages.get(path!)!.headings.has(anchor),
          `${page} links to missing heading ${link}`,
        ).toBe(true);
    }
});

test("standard-module callables have individual public reference entries", async () => {
  const declarations = await read("native/include/kalandra/StandardLibrary.h");
  const pages: Record<string, string> = {
    Io: "console",
    Format: "console",
    Process: "applications",
    Filesystem: "filesystem",
    Parse: "parsing",
    Path: "paths",
    Env: "environment",
    Time: "time",
    Buffer: "buffers",
    Option: "options",
    Iteration: "iteration",
  };
  let count = 0;
  for (const module of declarations.matchAll(
    /case StandardModule::(\w+):\s*return R"k\(([\s\S]*?)\)k";/g,
  )) {
    const page = pages[module[1]!];
    expect(page).toBeDefined();
    const reference = await readFile(
      new URL(`${page}.tiramisu`, content),
      "utf8",
    );
    for (const fn of module[2]!.matchAll(/export fn (\w+)\(/g)) {
      ++count;
      expect(reference).toContain(`h2 { "${fn[1]}" }`);
      expect(reference).toContain(`code { "${fn[1]}(`);
    }
  }
  expect(count).toBe(56);
  const options = await readFile(new URL("options.tiramisu", content), "utf8");
  expect(options).toContain('h2 { "Option[T]" }');
  expect(options).toContain('from "std/option" import Option');
  expect(options).toContain("Option.None");
  expect(options).toContain("Option.Some");
  const console = await readFile(new URL("console.tiramisu", content), "utf8");
  expect(console).toContain('h2 { "IoError" }');
  const applications = await readFile(
    new URL("applications.tiramisu", content),
    "utf8",
  );
  expect(applications).toContain("code: u8");
  expect(applications).toContain('from "std/process" import ExitStatus');
});
