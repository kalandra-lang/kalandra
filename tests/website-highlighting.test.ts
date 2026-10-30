import { test, expect } from "bun:test";
import {
  highlightKalandra,
  highlightDocumentation,
  decodeCode,
} from "../scripts/website-highlighting.js";
const plain = (html: string) => decodeCode(html.replace(/<[^>]*>/g, ""));

test("website highlighting preserves source and escapes HTML, including literal entities", () => {
  for (const source of [
    "",
    "# λ comment\r\nfn main() -> int:\r\n  40 + 2\r\n",
    'fn tag: "<script>&lt;&amp;\\\"</script>"\n',
    "export fn main() -> int ! TaskError:\n  try parallel:\n    try await spawn double(21)\n",
    '"unterminated',
  ]) {
    const html = highlightKalandra(source);
    expect(plain(html)).toBe(source);
    expect(html).not.toContain("<script>");
  }
  expect(highlightKalandra("fn f: 42")).toContain("--shiki-dark:");
});

test("Tiramisu adapter preserves fallback spans and targets only Kalandra constants", () => {
  const source = 'fn f: "&lt;<b>"\n';
  const fallback =
    '<span class="line">fn f: &quot;&amp;lt;&lt;b&gt;&quot;</span>\n';
  const input = `const __code_0 = ${JSON.stringify(fallback)};\nconst __code_1 = "untouched";\n<CodeBlock language="kalandra" code={__code_0} />\n<CodeBlock language="bash" code={__code_1} />`;
  const output = highlightDocumentation(input);
  const html = JSON.parse(
    output.match(/const __code_0 = ("(?:[^"\\]|\\.)*")/)![1]!,
  );
  expect(plain(html)).toBe(source);
  expect(output).toContain('const __code_1 = "untouched";');
  expect(highlightDocumentation('const homepageCode =\n "fn f: 1";')).toContain(
    "kalandra-token",
  );
});

test("Tiramisu numeric entities decode exactly once for comparisons and literal strings", () => {
  const source = 'if value <= 1: "&#x3C;"';
  const fallback = "if value &#x3C;= 1: &quot;&amp;#x3C;&quot;";
  const output = highlightDocumentation(
    `const __code_0 = ${JSON.stringify(fallback)};\n<CodeBlock language="kalandra" code={__code_0} />`,
  );
  const html = JSON.parse(
    output.match(/const __code_0 = ("(?:[^"\\]|\\.)*")/)![1]!,
  );
  expect(plain(html)).toBe(source);
  expect(decodeCode("&#60;&#x3E;&#39;")).toBe("<>'");
});
