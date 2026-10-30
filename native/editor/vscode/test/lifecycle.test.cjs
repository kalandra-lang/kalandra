const { test, expect, mock } = require("bun:test");
test("trust, native argv/logging, restarts, failed-start recovery and shutdown", async () => {
  const commands = new Map();
  const clients = [];
  let configChanged;
  let failStart = false;
  let failDispose = false;
  let notifications = 0;
  const output = {
    appendLine() {},
    show() {},
    dispose() {},
    error() {},
    warn() {},
    info() {},
    debug() {},
    trace() {},
  };
  const workspace = {
    isTrusted: false,
    workspaceFolders: [{ uri: { scheme: "file", fsPath: "/workspace" } }],
    getConfiguration: () => ({
      get: (key) =>
        ({
          serverPath: "/tools/kalandra-lsp",
          projectPath: "",
          placement: false,
        })[key],
    }),
    onDidChangeConfiguration: (callback) => {
      configChanged = callback;
      return { dispose() {} };
    },
    onDidChangeWorkspaceFolders: () => ({ dispose() {} }),
  };
  mock.module("vscode", () => ({
    workspace,
    window: {
      createOutputChannel: (name, options) => {
        expect(options.log).toBe(true);
        return output;
      },
      showErrorMessage: () => {
        ++notifications;
        return new Promise(() => {});
      },
    },
    commands: {
      registerCommand: (name, callback) => {
        commands.set(name, callback);
        return { dispose() {} };
      },
      executeCommand: async () => {},
    },
  }));
  mock.module("vscode-languageclient/node", () => ({
    CloseAction: { DoNotRestart: 1 },
    ErrorAction: { Continue: 1 },
    LanguageClient: class {
      constructor(id, name, server, options) {
        this.server = server;
        this.options = options;
        this.disposed = false;
        clients.push(this);
      }
      async start() {
        if (failStart) {
          failStart = false;
          throw new Error("start failed");
        }
      }
      async dispose() {
        this.disposed = true;
        if (failDispose) {
          failDispose = false;
          throw new Error("already stopped");
        }
      }
    },
  }));
  const extension = require("../src/extension.cjs");
  await extension.activate({ subscriptions: [] });
  expect(clients).toHaveLength(0);
  workspace.isTrusted = true;
  await commands.get("kalandra.native.restart")();
  expect(clients).toHaveLength(1);
  expect(clients[0].server).toEqual({
    command: "/tools/kalandra-lsp",
    args: [],
    options: { shell: false },
  });
  expect(clients[0].options.errorHandler.closed().action).toBe(1);
  configChanged({ affectsConfiguration: () => true });
  await commands.get("kalandra.native.restart")();
  expect(clients).toHaveLength(3);
  expect(clients.slice(0, -1).every((client) => client.disposed)).toBe(true);
  failStart = true;
  await commands.get("kalandra.native.restart")();
  expect(notifications).toBe(1); // A dismissed/ignored notification cannot hold startup open.
  failDispose = true;
  await commands.get("kalandra.native.restart")();
  expect(clients).toHaveLength(5);
  failStart = true;
  await commands.get("kalandra.native.restart")();
  expect(clients).toHaveLength(6);
  failDispose = true;
  await extension.deactivate();
  expect(clients.at(-1).disposed).toBe(true);
  await commands.get("kalandra.native.restart")();
  expect(clients).toHaveLength(6);
});
