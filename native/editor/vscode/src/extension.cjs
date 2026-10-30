const vscode = require("vscode");
const {
  LanguageClient,
  CloseAction,
  ErrorAction,
} = require("vscode-languageclient/node");
const { serverConfiguration } = require("./config.cjs");

let client;
let output;
let pending = Promise.resolve();
let disposed = false;

function activate(context) {
  output = vscode.window.createOutputChannel("Kalandra Native", { log: true });
  context.subscriptions.push(output);
  const restart = () => {
    pending = pending
      .then(async () => {
        if (client) {
          const previous = client;
          client = undefined;
          try {
            await previous.dispose();
          } catch (error) {
            output.appendLine(String(error));
          }
        }
        if (disposed || !vscode.workspace.isTrusted) return;
        try {
          const config = vscode.workspace.getConfiguration("kalandra.native");
          const folders = (vscode.workspace.workspaceFolders || []).filter(
            (f) => f.uri.scheme === "file",
          );
          const launch = serverConfiguration(
            {
              serverPath: config.get("serverPath"),
              projectPath: config.get("projectPath"),
              placement: config.get("placement"),
            },
            folders.map((f) => f.uri.fsPath),
          );
          client = new LanguageClient(
            "kalandra-native",
            "Kalandra Native",
            {
              command: launch.command,
              args: launch.args,
              // The native executable uses stdio by default; specifying the client
              // stdio enum would append a Node-style --stdio argument it does not accept.
              options: { shell: false },
            },
            {
              documentSelector: [
                { scheme: "file", language: "kalandra", pattern: "**/*.k" },
              ],
              outputChannel: output,
              // Crashes require an explicit restart rather than a repeated launch loop.
              errorHandler: {
                error: () => ({ action: ErrorAction.Continue }),
                closed: () => ({ action: CloseAction.DoNotRestart }),
              },
            },
          );
          await client.start();
        } catch (error) {
          output.appendLine(String(error));
          if (!disposed) {
            void vscode.window
              .showErrorMessage(
                "Kalandra could not start its native language server. Check Server Path and Project Path.",
                "Open Settings",
                "Show Output",
              )
              .then((action) => {
                if (disposed) return;
                if (action === "Open Settings")
                  void vscode.commands.executeCommand(
                    "workbench.action.openSettings",
                    "kalandra.native",
                  );
                if (action === "Show Output") output.show();
              });
          }
        }
      })
      .catch((error) => output.appendLine(String(error)));
    return pending;
  };
  context.subscriptions.push(
    vscode.commands.registerCommand("kalandra.native.restart", restart),
    vscode.commands.registerCommand("kalandra.native.showOutput", () =>
      output.show(),
    ),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration("kalandra.native")) void restart();
    }),
    vscode.workspace.onDidChangeWorkspaceFolders(() => void restart()),
  );
  return restart();
}
async function deactivate() {
  disposed = true;
  await pending;
  if (client) {
    const previous = client;
    client = undefined;
    try {
      await previous.dispose();
    } catch (error) {
      output.appendLine(String(error));
    }
  }
}
module.exports = { activate, deactivate };
