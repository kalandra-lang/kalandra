const { test, expect } = require("bun:test");
const { serverConfiguration } = require("../src/config.cjs");
const base = { serverPath: "kalandra-lsp", projectPath: "", placement: false };
test("launch a binary directly, preserving absolute paths with spaces", () => {
  expect(serverConfiguration(base, [])).toEqual({
    command: "kalandra-lsp",
    args: [],
  });
  expect(
    serverConfiguration(
      { ...base, serverPath: "/tools with spaces/kalandra-lsp" },
      [],
    ),
  ).toEqual({ command: "/tools with spaces/kalandra-lsp", args: [] });
});
test("explicit project and placement arguments are separate argv entries", () => {
  expect(
    serverConfiguration(
      { ...base, projectPath: "project with spaces.toml", placement: true },
      ["/workspace"],
    ),
  ).toEqual({
    command: "kalandra-lsp",
    args: ["--placement", "--project", "/workspace/project with spaces.toml"],
  });
  expect(
    serverConfiguration({ ...base, projectPath: "/other/project.toml" }, [
      "/one",
      "/two",
    ]).args,
  ).toEqual(["--project", "/other/project.toml"]);
});
test("reject relative launch paths, shell expressions, control characters and ambiguous projects", () => {
  for (const serverPath of [
    "",
    "./kalandra-lsp",
    "../build/native/kalandra-lsp",
    "bun server.js",
    "server; echo nope",
    "server\n",
    "-server",
  ]) {
    expect(() =>
      serverConfiguration({ ...base, serverPath }, ["/workspace"]),
    ).toThrow();
  }
  for (const folders of [[], ["/one", "/two"]]) {
    expect(() =>
      serverConfiguration({ ...base, projectPath: "kalandra.toml" }, folders),
    ).toThrow();
  }
  for (const projectPath of ["config.json", "config.toml\0"]) {
    expect(() =>
      serverConfiguration({ ...base, projectPath }, ["/one"]),
    ).toThrow();
  }
});
test("manifest limits execution to trusted local workspaces and native .k documents", () => {
  const manifest = require("../package.json");
  expect(manifest.capabilities.untrustedWorkspaces.supported).toBe(false);
  expect(manifest.capabilities.virtualWorkspaces.supported).toBe(false);
  expect(manifest.contributes.languages[0].extensions).toEqual([".k"]);
  expect(
    manifest.contributes.configuration.properties["kalandra.native.serverPath"]
      .scope,
  ).toBe("machine");
});
