const vscode = require("vscode");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const checks = [];
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function eventually(action, description) {
  const end = Date.now() + 20000;
  let last;
  while (Date.now() < end) {
    try {
      const result = await action();
      if (result) return result;
    } catch (error) {
      last = error;
    }
    await delay(100);
  }
  throw new Error(`Timed out: ${description}${last ? ` (${last})` : ""}`);
}
function position(document, needle, inside = 1) {
  const offset = document.getText().indexOf(needle);
  assert(offset >= 0, `Missing ${needle}`);
  return document.positionAt(offset + inside);
}
async function replace(document, text) {
  const edit = new vscode.WorkspaceEdit();
  edit.replace(
    document.uri,
    new vscode.Range(
      document.positionAt(0),
      document.positionAt(document.getText().length),
    ),
    text,
  );
  assert(await vscode.workspace.applyEdit(edit));
}
function clearDiagnostics(document) {
  return eventually(
    () => vscode.languages.getDiagnostics(document.uri).length === 0,
    "diagnostics clear",
  );
}
async function hover(document, needle) {
  return eventually(async () => {
    const values = await vscode.commands.executeCommand(
      "vscode.executeHoverProvider",
      document.uri,
      position(document, needle),
    );
    return values?.length ? values : undefined;
  }, "hover registration/result");
}
function hoverText(values) {
  return values
    .flatMap((value) =>
      value.contents.map((c) => (typeof c === "string" ? c : c.value)),
    )
    .join("\n");
}
function label(item) {
  return typeof item.label === "string" ? item.label : item.label.label;
}
async function complete(document, marked, expected, trigger) {
  const cursor = marked.indexOf("|");
  assert(cursor >= 0);
  await replace(document, marked.replace("|", ""));
  const result = await eventually(async () => {
    const values = await vscode.commands.executeCommand(
      "vscode.executeCompletionItemProvider",
      document.uri,
      document.positionAt(cursor),
      trigger,
    );
    return values?.items.some((item) => label(item) === expected)
      ? values
      : undefined;
  }, `completion for ${expected}`);
  return result;
}
async function applyCompletion(document, item, oldText, newText) {
  const range = item.textEdit?.range || item.range?.replacing || item.range;
  const text = item.textEdit?.newText || item.insertText;
  assert(range instanceof vscode.Range, "completion replacement range");
  assert.equal(document.getText(range), oldText);
  assert.equal(text, newText);
  const edit = new vscode.WorkspaceEdit();
  edit.replace(document.uri, range, text);
  assert(await vscode.workspace.applyEdit(edit));
}
async function run() {
  const workspace = process.env.KALANDRA_EDITOR_WORKSPACE;
  const mainUri = vscode.Uri.file(path.join(workspace, "main.k"));
  const libraryUri = vscode.Uri.file(path.join(workspace, "library.k"));
  const callerUri = vscode.Uri.file(path.join(workspace, "caller.k"));
  const mainDisk = await fs.readFile(mainUri.fsPath, "utf8");
  const libraryDisk = await fs.readFile(libraryUri.fsPath, "utf8");
  const extension = vscode.extensions.getExtension(
    "kalandra-lang.kalandra-native",
  );
  assert(extension, "Packed extension not loaded");
  const main = await vscode.workspace.openTextDocument(mainUri);
  await vscode.window.showTextDocument(main);
  await extension.activate();
  assert.equal(main.languageId, "kalandra");
  assert.equal(vscode.workspace.workspaceFolders.length, 2);
  const firstHover = await hover(main, "double(21)");
  assert.match(hoverText(firstHover), /int64/);
  checks.push(
    "Packed VSIX activates for .k and starts the real native server in a multi-root window",
  );

  const definitions = await vscode.commands.executeCommand(
    "vscode.executeDefinitionProvider",
    mainUri,
    position(main, "double(21)"),
  );
  assert(
    definitions.some(
      (value) => (value.uri || value.targetUri).fsPath === libraryUri.fsPath,
    ),
  );
  checks.push("Go to definition follows an imported function to its source");
  const references = await vscode.commands.executeCommand(
    "vscode.executeReferenceProvider",
    mainUri,
    position(main, "double(21)"),
  );
  assert(references.some((value) => value.uri.fsPath === callerUri.fsPath));
  assert(!references.some((value) => value.uri.fsPath.endsWith("outsider.k")));
  checks.push(
    "Workspace references find unopened callers and respect the selected root",
  );
  const rename = await vscode.commands.executeCommand(
    "vscode.executeDocumentRenameProvider",
    mainUri,
    position(main, "double(21)"),
    "twice",
  );
  assert(rename instanceof vscode.WorkspaceEdit);
  assert(rename.entries().some(([uri]) => uri.fsPath === libraryUri.fsPath));
  assert(rename.entries().some(([uri]) => uri.fsPath === callerUri.fsPath));
  assert(
    rename
      .entries()
      .flatMap(([, edits]) => edits)
      .every((edit) => edit.newText === "twice"),
  );
  checks.push("Rename reaches the editor as a checked workspace edit proposal");

  await replace(
    main,
    'from "./library.k" import double\nfn main()->int:\n  dou\n',
  );
  const completion = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeCompletionItemProvider",
      mainUri,
      position(main, "dou\n", 3),
    );
    return result?.items.find(
      (item) =>
        (typeof item.label === "string" ? item.label : item.label.label) ===
        "double",
    );
  }, "scope completion");
  assert.equal(completion.textEdit?.newText || completion.insertText, "double");
  checks.push("Partly typed expression names receive native completion");

  const completionUri = vscode.Uri.file(path.join(workspace, "completion.k"));
  const completionLibraryUri = vscode.Uri.file(
    path.join(workspace, "completion-library.k"),
  );
  const completionDocument =
    await vscode.workspace.openTextDocument(completionUri);
  const completionDisk = await fs.readFile(completionUri.fsPath, "utf8");
  const completionLibraryDisk = await fs.readFile(
    completionLibraryUri.fsPath,
    "utf8",
  );
  const imports =
    'from "./completion-library.k" import Box as Crate, Choice as Pick\n';
  let values = await complete(
    completionDocument,
    imports + "fn main():\n  box=Crate {value:1,other:2}\n  box.va|Suffix\n",
    "value",
  );
  let item = values.items.find((item) => label(item) === "value");
  assert.equal(item.kind, vscode.CompletionItemKind.Field);
  await applyCompletion(completionDocument, item, "vaSuffix", "value");
  assert.match(hoverText(await hover(completionDocument, "value\n")), /int64/);
  await clearDiagnostics(completionDocument);
  checks.push(
    "Imported record-field completion applies the full replacement range in VS Code",
  );

  values = await complete(
    completionDocument,
    imports + "fn main():\n  Pick.|\n",
    "Some",
    ".",
  );
  assert.deepEqual(values.items.map(label).sort(), ["Other", "Some"]);
  assert.equal(
    values.items.find((item) => label(item) === "Some").kind,
    vscode.CompletionItemKind.EnumMember,
  );
  values = await complete(
    completionDocument,
    imports + "fn main():\n  Pick.So|Suffix {value:1,count:2}\n",
    "Some",
  );
  await applyCompletion(
    completionDocument,
    values.items.find((item) => label(item) === "Some"),
    "SoSuffix",
    "Some",
  );
  assert.match(hoverText(await hover(completionDocument, "value:1")), /int64/);
  await clearDiagnostics(completionDocument);
  values = await complete(
    completionDocument,
    imports +
      "fn inspect(p:Pick):\n  match p:\n    Pick.Other: 0\n    Pick.| {value:item}: item\n" +
      "fn main():\n  0\n",
    "Some",
    ".",
  );
  assert.deepEqual(values.items.map(label), ["Some"]);
  await applyCompletion(completionDocument, values.items[0], "", "Some");
  assert.match(hoverText(await hover(completionDocument, "item}:")), /int64/);
  await clearDiagnostics(completionDocument);
  checks.push(
    "Enum completion uses EnumMember items and excludes already matched cases in VS Code",
  );

  values = await complete(
    completionDocument,
    imports + "fn main():\n  Crate {|,other:2}\n",
    "value",
    "{",
  );
  assert.deepEqual(values.items.map(label), ["value"]);
  await applyCompletion(completionDocument, values.items[0], "", "value: ");
  assert(completionDocument.getText().includes("Crate {value: ,other:2}"));
  await replace(
    completionDocument,
    completionDocument.getText().replace("value: ,", "value:1,"),
  );
  assert.match(hoverText(await hover(completionDocument, "value:1")), /int64/);
  await clearDiagnostics(completionDocument);
  values = await complete(
    completionDocument,
    imports +
      "fn main():\n  box=Crate {value:1,other:2}\n  box with {va|Suffix:3}\n",
    "value",
  );
  await applyCompletion(
    completionDocument,
    values.items[0],
    "vaSuffix",
    "value",
  );
  assert.match(hoverText(await hover(completionDocument, "value:3")), /int64/);
  await clearDiagnostics(completionDocument);
  values = await complete(
    completionDocument,
    imports + "fn main():\n  Pick.Some {va|Suffix:1,count:2}\n",
    "value",
  );
  assert.deepEqual(values.items.map(label), ["value"]);
  await applyCompletion(
    completionDocument,
    values.items[0],
    "vaSuffix",
    "value",
  );
  assert.match(hoverText(await hover(completionDocument, "value:1")), /int64/);
  await clearDiagnostics(completionDocument);
  checks.push(
    "Record/update/enum payload labels preserve separators and exclude supplied fields in VS Code",
  );

  const pattern =
    imports +
    "fn inspect(p:Pick):\n  match p:\n" +
    "    Pick.Some {count:total,va|Suffix}: value+total\n" +
    "    Pick.Other: 0\nfn main():\n  0\n";
  values = await complete(completionDocument, pattern, "value", ",");
  assert.deepEqual(values.items.map(label), ["value"]);
  await applyCompletion(
    completionDocument,
    values.items[0],
    "vaSuffix",
    "value",
  );
  assert.match(
    hoverText(await hover(completionDocument, "value+total")),
    /int64/,
  );
  await clearDiagnostics(completionDocument);
  values = await complete(
    completionDocument,
    pattern
      .replace("va|Suffix", "va|Suffix:item")
      .replace("value+total", "item+total"),
    "value",
  );
  await applyCompletion(
    completionDocument,
    values.items[0],
    "vaSuffix",
    "value",
  );
  assert(completionDocument.getText().includes("value:item"));
  assert.match(
    hoverText(await hover(completionDocument, "item+total")),
    /int64/,
  );
  await clearDiagnostics(completionDocument);
  checks.push(
    "Match payload completion preserves shorthand and explicit local bindings in VS Code",
  );

  await replace(
    completionDocument,
    'from "./completion-library.k" import make as factory\n' +
      "fn main():\n  factory()(1,2)\n",
  );
  let higherSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      completionUri,
      position(completionDocument, "(1,2)", 3),
      ",",
    );
    return result?.signatures.length ? result : undefined;
  }, "factory-result signature");
  assert.equal(
    higherSignature.signatures[0].label,
    "fn(int64, int64) -> int64",
  );
  assert.equal(higherSignature.activeParameter, 1);
  assert.deepEqual(higherSignature.signatures[0].parameters[1].label, [10, 15]);
  await replace(
    completionDocument,
    'from "./completion-library.k" import make as factory\n' +
      "fn main():\n  callbacks=[factory()]\n  callbacks[0](1,2)\n",
  );
  higherSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      completionUri,
      position(completionDocument, "callbacks[0](1,2)", 15),
      ",",
    );
    return result?.signatures.length ? result : undefined;
  }, "indexed-callable signature");
  assert.equal(
    higherSignature.signatures[0].label,
    "fn(int64, int64) -> int64",
  );
  assert.equal(higherSignature.activeParameter, 1);
  checks.push(
    "Higher-order signature help follows imported factories and indexed callables in VS Code",
  );

  const completionLibrary =
    await vscode.workspace.openTextDocument(completionLibraryUri);
  await replace(
    completionLibrary,
    completionLibraryDisk.replace("value:int", "variable:int"),
  );
  values = await complete(
    completionDocument,
    imports + "fn main():\n  Crate {va|Suffix:1,other:2}\n",
    "variable",
  );
  assert.deepEqual(values.items.map(label), ["variable"]);
  await applyCompletion(
    completionDocument,
    values.items[0],
    "vaSuffix",
    "variable",
  );
  assert.match(
    hoverText(await hover(completionDocument, "variable:1")),
    /int64/,
  );
  await clearDiagnostics(completionDocument);
  assert.equal(await fs.readFile(completionUri.fsPath, "utf8"), completionDisk);
  assert.equal(
    await fs.readFile(completionLibraryUri.fsPath, "utf8"),
    completionLibraryDisk,
  );
  await replace(completionLibrary, completionLibraryDisk);
  await replace(completionDocument, completionDisk);
  checks.push(
    "Member completions follow unsaved imported types without changing disk sources",
  );

  await replace(main, mainDisk);
  const signature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "double(21)", 8),
      "(",
    );
    return result?.signatures.length ? result : undefined;
  }, "checked signature help");
  assert.equal(
    signature.signatures[0].label,
    "fn double(value: int64) -> int64",
  );
  const parameterLabel = signature.signatures[0].parameters[0].label;
  assert.equal(
    Array.isArray(parameterLabel)
      ? signature.signatures[0].label.slice(...parameterLabel)
      : parameterLabel,
    "value: int64",
  );
  assert.equal(signature.activeParameter, 0);
  checks.push(
    "Signature help exposes the checked imported function and active argument in VS Code",
  );
  await replace(
    main,
    'from "./library.k" import double\nfn main()->int:\n  double(21\n',
  );
  const recoveredSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "double(21", 9),
      "(",
    );
    return result?.signatures.length ? result : undefined;
  }, "unfinished call signature");
  assert.equal(
    recoveredSignature.signatures[0].label,
    "fn double(value: int64) -> int64",
  );
  assert.equal(recoveredSignature.activeParameter, 0);
  await eventually(
    () =>
      vscode.languages.getDiagnostics(mainUri).some((d) => d.code === "L005"),
    "original unclosed-delimiter diagnostic stays visible",
  );
  assert(main.getText().includes("double(21\n"));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  assert.match(hoverText(await hover(main, "double(21)")), /int64/);
  checks.push(
    "Trailing-parenthesis signature recovery preserves the unsaved buffer and its diagnostic in VS Code",
  );
  await replace(
    main,
    'from "./library.k" import double\nfn main()->int:\n  double(',
  );
  let partialSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "double(", 7),
      "(",
    );
    return result?.signatures.length ? result : undefined;
  }, "first missing argument signature");
  assert.equal(
    partialSignature.signatures[0].label,
    "fn double(value: int64) -> int64",
  );
  assert.equal(partialSignature.activeParameter, 0);
  await replace(
    main,
    'from "./completion-library.k" import make as factory\nfn main():\n  factory()(1,',
  );
  partialSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "factory()(1,", 12),
      ",",
    );
    return result?.signatures.length ? result : undefined;
  }, "next missing argument signature");
  assert.equal(
    partialSignature.signatures[0].label,
    "fn(int64, int64) -> int64",
  );
  assert.equal(partialSignature.activeParameter, 1);
  assert(main.getText().endsWith("factory()(1,"));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  assert.match(hoverText(await hover(main, "double(21)")), /int64/);
  checks.push(
    "Missing-argument help shows first and next parameters without inventing source values in VS Code",
  );
  await replace(
    main,
    'from "./completion-library.k" import make as factory\nfn main():\n  factory()(1, vaSuffix)\n',
  );
  const editingSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "vaSuffix", 2),
    );
    return result?.signatures.length ? result : undefined;
  }, "partly typed final identifier argument");
  assert.equal(
    editingSignature.signatures[0].label,
    "fn(int64, int64) -> int64",
  );
  assert.equal(editingSignature.activeParameter, 1);
  assert.deepEqual(
    editingSignature.signatures[0].parameters[1].label,
    [10, 15],
  );
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((d) => d.code === "S016" && d.message.includes("vaSuffix")),
    "real unknown-identifier diagnostic remains visible",
  );
  assert(main.getText().includes("vaSuffix"));
  const editingHover = await vscode.commands.executeCommand(
    "vscode.executeHoverProvider",
    mainUri,
    position(main, "vaSuffix", 2),
  );
  assert.equal(editingHover?.length || 0, 0);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  assert.match(hoverText(await hover(main, "double(21)")), /int64/);
  checks.push(
    "Signature help stays visible while editing a final identifier without changing ordinary diagnostics or hover",
  );
  await replace(
    main,
    'from "./completion-library.k" import make as factory, Box\nfn main():\n  box=Box {value:1,other:2}\n  factory()(1, box.vaSuffix)\n',
  );
  const memberSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "vaSuffix", 2),
    );
    return result?.signatures.length ? result : undefined;
  }, "partly typed final record field argument");
  assert.equal(
    memberSignature.signatures[0].label,
    "fn(int64, int64) -> int64",
  );
  assert.equal(memberSignature.activeParameter, 1);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((d) => d.code === "S068" && d.message.includes("vaSuffix")),
    "real unknown-field diagnostic remains visible",
  );
  const memberHover = await vscode.commands.executeCommand(
    "vscode.executeHoverProvider",
    mainUri,
    position(main, "vaSuffix", 2),
  );
  assert.equal(memberHover?.length || 0, 0);
  assert(main.getText().includes("box.vaSuffix"));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  checks.push(
    "Signature help preserves parameter guidance while editing a checked record receiver's field",
  );
  await replace(
    main,
    'from "./completion-library.k" import make as factory, Box\nfn main():\n  box=Box {value:1,other:2}\n  factory()(1, box.)\n',
  );
  const emptyMemberSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "box.)", 4),
    );
    return result?.signatures.length ? result : undefined;
  }, "empty final record field argument");
  assert.equal(
    emptyMemberSignature.signatures[0].label,
    "fn(int64, int64) -> int64",
  );
  assert.equal(emptyMemberSignature.activeParameter, 1);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((d) => String(d.code).startsWith("P")),
    "real syntax diagnostic remains visible for empty field",
  );
  assert(main.getText().includes("box.)"));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  checks.push(
    "Signature help survives an empty final record field without editing the source or hiding its syntax diagnostic",
  );
  const openCallCompletion = await complete(
    main,
    'from "./completion-library.k" import make as factory\nfn main():\n  value=2\n  factory()(va|',
    "value",
  );
  const valueItem = openCallCompletion.items.find(
    (item) => label(item) === "value",
  );
  await applyCompletion(main, valueItem, "va", "value");
  assert(main.getText().endsWith("factory()(value"));
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((d) => String(d.code).startsWith("L")),
    "unclosed call diagnostic stays visible after applying completion",
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  checks.push(
    "Completion applies a name inside an unfinished call without adding delimiters or hiding its actual diagnostic",
  );
  const typeCompletion = await complete(
    main,
    'from "./completion-library.k" import Box as Sample\nfn inspect(value:Sa|Suffix):\n  0\nfn main():\n  0\n',
    "Sample",
  );
  const typeItem = typeCompletion.items.find(
    (item) => label(item) === "Sample",
  );
  assert.equal(typeItem.kind, vscode.CompletionItemKind.Struct);
  await applyCompletion(main, typeItem, "SaSuffix", "Sample");
  await clearDiagnostics(main);
  assert.match(hoverText(await hover(main, "Sample):")), /type Sample/);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  checks.push(
    "Type completion applies an imported alias in an annotation and checks the resulting program",
  );
  const channelCompletion = await complete(
    main,
    'from "./completion-library.k" import Problem as Failure\nfn inspect()->int ! Fa|Suffix:\n  0\nfn main():\n  0\n',
    "Failure",
  );
  const channelItem = channelCompletion.items.find(
    (item) => label(item) === "Failure",
  );
  assert.equal(channelItem.kind, vscode.CompletionItemKind.Struct);
  await applyCompletion(main, channelItem, "FaSuffix", "Failure");
  await clearDiagnostics(main);
  assert(hoverText(await hover(main, "Failure:")));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  checks.push(
    "Error-channel completion applies an imported alias and checks the resulting fallible signature",
  );
  await clearDiagnostics(main);
  const importCompletion = await complete(
    main,
    'from "./completion-library.k" import ma|Suffix as factory\nfn main():\n  factory()(1,2)\n',
    "make",
  );
  const importItem = importCompletion.items.find(
    (item) => label(item) === "make",
  );
  assert.equal(importItem.kind, vscode.CompletionItemKind.Function);
  await applyCompletion(main, importItem, "maSuffix", "make");
  await clearDiagnostics(main);
  assert(hoverText(await hover(main, "factory()(1,2)")));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Import completion applies a public function name while preserving its local alias and checks the resulting program",
  );
  const pathCompletion = await complete(
    main,
    'from "./lib|Suffix.k" import double as twice\nfn main():\n  twice(21)\n',
    "./library.k",
  );
  const pathItem = pathCompletion.items.find(
    (item) => label(item) === "./library.k",
  );
  assert.equal(pathItem.kind, vscode.CompletionItemKind.File);
  await applyCompletion(main, pathItem, "./libSuffix.k", "./library.k");
  await clearDiagnostics(main);
  assert(hoverText(await hover(main, "twice(21)")));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Relative import-path completion replaces only the string contents, preserves the local alias and checks the resulting call",
  );
  await replace(main, 'fn main():\n  "😀abc" -> slice(0,2)\n');
  await clearDiagnostics(main);
  const builtinSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, ",2", 1),
      ",",
    );
    return result?.signatures?.[0]?.label ===
      "fn slice(input: String, start: int64, end: int64) -> String"
      ? result
      : undefined;
  }, "checked String slice signature");
  assert.equal(builtinSignature.activeParameter, 2);
  assert.equal(builtinSignature.signatures[0].parameters.length, 3);
  assert.equal(main.getText(), 'fn main():\n  "😀abc" -> slice(0,2)\n');
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Collection builtin signature help shows checked String parameters and the piped active argument without changing source",
  );
  const unfinishedConcat =
    "fn main():\n  values:[int32]=[1,2]\n  concat(values,";
  await replace(main, unfinishedConcat);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((d) => String(d.code) === "L005"),
    "actual unfinished concat diagnostic",
  );
  const concatSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      main.positionAt(unfinishedConcat.length),
      ",",
    );
    return result?.signatures?.[0]?.label ===
      "fn concat(left: [int32], right: [int32]) -> [int32]"
      ? result
      : undefined;
  }, "missing concat operand signature");
  assert.equal(concatSignature.activeParameter, 1);
  assert.equal(main.getText(), unfinishedConcat);
  assert(
    vscode.languages
      .getDiagnostics(mainUri)
      .some((d) => String(d.code) === "L005"),
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Missing concat operand guidance derives int32 arrays from the checked left operand and preserves the unfinished source and its syntax diagnostic",
  );
  const builtinCompletion = await complete(
    main,
    "fn main():\n  co|Suffix([1],[2])\n",
    "concat",
  );
  const builtinItem = builtinCompletion.items.find(
    (item) => label(item) === "concat",
  );
  assert.equal(builtinItem.kind, vscode.CompletionItemKind.Function);
  await applyCompletion(main, builtinItem, "coSuffix", "concat");
  await clearDiagnostics(main);
  const conversionCompletion = await complete(
    main,
    "fn main():\n  flo|Suffix(1)\n",
    "float32",
  );
  const conversionItem = conversionCompletion.items.find(
    (item) => label(item) === "float32",
  );
  assert.equal(conversionItem.kind, vscode.CompletionItemKind.Function);
  await applyCompletion(main, conversionItem, "floSuffix", "float32");
  await clearDiagnostics(main);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Builtin call-target completion applies collection and numeric conversion names as Function items and checks both resulting calls",
  );
  const numericSource =
    "fn main():\n  values=tensor([1,2],[1,2])\n  float32(values)\n";
  await replace(main, numericSource);
  await clearDiagnostics(main);
  const numericSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "float32(values)", 8),
      "(",
    );
    return result?.signatures?.[0]?.label ===
      "fn float32(input: Tensor[int64, 1, 2]) -> Tensor[float32, 1, 2]"
      ? result
      : undefined;
  }, "checked tensor numeric conversion signature");
  assert.equal(numericSignature.activeParameter, 0);
  assert.equal(numericSignature.signatures[0].parameters.length, 1);
  assert.equal(main.getText(), numericSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Numeric conversion signature help shows checked source/destination tensor widths and fixed shape without changing source",
  );
  const matrixSource =
    "fn main():\n  left=tensor([1,2],[1,2])\n  right=tensor([3,4],[2,1])\n  left -> matmul(right)\n";
  await replace(main, matrixSource);
  await clearDiagnostics(main);
  const matrixSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "matmul(right)", 7),
      "(",
    );
    return result?.signatures?.[0]?.label ===
      "fn matmul(left: Tensor[int64, 1, 2], right: Tensor[int64, 2, 1]) -> Tensor[int64, 1, 1]"
      ? result
      : undefined;
  }, "checked matrix shape signature");
  assert.equal(matrixSignature.activeParameter, 1);
  assert.equal(matrixSignature.signatures[0].parameters.length, 2);
  assert.equal(main.getText(), matrixSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Tensor builtin signature help shows checked matrix operands/result shapes and the piped right parameter without changing source",
  );
  const parallelSource =
    "fn main()->[int] ! TaskError:\n  offset=3\n  callback=fn(v:int):v+offset\n  try ([1,2] -> parallelMap(callback))\n";
  await replace(main, parallelSource);
  await clearDiagnostics(main);
  const parallelSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "parallelMap(callback)", 12),
      "(",
    );
    return result?.signatures?.[0]?.label ===
      "fn parallelMap(input: [int64], callback: fn(int64) -> int64) -> [int64] ! TaskError"
      ? result
      : undefined;
  }, "checked parallel callback signature");
  assert.equal(parallelSignature.activeParameter, 1);
  assert.equal(parallelSignature.signatures[0].parameters.length, 2);
  assert.equal(main.getText(), parallelSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Map signature help exposes the public callback and parallel TaskError channel while hiding captured values and preserving source",
  );
  await replace(main, parallelSource);
  await clearDiagnostics(main);
  const builtinHover = hoverText(await hover(main, "parallelMap(callback)"));
  assert.match(
    builtinHover,
    /fn parallelMap\(input: \[int64\], callback: fn\(int64\) -> int64\) -> \[int64\] ! TaskError/,
  );
  assert(!builtinHover.includes("offset"));
  await replace(main, "fn main():\n  float32(1)\n");
  await clearDiagnostics(main);
  assert.match(
    hoverText(await hover(main, "float32(1)")),
    /fn float32\(input: int64\) -> float32/,
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Builtin hovers show checked parallel callback/error and numeric conversion signatures while hiding captures and preserving disk source",
  );
  await vscode.workspace
    .getConfiguration("kalandra.native")
    .update("placement", true, vscode.ConfigurationTarget.Workspace);
  const selectorCompletion = await complete(
    main,
    "fn main():\n  target=devices.g|Suffix(1)\n  0\n",
    "gpu",
    ".",
  );
  const selectorItem = selectorCompletion.items.find(
    (item) => label(item) === "gpu",
  );
  assert.equal(selectorItem.kind, vscode.CompletionItemKind.Function);
  await applyCompletion(main, selectorItem, "gSuffix", "gpu");
  await clearDiagnostics(main);
  assert(main.getText().includes("devices.gpu(1)"));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  const transferSource =
    "fn main()->int ! TaskError:\n  try parallel:\n    values=tensor([1,2])\n    resident=try await transfer(values,devices.gpu(1))\n    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n";
  const transferCompletions = await complete(
    main,
    transferSource.replace("transfer(values", "tra|Suffix(values"),
    "transfer",
  );
  const transferItem = transferCompletions.items.find(
    (item) => label(item) === "transfer",
  );
  assert.equal(transferItem.kind, vscode.CompletionItemKind.Function);
  await applyCompletion(main, transferItem, "traSuffix", "transfer");
  await clearDiagnostics(main);
  assert.equal(main.getText(), transferSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  checks.push(
    "Placement-mode transfer completion applies only the call name inside an owning parallel group and checks the resulting program without probing hardware",
  );
  const transferLabel =
    "fn transfer(input: Tensor[int64, 2], target: Device[gpu:1]) -> task<Tensor[int64, 2] on gpu:1>";
  assert(
    hoverText(await hover(main, "transfer(values")).includes(transferLabel),
  );
  const transferSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "transfer(values,devices.gpu(1))", 16),
      ",",
    );
    return result?.signatures?.[0]?.label === transferLabel
      ? result
      : undefined;
  }, "checked transfer signature");
  assert.equal(transferSignature.activeParameter, 1);
  assert.equal(transferSignature.signatures[0].parameters.length, 2);
  assert(
    hoverText(await hover(main, "transfer(resident")).includes(
      "fn transfer(input: Tensor[int64, 2] on gpu:1, target: Device[cpu]) -> task<Tensor[int64, 2]>",
    ),
  );
  assert.equal(main.getText(), transferSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  checks.push(
    "Transfer hover and signature help expose checked tensor shapes, exact GPU/CPU residency and task results without hardware probes or source changes",
  );
  const gpuSelectorLabel = "fn devices.gpu(index: uint32) -> Device[gpu:1]";
  assert(
    hoverText(await hover(main, "devices.gpu(1)")).includes(gpuSelectorLabel),
  );
  assert(
    hoverText(await hover(main, "devices.cpu()")).includes(
      "fn devices.cpu() -> Device[cpu]",
    ),
  );
  const gpuSelectorSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "devices.gpu(1)", 12),
      "(",
    );
    return result?.signatures?.[0]?.label === gpuSelectorLabel
      ? result
      : undefined;
  }, "checked explicit GPU selector signature");
  assert.equal(gpuSelectorSignature.activeParameter, 0);
  assert.equal(gpuSelectorSignature.signatures[0].parameters.length, 1);
  const defaultSelectorSource = "fn main():\n  target=devices.gpu()\n  0\n";
  await replace(main, defaultSelectorSource);
  await clearDiagnostics(main);
  assert(
    hoverText(await hover(main, "devices.gpu()")).includes(
      "fn devices.gpu() -> Device[gpu:0]",
    ),
  );
  assert.equal(main.getText(), defaultSelectorSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  checks.push(
    "Checked device-selector hover and signature help show CPU, default GPU and explicit uint32 index forms without hardware probes or source writes",
  );
  await replace(main, mainDisk);
  await vscode.workspace
    .getConfiguration("kalandra.native")
    .update("placement", false, vscode.ConfigurationTarget.Workspace);
  await clearDiagnostics(main);
  await hover(main, "double(21)");
  checks.push(
    "Placement-mode device-selector completion edits only the member, preserves the explicit index and checks without probing hardware",
  );
  const openCalleeSource =
    "fn double(value:int):\n  value*2\nfn main():\n  dou|Suffix(21\n";
  const openCalleeCompletion = await complete(main, openCalleeSource, "double");
  const openCalleeItem = openCalleeCompletion.items.find(
    (item) => label(item) === "double",
  );
  assert.equal(openCalleeItem.kind, vscode.CompletionItemKind.Function);
  await applyCompletion(main, openCalleeItem, "douSuffix", "double");
  const appliedOpenCallee = openCalleeSource.replace("dou|Suffix", "double");
  assert.equal(main.getText(), appliedOpenCallee);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "real unclosed call diagnostic after callee completion",
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, appliedOpenCallee.trimEnd() + ")\n");
  await clearDiagnostics(main);
  assert(hoverText(await hover(main, "double(21)")).includes("int64"));
  checks.push(
    "Callee completion inside an unclosed argument list preserves arguments and the actual delimiter diagnostic, then checks after the user closes the call",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const openArgumentSource =
    "type Reading:\n  value:int\nfn combine(left:int,right:int):\n  left+right\nfn main():\n  reading=Reading {value:2}\n  combine(1,reading.va|Suffix\n";
  const openArgumentCompletion = await complete(
    main,
    openArgumentSource,
    "value",
  );
  const openArgumentItem = openArgumentCompletion.items.find(
    (item) => label(item) === "value",
  );
  assert.equal(openArgumentItem.kind, vscode.CompletionItemKind.Field);
  await applyCompletion(main, openArgumentItem, "vaSuffix", "value");
  const appliedOpenArgument = openArgumentSource.replace("va|Suffix", "value");
  assert.equal(main.getText(), appliedOpenArgument);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "real delimiter diagnostic after final argument completion",
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, appliedOpenArgument.trimEnd() + ")\n");
  await clearDiagnostics(main);
  assert(hoverText(await hover(main, "value)")).includes("int64"));
  checks.push(
    "Final argument field completion replaces the full suffix in an open call while preserving source, receiver and real delimiter diagnostics until explicit closure",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const suffixSignatureSource =
    "fn combine(left:int,right:int):\n  left+right\nfn main():\n  value=2\n  combine(1,vaSuffix\n";
  await replace(main, suffixSignatureSource);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "unclosed suffix signature source diagnostic",
  );
  const suffixSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "vaSuffix", 2),
      ",",
    );
    return result?.signatures?.[0]?.label ===
      "fn combine(left: int64, right: int64) -> int64"
      ? result
      : undefined;
  }, "signature help inside final name suffix before missing closing parenthesis");
  assert.equal(suffixSignature.activeParameter, 1);
  assert.equal(suffixSignature.signatures[0].parameters.length, 2);
  assert.equal(main.getText(), suffixSignatureSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  checks.push(
    "Signature help inside a final argument suffix preserves the open source and real delimiter diagnostic while showing validated callee parameters",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const earlierArgumentSource =
    "type Reading:\n  value:int\nfn triple(first:int,second:int,third:int):\n  first+second+third\nfn main():\n  reading=Reading {value:1}\n  triple(reading.va|Suffix,2\n";
  const earlierArgumentCompletion = await complete(
    main,
    earlierArgumentSource,
    "value",
  );
  const earlierArgumentItem = earlierArgumentCompletion.items.find(
    (item) => label(item) === "value",
  );
  assert.equal(earlierArgumentItem.kind, vscode.CompletionItemKind.Field);
  await applyCompletion(main, earlierArgumentItem, "vaSuffix", "value");
  const appliedEarlierArgument = earlierArgumentSource.replace(
    "va|Suffix",
    "value",
  );
  assert.equal(main.getText(), appliedEarlierArgument);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "earlier argument edit preserves real delimiter diagnostic",
  );
  await replace(main, appliedEarlierArgument.trimEnd() + ")\n");
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "S027"),
    "ordinary missing third argument diagnostic",
  );
  await replace(main, appliedEarlierArgument.trimEnd() + ",3)\n");
  await clearDiagnostics(main);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  checks.push(
    "Earlier argument field completion preserves later supplied arguments and missing-parameter diagnostics, then checks after the final argument is supplied",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const earlierSignatureSource =
    "fn triple(first:int,second:int,third:int):\n  first+second+third\nfn main():\n  triple(vaSuffix,2\n";
  await replace(main, earlierSignatureSource);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "earlier signature source retains actual delimiter diagnostic",
  );
  const earlierSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "vaSuffix", 2),
      "(",
    );
    return result?.signatures?.[0]?.label ===
      "fn triple(first: int64, second: int64, third: int64) -> int64"
      ? result
      : undefined;
  }, "checked earlier signature slot before later supplied and missing arguments");
  assert.equal(earlierSignature.activeParameter, 0);
  assert.equal(earlierSignature.signatures[0].parameters.length, 3);
  assert.equal(main.getText(), earlierSignatureSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  checks.push(
    "Earlier argument signature help preserves later supplied arguments, missing parameters, real diagnostics and source while showing the checked active slot",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const emptyEarlierSource =
    "type Reading:\n  value:int\nfn combine(left:int,right:int):\n  left+right\nfn main():\n  reading=Reading {value:2}\n  combine(reading.,2)\n";
  await replace(main, emptyEarlierSource);
  await eventually(
    () => vscode.languages.getDiagnostics(mainUri).length > 0,
    "actual missing field parser diagnostic before comma",
  );
  const emptyEarlierSignature = await eventually(async () => {
    const result = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "reading.,", 8),
      "(",
    );
    return result?.signatures?.[0]?.label ===
      "fn combine(left: int64, right: int64) -> int64"
      ? result
      : undefined;
  }, "checked signature for an empty earlier field before comma");
  assert.equal(emptyEarlierSignature.activeParameter, 0);
  assert(!emptyEarlierSignature.signatures[0].label.includes("__kalandra"));
  assert.equal(main.getText(), emptyEarlierSource);
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(
    main,
    emptyEarlierSource.replace("reading.,", "reading.value,"),
  );
  await clearDiagnostics(main);
  checks.push(
    "Empty earlier field signature help validates the receiver and preserves real parser diagnostics and source until an actual field is supplied",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const expressionCompletionSource =
    "fn combine(left:int,right:int):\n  left+right\nfn main():\n  value=2\n  combine(1,2*va|Suffix\n";
  const expressionCompletions = await complete(
    main,
    expressionCompletionSource,
    "value",
  );
  const expressionItem = expressionCompletions.items.find(
    (item) => label(item) === "value",
  );
  assert.equal(expressionItem.kind, vscode.CompletionItemKind.Variable);
  await applyCompletion(main, expressionItem, "vaSuffix", "value");
  const appliedExpression = expressionCompletionSource.replace(
    "va|Suffix",
    "value",
  );
  assert.equal(main.getText(), appliedExpression);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "arithmetic completion retains real unclosed call diagnostic",
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, appliedExpression.trimEnd() + ")\n");
  await clearDiagnostics(main);
  checks.push(
    "Arithmetic operand completion preserves expression text and real delimiter diagnostics, then checks after the user closes the call",
  );
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  const recoveryBase =
    "fn add(left:int,right:int)->int:\n  left+right\nfn main():\n  value=2\n  values=[1,2]\n";
  for (const body of [
    "values[va|Suffix",
    "if true:\n    va|Suffix",
    "map(values,fn(item:int): va|Suffix",
  ]) {
    const source = recoveryBase + "  " + body + "\n";
    const items = await complete(main, source, "value");
    const item = items.items.find((entry) => label(entry) === "value");
    await eventually(
      () => vscode.languages.getDiagnostics(mainUri).length > 0,
      "unfinished source diagnostics",
    );
    await applyCompletion(main, item, "vaSuffix", "value");
    assert.equal(main.getText(), source.replace("va|Suffix", "value"));
    assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  }
  const compoundSource = recoveryBase + "  add(1,values[vaSuffix\n";
  await replace(main, compoundSource);
  const compoundHint = await eventually(async () => {
    const hint = await vscode.commands.executeCommand(
      "vscode.executeSignatureHelpProvider",
      mainUri,
      position(main, "vaSuffix", 2),
      "(",
    );
    return hint?.signatures?.length ? hint : undefined;
  }, "compound unfinished signature");
  assert.equal(compoundHint.activeParameter, 1);
  assert.match(compoundHint.signatures[0].label, /right: int64/);
  assert.equal(main.getText(), compoundSource);
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some((item) => item.code === "L005"),
    "compound signature retains actual diagnostics",
  );
  const keywordItems = await complete(main, recoveryBase + "  tru|\n", "true");
  assert.equal(
    keywordItems.items.find((item) => label(item) === "true").kind,
    vscode.CompletionItemKind.Keyword,
  );
  const islandSource =
    "fn broken():\n  add(1,\n" + recoveryBase + "  va|Suffix\n";
  await complete(main, islandSource, "value");
  assert.equal(main.getText(), islandSource.replace("|", ""));
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  await replace(main, mainDisk);
  await clearDiagnostics(main);
  checks.push(
    "Structural unfinished-code completion and compound signatures preserve diagnostics, suffix ranges and disk content across collection/control/closure holes and independent broken declarations; keywords use native completion kinds",
  );
  const library = await vscode.workspace.openTextDocument(libraryUri);
  await replace(
    library,
    "export fn double(value:float32)->float32:\n  value*2\n",
  );
  await eventually(
    () =>
      vscode.languages
        .getDiagnostics(mainUri)
        .some(
          (diagnostic) =>
            diagnostic.severity === vscode.DiagnosticSeverity.Error,
        ),
    "unsaved dependency diagnostics",
  );
  await replace(library, libraryDisk);
  await clearDiagnostics(main);
  checks.push(
    "Unsaved dependency edits recheck callers and clearing errors updates diagnostics",
  );

  const unformatted =
    'from "./library.k" import double\r\n# 😀\r\nfn main( )->int:\r\n    answer=double(21)\r\n    answer';
  await replace(main, unformatted);
  const edits = await vscode.commands.executeCommand(
    "vscode.executeFormatDocumentProvider",
    mainUri,
    { tabSize: 4, insertSpaces: true },
  );
  assert(edits.length > 0); // VS Code can minimize the server's whole-document edit.
  const formatted = new vscode.WorkspaceEdit();
  formatted.set(mainUri, edits);
  assert(await vscode.workspace.applyEdit(formatted));
  assert.equal(
    main.getText(),
    'from "./library.k" import double\n# 😀\nfn main() -> int:\n  answer = double(21)\n  answer\n',
  );
  await clearDiagnostics(main);
  assert.equal(
    (
      await vscode.commands.executeCommand(
        "vscode.executeFormatDocumentProvider",
        mainUri,
        { tabSize: 2, insertSpaces: true },
      )
    )?.length || 0,
    0,
  );
  assert.equal(await fs.readFile(mainUri.fsPath, "utf8"), mainDisk);
  assert.equal(await fs.readFile(libraryUri.fsPath, "utf8"), libraryDisk);
  checks.push(
    "Format Document applies Unicode/CRLF edits, is idempotent and leaves disk files unchanged",
  );

  await replace(main, mainDisk.replace("./library.k", "math"));
  await vscode.workspace
    .getConfiguration("kalandra.native")
    .update(
      "projectPath",
      path.join(workspace, "kalandra.toml"),
      vscode.ConfigurationTarget.Workspace,
    );
  await vscode.commands.executeCommand("kalandra.native.restart");
  await clearDiagnostics(main);
  assert.match(hoverText(await hover(main, "double(21)")), /int64/);
  checks.push(
    "Explicit project configuration restarts the native server and resolves package imports",
  );
  await vscode.commands.executeCommand("kalandra.native.restart");
  assert.match(hoverText(await hover(main, "double(21)")), /int64/);
  checks.push("Manual server restart restores checked editor features");
  await fs.writeFile(
    process.env.KALANDRA_EDITOR_RESULT,
    JSON.stringify(
      {
        vscode: vscode.version,
        platform: process.platform,
        architecture: process.arch,
        extension: extension.packageJSON.version,
        checks,
      },
      null,
      2,
    ) + "\n",
  );
  console.log(
    `Kalandra native editor: ${checks.length} real extension-host checks passed.`,
  );
}
module.exports = {
  run: async () => {
    let timer;
    try {
      await Promise.race([
        run(),
        new Promise((_, reject) => {
          timer = setTimeout(
            () => reject(new Error("Native editor test exceeded 90 seconds")),
            90000,
          );
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  },
};
