<script lang="ts">
  import { DocsLayout, DocPage, PrevNextNav } from "$lib/components";
  import OpenDropdown from "$lib/components/OpenDropdown.svelte";
  import config from "$lib/tiramisu.config";
  import { socialImage } from "$lib/social";
  import { resolveConfig, getOpenLinks } from "@tiramisu-docs/kit";
  import type { LocaleConfig } from "@tiramisu-docs/kit";

  let { data }: { data: Record<string, any> } = $props();

  const resolved = resolveConfig(config);
  const links = $derived(
    resolved.url && data.slug
      ? getOpenLinks({
          baseUrl: resolved.url,
          slug: data.slug,
          locale: data.locale,
          github: resolved.github,
          mcp: resolved.mcp,
          title: resolved.title,
        })
      : [],
  );
</script>

<svelte:head>
  {#if !data.meta?.image}
    <meta property="og:image:type" content="image/png" />
    <meta property="og:image:width" content={String(socialImage.width)} />
    <meta property="og:image:height" content={String(socialImage.height)} />
    <meta property="og:image:alt" content={socialImage.alt} />
    <meta name="twitter:image:alt" content={socialImage.alt} />
  {/if}
</svelte:head>

<DocsLayout
  config={resolved}
  sidebar={data.activeSidebar}
  headings={data.headings}
  sections={data.sections}
  locale={data.locale}
  locales={data.locales
    ? resolved.i18n?.locales?.filter((l: LocaleConfig) =>
        data.locales.includes(l.code),
      )
    : undefined}
  showFallbackBanner={data.showFallbackBanner}
>
  <DocPage
    meta={{
      ...data.meta,
      description: data.meta?.description || resolved.description,
      image: data.meta?.image || socialImage.path,
    }}
    lastEdited={data.lastEdited}
    slug={data.slug}
    baseUrl={resolved.url}
    sidebar={data.activeSidebar}
    siteName={resolved.title}
    instantOg={resolved.instantOg}
  >
    {#snippet headerActions()}
      <OpenDropdown {links} markdown={data.markdown} />
    {/snippet}
    {#if data.component}
      {@const Component = data.component}
      <Component />
    {/if}
  </DocPage>
  <PrevNextNav
    sidebar={data.activeSidebar}
    currentSlug={data.slug}
    locale={data.locale}
  />
</DocsLayout>
