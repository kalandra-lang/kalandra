// Lexical display highlighting for native .k source. This is deliberately not
// a parser or a dependency on the retired prototype compiler.
const keywords = new Set(
  "fn export from import as type enum error if else match not and or true false parallel spawn await try catch fail on with".split(
    " ",
  ),
);
const numericTypes = new Set(
  "int uint float int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64 Bool String Tensor TaskError FaultKind".split(
    " ",
  ),
);
const colors = {
  keyword: ["#b53a17", "#f86737"],
  string: ["#5e492f", "#d6c7b5"],
  number: ["#8a3218", "#f4ac91"],
  identifier: ["#242424", "#f6f6f6"],
  punctuation: ["#333333", "#b5b5b5"],
  comment: ["#626262", "#999999"],
} as const;
export function escapeCode(text: string) {
  return text.replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ]!,
  );
}
/** Decode one layer only: source text such as '&lt;' must remain literal. */
export function decodeCode(text: string) {
  return text.replace(
    /&(#x[0-9a-f]+|#\d+|amp|lt|gt|quot);/gi,
    (original, entity: string) => {
      if (entity.startsWith("#")) {
        const hex = entity.charAt(1).toLowerCase() === "x";
        const point = parseInt(entity.slice(hex ? 2 : 1), hex ? 16 : 10);
        return point <= 0x10ffff ? String.fromCodePoint(point) : original;
      }
      return { amp: "&", lt: "<", gt: ">", quot: '"' }[entity.toLowerCase()]!;
    },
  );
}
export function highlightKalandra(text: string) {
  const spans: { start: number; end: number; kind: keyof typeof colors }[] = [];
  const pattern =
    /#[^\r\n]*|"(?:\\[\s\S]|[^"\\])*"?|\b[A-Za-z_][A-Za-z_0-9]*\b|\b[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\b|->|==|!=|<=|>=|[+*/%=<>!:[\]{}(),.-]/g;
  for (const match of text.matchAll(pattern)) {
    const token = match[0];
    const kind: keyof typeof colors = token.startsWith("#")
      ? "comment"
      : token.startsWith('"')
        ? "string"
        : keywords.has(token)
          ? "keyword"
          : /^\d/.test(token)
            ? "number"
            : numericTypes.has(token) || /^[A-Za-z_]/.test(token)
              ? "identifier"
              : "punctuation";
    spans.push({ start: match.index!, end: match.index! + token.length, kind });
  }
  spans.sort((a, b) => a.start - b.start || a.end - b.end);
  let cursor = 0,
    html = "";
  for (const span of spans) {
    if (span.start < cursor) continue;
    const [light, dark] = colors[span.kind];
    html +=
      escapeCode(text.slice(cursor, span.start)) +
      `<span class="kalandra-token" style="--shiki-light:${light};--shiki-dark:${dark}">${escapeCode(text.slice(span.start, span.end))}</span>`;
    cursor = span.end;
  }
  return html + escapeCode(text.slice(cursor));
}

/** Tiramisu emits escaped code constants before Svelte compiles the page. */
export function highlightDocumentation(svelte: string) {
  const names = new Set(
    Array.from(
      svelte.matchAll(
        /<CodeBlock\s+language="kalandra"\s+code=\{(__code_\d+)\}/g,
      ),
      (m) => m[1],
    ),
  );
  return svelte.replace(
    /const (__code_\d+|homepageCode)\s*=\s*("(?:[^"\\]|\\.)*")/g,
    (original, name: string, json: string) => {
      if (!names.has(name) && name !== "homepageCode") return original;
      const value: string = JSON.parse(json);
      // Tiramisu's unknown-language fallback may already wrap lines in plain spans.
      const raw =
        name === "homepageCode"
          ? value
          : decodeCode(value.replace(/<[^>]*>/g, ""));
      return `const ${name} = ${JSON.stringify(highlightKalandra(raw))}`;
    },
  );
}
export function kalandraHighlightPlugin() {
  return {
    name: "kalandra-documentation-highlighting",
    enforce: "pre" as const,
    transform(source: string, id: string) {
      // Kit 0.1.x strips highlighting tags but leaves HTML entities in the
      // clipboard. Read the rendered text so operators and strings copy exactly.
      if (id.replaceAll("\\", "/").endsWith("/tiramisu/CodeBlock.svelte")) {
        return {
          code: source.replace(
            'const text = code.replace(/<[^>]*>/g, "")',
            'const template = document.createElement("template"); template.innerHTML = code; const text = template.content.textContent ?? ""',
          ),
          map: null,
        };
      }
      if (
        !id.endsWith(".tiramisu.svelte") &&
        !id.endsWith("/src/routes/+page.svelte")
      )
        return;
      return { code: highlightDocumentation(source), map: null };
    },
  };
}
