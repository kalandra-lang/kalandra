import { generateLlmsFullTxt } from "@tiramisu-docs/kit/seo";
import { searchIndex, locales } from "virtual:tiramisu-docs";
import config from "#lib/tiramisu.config.ts";
import { resolveConfig } from "@tiramisu-docs/kit";

export const prerender = true;

export function GET() {
  const resolved = resolveConfig(config);
  const baseUrl = resolved.url ?? "https://example.com";
  if (locales) {
    const parts = Object.entries(locales).map(([code, data]) =>
      generateLlmsFullTxt(data.searchIndex, {
        title: resolved.title,
        description: resolved.description,
        baseUrl,
        basePath: `/docs/${code}`,
      }),
    );
    return new Response(parts.join("\n"), {
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
  const txt = generateLlmsFullTxt(searchIndex, {
    title: resolved.title,
    description: resolved.description,
    baseUrl,
  });
  return new Response(txt, {
    headers: { "Content-Type": "text/plain; charset=utf-8" },
  });
}
