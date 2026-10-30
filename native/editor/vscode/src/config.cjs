const path = require("node:path");

// Pure validation shared with tests. Launch an executable directly, never a shell.
function serverConfiguration(settings, folders) {
  const command = settings.serverPath;
  if (
    typeof command !== "string" ||
    !command ||
    /[\0\r\n]/.test(command) ||
    (!path.isAbsolute(command) && !/^[A-Za-z0-9_.-]+$/.test(command)) ||
    command.startsWith("-")
  ) {
    throw new Error(
      "Server Path must be an absolute executable path or an executable name on PATH.",
    );
  }
  const project = settings.projectPath;
  if (typeof project !== "string" || /[\0\r\n]/.test(project)) {
    throw new Error("Project Path must be an optional .toml manifest path.");
  }
  const args = [];
  if (settings.placement === true) args.push("--placement");
  if (project) {
    if (path.extname(project) !== ".toml")
      throw new Error("Project Path must end in .toml.");
    if (!path.isAbsolute(project) && folders.length !== 1) {
      throw new Error(
        "A relative Project Path requires exactly one local workspace folder.",
      );
    }
    args.push(
      "--project",
      path.isAbsolute(project) ? project : path.resolve(folders[0], project),
    );
  }
  return { command, args };
}
module.exports = { serverConfiguration };
