const { test, expect } = require("bun:test");
const fs = require("node:fs/promises");
const path = require("node:path");
const { Registry, parseRawGrammar, INITIAL } = require("vscode-textmate");
const { loadWASM, OnigScanner, OnigString } = require("vscode-oniguruma");
async function grammar() {
  const bytes = await fs.readFile(
    require.resolve("vscode-oniguruma/release/onig.wasm"),
  );
  await loadWASM(
    bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  );
  const registry = new Registry({
    onigLib: Promise.resolve({
      createOnigScanner: (sources) => new OnigScanner(sources),
      createOnigString: (value) => new OnigString(value),
    }),
    loadGrammar: async () =>
      parseRawGrammar(
        await fs.readFile(
          path.join(__dirname, "../syntaxes/kalandra.tmLanguage.json"),
          "utf8",
        ),
        "kalandra.json",
      ),
  });
  return registry.loadGrammar("source.kalandra");
}
test("native grammar colors keywords, types, operators, numbers and comments", async () => {
  const g = await grammar();
  const line = "fn double(value: int32) -> int32: value * 2 # comment";
  const result = g.tokenizeLine(line, INITIAL);
  const scope = (needle) =>
    result.tokens
      .find(
        (token) =>
          token.startIndex <= line.indexOf(needle) &&
          token.endIndex > line.indexOf(needle),
      )
      .scopes.join(" ");
  expect(scope("fn")).toContain("keyword.declaration");
  expect(scope("double")).toContain("entity.name.function");
  expect(scope("int32")).toContain("storage.type");
  expect(scope("->")).toContain("keyword.operator");
  expect(scope("2 #")).toContain("constant.numeric");
  expect(scope("# comment")).toContain("comment.line");
});
test("string contents suppress keywords/comments and escaped quotes stay in the String", async () => {
  const g = await grammar();
  const line = '"fn # not a comment \\" true" # comment';
  const result = g.tokenizeLine(line, INITIAL);
  const inside = result.tokens.filter(
    (token) => token.startIndex < line.lastIndexOf('"'),
  );
  expect(
    inside.every((token) =>
      token.scopes.includes("string.quoted.double.kalandra"),
    ),
  ).toBe(true);
  expect(result.tokens.at(-1).scopes).toContain(
    "comment.line.number-sign.kalandra",
  );
  expect(
    g.tokenizeLine(
      "fn main():",
      g.tokenizeLine('"unfinished', INITIAL).ruleStack,
    ).tokens[0].scopes,
  ).toContain("keyword.declaration.kalandra");
});
