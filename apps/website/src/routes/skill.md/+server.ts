import { generateSkillMd } from "@tiramisu-docs/kit/seo";
import { docs, locales } from "virtual:tiramisu-docs";
import config from "#lib/tiramisu.config.ts";
import { resolveConfig } from "@tiramisu-docs/kit";

export const prerender = true;

export function GET() {
  const resolved = resolveConfig(config);
  if (locales) {
    const parts = Object.entries(locales).map(([code, data]) =>
      generateSkillMd(data.docs, {
        title: `${resolved.title} (${code})`,
        description: resolved.description,
      }),
    );
    return new Response(parts.join("\n\n---\n\n"), {
      headers: { "Content-Type": "text/markdown; charset=utf-8" },
    });
  }
  const md = generateSkillMd(docs, {
    title: resolved.title,
    description: resolved.description,
  });
  return new Response(md, {
    headers: { "Content-Type": "text/markdown; charset=utf-8" },
  });
}
