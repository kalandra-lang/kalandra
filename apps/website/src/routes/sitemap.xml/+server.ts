import { generateSitemap } from "@tiramisu-docs/kit/seo";
import { docs, locales } from "virtual:tiramisu-docs";
import config from "#lib/tiramisu.config.ts";
import { resolveConfig } from "@tiramisu-docs/kit";

export const prerender = true;

export function GET() {
  const resolved = resolveConfig(config);
  const baseUrl = resolved.url ?? "https://example.com";
  const allDocs = locales
    ? Object.entries(locales).flatMap(([code, data]) =>
        data.docs.map((d) => ({ ...d, slug: `${code}/${d.slug}` })),
      )
    : docs;
  const xml = generateSitemap(allDocs, {
    baseUrl,
    locales: resolved.i18n?.locales,
    defaultLocale: resolved.i18n?.defaultLocale,
  });
  return new Response(xml, { headers: { "Content-Type": "application/xml" } });
}
