<script lang="ts">
  import { onMount, tick, type ComponentProps } from "svelte";
  import { afterNavigate } from "$app/navigation";
  import {
    DocsLayout as KitLayout,
    Sidebar,
    TableOfContents,
    SearchDialog,
  } from "@tiramisu-docs/kit/components";

  let {
    config,
    sidebar,
    headings,
    children,
    sections,
    locale,
    locales,
    showFallbackBanner = false,
  }: ComponentProps<typeof KitLayout> = $props();
  let menuOpen = $state(false);
  let searchOpen = $state(false);
  let dark = $state(true);
  let menuButton: HTMLButtonElement;
  let searchButton: HTMLButtonElement;
  let searchLayer = $state<HTMLDivElement>();
  function searchKeydown(event: KeyboardEvent) {
    if (event.key !== "Tab" || !searchLayer) return;
    const controls = Array.from(
      searchLayer.querySelectorAll<HTMLElement>(
        'button:not(:disabled), input:not(:disabled), a[href], [tabindex="0"]',
      ),
    ).filter((el) => el.getClientRects().length);
    const first = controls[0];
    const last = controls[controls.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last?.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first?.focus();
    }
  }

  onMount(() => {
    dark = document.documentElement.classList.contains("dark");
  });
  afterNavigate(() => {
    menuOpen = false;
  });
  function toggleTheme() {
    dark = !dark;
    document.documentElement.classList.toggle("dark", dark);
    try {
      localStorage.setItem("theme", dark ? "dark" : "light");
    } catch {
      /* The current session still keeps its theme. */
    }
  }
  function openSearch() {
    menuOpen = false;
    searchOpen = true;
  }
  function keydown(event: KeyboardEvent) {
    if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      if (searchOpen) {
        searchOpen = false;
        searchButton?.focus();
      } else openSearch();
    } else if (event.key === "Escape" && menuOpen) {
      menuOpen = false;
      menuButton?.focus();
    }
  }
  let wasSearchOpen = false;
  $effect(() => {
    if (wasSearchOpen && !searchOpen) {
      tick().then(() => searchButton?.focus());
    }
    wasSearchOpen = searchOpen;
  });
</script>

<svelte:window onkeydown={keydown} />
<div class="docs-site">
  <a class="docs-skip" href="#docs-main">Skip to content</a>
  <header class="docs-header">
    <div class="docs-header-inner">
      <a class="docs-brand" href="/" aria-label="Kalandra home"
        ><span aria-hidden="true"></span>{config.title}</a
      >
      <a class="docs-label" href="/docs">Documentation</a>
      <nav class="docs-header-links" aria-label="Main navigation">
        <a href="/docs/getting-started">Get started</a>
        <a href="/docs/gpu">GPU computing</a>
        <a href="https://github.com/kalandra-lang/kalandra">GitHub</a>
      </nav>
      <button
        class="docs-search"
        bind:this={searchButton}
        onclick={openSearch}
        aria-label="Search documentation"
      >
        <svg
          width="17"
          height="17"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="1.5"
          aria-hidden="true"
          ><circle cx="10.5" cy="10.5" r="6.5" /><path d="m16 16 5 5" /></svg
        >
        <span>Search</span><kbd><span aria-hidden="true">⌘</span> K</kbd>
      </button>
      <button
        class="docs-theme"
        onclick={toggleTheme}
        aria-label={dark ? "Switch to light mode" : "Switch to dark mode"}
      >
        <svg
          width="19"
          height="19"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="1.5"
          aria-hidden="true"
        >
          {#if dark}<circle cx="12" cy="12" r="4" /><path
              d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"
            />{:else}<path
              d="M20.5 14.5A8.5 8.5 0 0 1 9.5 3.5a8.5 8.5 0 1 0 11 11Z"
            />{/if}
        </svg>
      </button>
      <button
        class="docs-menu"
        bind:this={menuButton}
        aria-expanded={menuOpen}
        aria-controls="docs-navigation"
        onclick={() => (menuOpen = !menuOpen)}
        >{menuOpen ? "Close" : "Menu"}
        <svg
          width="18"
          height="18"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="1.5"
          aria-hidden="true"
          >{#if menuOpen}<path d="m6 6 12 12M6 18 18 6" />{:else}<path
              d="M4 8h16M4 16h16"
            />{/if}</svg
        >
      </button>
    </div>
    {#if sections?.length}
      <nav class="docs-sections" aria-label="Documentation sections">
        {#each sections as section}<a
            href={section.href ??
              (section.path ? `/docs/${section.path}` : "/docs")}
            >{section.label}</a
          >{/each}
      </nav>
    {/if}
  </header>
  {#if showFallbackBanner}<p class="docs-fallback">
      This page is not available in the selected language. Showing the default
      version.
    </p>{/if}
  <div class="docs-grid">
    <aside
      id="docs-navigation"
      class="docs-sidebar"
      class:mobile-open={menuOpen}
      aria-label="Documentation navigation"
    >
      <Sidebar {config} groups={sidebar} {locale} {locales} />
    </aside>
    <div class="docs-reading-column">
      <main id="docs-main" tabindex="-1">{@render children()}</main>
      <footer class="docs-footer">
        <span>{config.footer?.copyright}</span><a
          href="https://github.com/kalandra-lang/kalandra">GitHub</a
        ><a href="https://tiramisudocs.com/">Powered by Tiramisu</a>
      </footer>
    </div>
    {#if headings?.length}
      <aside class="docs-toc" aria-label="On this page">
        <TableOfContents {headings} />
      </aside>
    {/if}
  </div>
</div>
{#if searchOpen}
  <div
    class="docs-search-layer"
    role="dialog"
    aria-modal="true"
    aria-label="Search documentation"
    tabindex="-1"
    bind:this={searchLayer}
    onkeydown={searchKeydown}
  >
    <SearchDialog bind:open={searchOpen} {locale} />
  </div>
{/if}
