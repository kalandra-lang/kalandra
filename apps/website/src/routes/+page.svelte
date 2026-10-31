<script lang="ts">
  import { onDestroy } from "svelte";
  import type { PageData } from "./$types";
  import ExecutionField from "$lib/components/ExecutionField.svelte";
  import { socialImage } from "$lib/social";
  let { data }: { data: PageData } = $props();
  let selected = $state(0);
  let copied = $state(false);
  let copyError = $state(false);
  let copyTimer: ReturnType<typeof setTimeout> | undefined;
  const example = $derived(data.examples[selected]);
  function resetCopy() {
    if (copyTimer) clearTimeout(copyTimer);
    copied = false;
    copyError = false;
  }
  async function copySource() {
    resetCopy();
    try {
      await navigator.clipboard.writeText(example.source);
      copied = true;
    } catch {
      copyError = true;
    }
    copyTimer = setTimeout(resetCopy, 2500);
  }
  function tokens(source: string) {
    return source
      .split(/(\b(?:fn|try|parallel|spawn|on|await|if|else)\b|->)/g)
      .map((text) => ({
        text,
        keyword: /^(fn|try|parallel|spawn|on|await|if|else|->)$/.test(text),
      }));
  }
  onDestroy(() => {
    if (copyTimer) clearTimeout(copyTimer);
  });
  let menuOpen = $state(false);
  let menuButton: HTMLButtonElement;
  function handleNavigationKey(event: KeyboardEvent) {
    if (event.key === "Escape" && menuOpen) {
      menuOpen = false;
      menuButton?.focus();
    }
  }
  const repo = "https://github.com/kalandra-lang/kalandra";
</script>

{#snippet arrow(external = false)}
  <svg
    width="24"
    height="24"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    aria-hidden="true"
  >
    {#if external}<path d="M6 18 18 6M6 6h12v12" />{:else}<path
        d="M4 12h15m-6-6 6 6-6 6"
      />{/if}
  </svg>
{/snippet}

<svelte:head>
  <title>Kalandra — Native CPU and GPU programming</title>
  <meta
    name="description"
    content="Kalandra is a native programming language for CPU and GPU computing. Compose typed functions, work with tensors, and place numerical tasks on Apple Metal."
  />
  <link rel="canonical" href="https://kalandra.dev/" />
  <meta property="og:type" content="website" /><meta
    property="og:site_name"
    content="Kalandra"
  />
  <meta
    property="og:title"
    content="Kalandra — Native CPU and GPU programming"
  />
  <meta
    property="og:description"
    content="A shared language for typed CPU workers, resident GPU graphs, and numerical computing."
  />
  <meta property="og:url" content="https://kalandra.dev/" /><meta
    property="og:image"
    content={socialImage.url}
  />
  <meta property="og:image:width" content={String(socialImage.width)} /><meta
    property="og:image:height"
    content={String(socialImage.height)}
  /><meta property="og:image:alt" content={socialImage.alt} />
  <meta property="og:image:type" content="image/png" />
  <meta name="twitter:image" content={socialImage.url} />
  <meta name="twitter:image:alt" content={socialImage.alt} />
  <meta name="twitter:card" content="summary_large_image" /><meta
    name="theme-color"
    content="#000000"
  />
</svelte:head>

<svelte:window onkeydown={handleNavigationKey} />

<div class="swiss-site">
  <a class="skip" href="#main">Skip to content</a>
  <header class="shell">
    <a class="brand" href="/" aria-label="Kalandra home"
      ><span class="brand-mark" aria-hidden="true"></span>Kalandra</a
    >
    <button
      bind:this={menuButton}
      class="menu"
      aria-expanded={menuOpen}
      aria-controls="main-nav"
      onclick={() => (menuOpen = !menuOpen)}
      >{menuOpen ? "Close" : "Menu"}<svg
        width="20"
        height="20"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.5"
        aria-hidden="true"
        >{#if menuOpen}<path d="m6 6 12 12M6 18 18 6" />{:else}<path
            d="M4 8h16M4 16h16"
          />{/if}</svg
      ></button
    >
    <nav id="main-nav" class:open={menuOpen} aria-label="Main navigation">
      <a href="/docs/syntax">Language</a><a href="/docs/gpu">GPU computing</a><a
        href="/docs">Documentation</a
      ><a href={repo}>GitHub</a>
    </nav>
  </header>
  <main id="main">
    <section class="hero shell" aria-labelledby="hero-title">
      <h1 id="hero-title">
        <span>One language.</span><span>CPU and GPU.</span>
      </h1>
      <div class="hero-intro">
        <p class="hero-description">
          Typed functions. Structured parallelism.<br />Native foundations for
          numerical and AI workloads.
        </p>
        <div class="hero-actions">
          <a class="button" href="/docs/getting-started"
            >Get started {@render arrow()}</a
          ><a class="guide-link" href="/docs/syntax">Read the language guide</a>
        </div>
        <p class="preview-note">Open source. Native development preview.</p>
      </div>
      <ExecutionField />
    </section>

    <section class="examples shell" aria-labelledby="examples-title">
      <div class="example-intro">
        <h2 id="examples-title">Write functions.<br />Place the work.</h2>
        <p aria-live="polite">{example.description}</p>
      </div>
      <div class="code-example">
        <div class="filebar">
          <div class="source-picker">
            <label class="sr-only" for="example"
              >Choose a native code example</label
            >
            <select id="example" bind:value={selected} onchange={resetCopy}>
              {#each data.examples as item, index}<option value={index}
                  >{item.file}</option
                >{/each}
            </select>
            <svg
              width="12"
              height="12"
              viewBox="0 0 16 16"
              fill="none"
              stroke="currentColor"
              stroke-width="1.4"
              aria-hidden="true"><path d="m4 6 4 4 4-4" /></svg
            >
          </div>
          <div class="code-actions">
            <a
              href={`${repo}/blob/native/${example.path}`}
              aria-label={`View complete ${example.title} source`}
              >Source {@render arrow(true)}</a
            ><button
              onclick={copySource}
              aria-label="Copy displayed source code"
              >{copied ? "Copied" : copyError ? "Select code" : "Copy"}</button
            >
          </div>
        </div>
        <!-- Local overflow is keyboard accessible for longer native source choices. -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <pre tabindex="0" aria-label={`${example.title} source code`}><code
            >{#each tokens(example.source) as token}<span
                class:keyword={token.keyword}>{token.text}</span
              >{/each}</code
          ></pre>
        <span class="sr-only" role="status"
          >{copied
            ? "Source copied to clipboard."
            : copyError
              ? "Clipboard unavailable. Select and copy the source manually."
              : ""}</span
        >
      </div>
    </section>

    <section class="computing shell" aria-labelledby="computing-title">
      <p class="section-label">Native computing</p>
      <div class="computing-heading">
        <h2 id="computing-title">From tensors to parallel tasks.</h2>
        <a class="platform-summary" href="#platforms"
          ><span>macOS / CPU + Metal</span><span>Linux, Windows / CPU</span></a
        >
      </div>
      <p class="computing-intro">
        Express the computation as functions. Make execution and data movement
        explicit. A shared language for typed CPU workers and resident GPU
        graphs.
      </p>
      <div class="capabilities">
        <article>
          <h3>Compose functions.</h3>
          <p>
            Build numerical graphs from typed functions, tensor arithmetic,
            matrix products and reductions. Intermediate results can remain on
            the GPU.
          </p>
          <a href="/docs/functions">The language {@render arrow()}</a>
        </article>
        <article>
          <h3>Place the work.</h3>
          <p>
            Dispatch CPU workers or place a numerical task on the GPU. Transfer
            tensors explicitly, await results, and keep task lifetimes within
            parallel scopes.
          </p>
          <a href="/docs/parallelism">Parallel execution {@render arrow()}</a>
        </article>
        <article>
          <h3>Build native programs.</h3>
          <p>
            Compile native applications with a checker, formatter, language
            server and VS Code extension supporting your development workflow.
          </p>
          <a href="/docs/tooling">Native tooling {@render arrow()}</a>
        </article>
      </div>
    </section>

    <section class="numerics shell" aria-labelledby="numerics-title">
      <div class="numerics-copy">
        <h2 id="numerics-title">Numerical by nature.</h2>
        <p>
          Matrix products, tensor arithmetic and reductions are foundations of
          modern AI and LLM systems. Work with them in a typed native language,
          with control over where data lives and computation runs.
        </p>
        <p>
          GPU programs support all ten native numeric types and dense tensor
          ranks zero through eight. Hardware float32 matmul supports optimized
          accumulation, with an explicit fixed-tree policy. GPU float64 requires
          software fallback.
        </p>
        <a class="inline-link" href="/docs/gpu-numerics"
          >Numerical policies {@render arrow()}</a
        >
      </div>
      <div class="operation-reference">
        <div>
          <code>matmul(a, b)</code>
          <p>Compose rank-two matrix products into resident graphs.</p>
        </div>
        <div>
          <code>relu(values)</code>
          <p>Apply a tensor activation without a trip back to the CPU.</p>
        </div>
        <div>
          <code>reduceSum(values)</code>
          <p>Balanced sums over typed numerical tensors.</p>
        </div>
        <div>
          <code>transfer(values, device)</code>
          <p>Explicit data movement between CPU and GPU.</p>
        </div>
        <a class="inline-link" href="/docs/gpu"
          >Explore GPU computing {@render arrow()}</a
        >
      </div>
      <p class="scope-note">
        Apple Metal is the current GPU backend. Model loading, a full LLM
        inference runtime and CUDA remain outside the current scope.
      </p>
    </section>

    <section
      class="platforms shell"
      id="platforms"
      aria-labelledby="platform-title"
    >
      <h2 id="platform-title">Native across platforms.</h2>
      <p class="section-intro">
        The same language and tools for CPU programs. Apple Metal on supported
        Macs.
      </p>
      <!-- The platform table keeps horizontal overflow inside a keyboard-focusable region. -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div
        class="platform-table"
        tabindex="0"
        role="region"
        aria-label="Supported native platforms"
      >
        <table>
          <thead
            ><tr
              ><th scope="col">Platform</th><th scope="col">Execution</th><th
                scope="col">Current baseline</th
              ></tr
            ></thead
          ><tbody
            ><tr
              ><th scope="row">macOS</th><td>CPU + Apple Metal GPU</td><td
                >macOS 26+, Apple silicon</td
              ></tr
            ><tr
              ><th scope="row">Linux</th><td>CPU</td><td
                >x86-64 and ARM64, Ubuntu 24.04 / glibc 2.39</td
              ></tr
            ><tr
              ><th scope="row">Windows</th><td>CPU</td><td
                >x86-64 and ARM64, Windows 11 with C++ build tools</td
              ></tr
            ></tbody
          >
        </table>
      </div>
      <a class="inline-link" href="/docs/installation"
        >Distribution and toolchain requirements {@render arrow()}</a
      >
    </section>

    <section class="onboarding shell" aria-labelledby="start-title">
      <div>
        <h2 id="start-title">Your first native project.</h2>
        <p>
          Install the toolchain, create a project, check its sources, and
          compile an executable. The starter includes two modules and a typed
          parallel map.
        </p>
        <a class="button" href="/docs/getting-started"
          >Get started {@render arrow()}</a
        ><a class="editor-link inline-link" href="/docs/tooling"
          >Set up your editor {@render arrow()}</a
        >
      </div>
      <div class="terminal">
        <p>With the native compiler installed</p>
        <pre><code
            >kalandra new my-app
kalandra-check --project my-app/kalandra.toml
kalandra --project my-app/kalandra.toml \
  -o my-app/program
./my-app/program</code
          ></pre>
      </div>
    </section>

    <section class="open-source shell">
      <h2>Open source,<br />in development.</h2>
      <div>
        <p>
          Explore the implementation, read the language design, or contribute to
          the next native milestone.
        </p>
        <a class="inline-link" href={repo}
          >Kalandra on GitHub {@render arrow(true)}</a
        ><a class="inline-link" href={`${repo}/blob/native/docs/roadmap.md`}
          >Development roadmap {@render arrow()}</a
        ><a class="inline-link" href="/docs/release-notes"
          >0.99.1 release notes {@render arrow()}</a
        >
      </div>
    </section>
  </main>
  <footer class="shell">
    <a class="brand" href="/" aria-label="Kalandra home"
      ><span class="brand-mark" aria-hidden="true"></span>Kalandra</a
    >
    <p>A native language for CPU and GPU computing.</p>
    <nav aria-label="Footer navigation">
      <a href="/docs">Documentation</a><a href={repo}>GitHub</a><a
        href={`${repo}/blob/native/LICENSE`}>Apache 2.0</a
      >
    </nav>
  </footer>
</div>

<style>
  .swiss-site {
    --page: #080808;
    --ink: #f6f6f6;
    --muted: #b5b5b5;
    --rule: #555;
    --signal: #f86737;
    background: var(--page);
    color: var(--ink);
    font-family: "Geist Variable", sans-serif;
    min-height: 100vh;
  }
  .swiss-site ::selection {
    background: var(--signal);
    color: var(--page);
  }
  .shell {
    width: min(1440px, calc(100% - 96px));
    margin-inline: auto;
  }
  a {
    color: inherit;
    text-decoration: none;
  }
  button {
    font: inherit;
  }
  a:focus-visible,
  button:focus-visible {
    outline: 2px solid var(--signal);
    outline-offset: 6px;
  }
  .skip {
    position: fixed;
    top: -100px;
    left: 24px;
    z-index: 50;
    padding: 12px 18px;
    background: var(--ink);
    color: var(--page);
  }
  .skip:focus {
    top: 12px;
  }
  header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    min-height: 64px;
    border-bottom: 1px solid var(--rule);
    gap: 32px;
  }
  .brand {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    font-size: 25px;
    font-weight: 750;
    letter-spacing: -0.04em;
  }
  .brand-mark {
    width: 30px;
    height: 21px;
    background: currentColor;
    mask: url("/brand/kalandra-mark.svg") center/contain no-repeat;
  }
  nav {
    display: flex;
    align-items: center;
    gap: 34px;
    font-size: 16px;
  }
  nav a {
    padding: 16px 0;
  }
  nav a:hover,
  .guide-link:hover {
    color: var(--signal);
  }
  .menu {
    display: none;
  }
  .hero {
    padding-top: 52px;
    padding-bottom: 34px;
    border-bottom: 1px solid var(--rule);
  }
  h1 {
    font-size: clamp(56px, 11.56vw, 184px);
    font-weight: 800;
    line-height: 0.86;
    letter-spacing: -0.04em;
    margin: 0;
  }
  h1 span {
    display: block;
  }
  h1 span:last-child {
    font-size: 1.04em;
  }
  .hero-intro {
    margin: 17px 0 0 50.2%;
  }
  .hero-description {
    color: #d6d6dc;
    font-size: clamp(18px, 1.8vw, 25px);
    line-height: 1.35;
    letter-spacing: -0.02em;
    margin: 0;
  }
  .hero-actions {
    display: flex;
    align-items: center;
    gap: 40px;
    margin-top: 27px;
  }
  .button {
    display: inline-flex;
    align-items: center;
    justify-content: space-between;
    gap: 30px;
    background: var(--signal);
    color: var(--page);
    font-size: 22px;
    line-height: 1.2;
    padding: 16px 32px;
    min-height: 60px;
    white-space: nowrap;
  }
  .button:hover {
    background: #ff8057;
  }
  .guide-link {
    font-size: 18px;
    line-height: 1.4;
    text-decoration: underline;
    text-underline-offset: 8px;
    text-decoration-thickness: 1px;
    white-space: nowrap;
  }
  .preview-note {
    color: var(--muted);
    font-size: 16px;
    line-height: 1.5;
    margin: 18px 0 0;
  }
  @media (min-width: 1600px) {
    h1 {
      font-size: 184px;
    }
  }
  @media (max-width: 1050px) {
    nav {
      gap: 23px;
      font-size: 14px;
    }
    .hero-actions {
      gap: 24px;
      flex-wrap: wrap;
    }
    .guide-link {
      font-size: 16px;
    }
    .hero-description {
      font-size: 18px;
    }
    .button {
      font-size: 19px;
      min-height: 54px;
      padding: 14px 24px;
    }
    .hero-intro {
      margin-left: 45%;
    }
  }
  @media (max-width: 760px) {
    .shell {
      width: calc(100% - 40px);
    }
    header {
      min-height: 64px;
      position: relative;
      flex-wrap: wrap;
      gap: 0;
    }
    .brand {
      font-size: 23px;
    }
    .brand-mark {
      width: 28px;
      height: 20px;
    }
    .menu {
      display: flex;
      align-items: center;
      gap: 12px;
      border: 0;
      background: none;
      color: var(--ink);
      padding: 15px 0;
      font-size: 14px;
    }
    nav {
      display: none;
      width: 100%;
      flex-direction: column;
      align-items: stretch;
      gap: 0;
      padding: 8px 0 16px;
    }
    nav.open {
      display: flex;
    }
    nav a {
      padding: 13px 0;
      border-top: 1px solid #2d2d2d;
      font-size: 16px;
    }
    .hero {
      padding-top: 44px;
      padding-bottom: 26px;
    }
    h1 {
      font-size: 12.35vw;
      line-height: 0.94;
    }
    .hero-intro {
      margin: 30px 0 0;
    }
    .hero-description {
      font-size: 18px;
      max-width: 560px;
    }
    .hero-actions {
      gap: 24px;
      margin-top: 24px;
    }
    .button {
      font-size: 17px;
      min-height: 52px;
      gap: 26px;
      padding: 14px 22px;
    }
    .guide-link {
      font-size: 15px;
    }
    .preview-note {
      font-size: 13px;
      margin-top: 20px;
    }
  }
  @media (max-width: 359px) {
    .shell {
      width: calc(100% - 32px);
    }
    .hero-actions {
      align-items: flex-start;
      gap: 24px;
      flex-direction: column;
    }
    .hero-description {
      font-size: 16px;
    }
  }

  h2 {
    font-weight: 750;
    letter-spacing: -0.04em;
    line-height: 1.02;
  }
  p {
    text-wrap: pretty;
  }
  .examples {
    display: grid;
    grid-template-columns: 1.045fr 1fr;
    gap: 60px;
    padding-block: 40px 36px;
    border-bottom: 1px solid var(--rule);
  }
  .example-intro h2 {
    font-size: clamp(38px, 6vw, 88px);
    line-height: 0.96;
    margin: 0;
  }
  .example-intro > p {
    color: #d6d6dc;
    font-size: 20px;
    line-height: 1.4;
    letter-spacing: -0.015em;
    margin: 18px 0 0;
    max-width: 550px;
  }
  .code-example {
    min-width: 0;
    align-self: start;
    border: 1px solid var(--rule);
  }
  .filebar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 12px;
    min-height: 36px;
    padding: 6px 18px;
    border-bottom: 1px solid var(--rule);
    font:
      12px/1.4 "Fira Code Variable",
      monospace;
  }
  .source-picker {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
  }
  .source-picker svg {
    pointer-events: none;
    flex-shrink: 0;
  }
  select {
    appearance: none;
    min-width: 0;
    max-width: 220px;
    min-height: 24px;
    padding: 0 2px;
    border: 0;
    border-radius: 0;
    color: var(--ink);
    background: var(--page);
    font: inherit;
    cursor: pointer;
  }
  select:focus-visible,
  pre:focus-visible,
  .platform-table:focus-visible {
    outline: 2px solid var(--signal);
    outline-offset: 5px;
  }
  .code-actions {
    display: flex;
    align-items: center;
    gap: 18px;
  }
  .code-actions a {
    min-height: 24px;
    display: flex;
    align-items: center;
    gap: 4px;
  }
  .code-actions svg {
    width: 12px;
    height: 12px;
  }
  .code-actions button {
    background: none;
    border: 0;
    padding: 0;
    color: inherit;
    cursor: pointer;
    min-width: 34px;
    min-height: 24px;
    text-align: right;
  }
  .code-actions a:hover,
  .code-actions button:hover {
    color: var(--signal);
  }
  .code-example pre {
    margin: 0;
    padding: 20px 24px 24px;
    font-size: 15px;
    line-height: 1.6;
    min-height: 164px;
    max-height: 330px;
    overflow: auto;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    tab-size: 2;
    scrollbar-color: #777 var(--page);
    scrollbar-width: thin;
  }
  .code-example code {
    font-family: "Fira Code Variable", monospace;
  }
  .keyword {
    color: var(--signal);
  }
  .computing {
    padding-block: 26px 80px;
    border-bottom: 1px solid var(--rule);
  }
  .section-label {
    font-size: 11px;
    line-height: 1.3;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    margin: 0 0 18px;
  }
  .computing-heading {
    display: grid;
    grid-template-columns: 1fr 225px;
    align-items: center;
    gap: 35px;
  }
  .computing-heading h2 {
    font-size: clamp(32px, 5vw, 72px);
    margin: 0;
  }
  .platform-summary {
    font-size: 13px;
    line-height: 1.8;
    letter-spacing: 0.01em;
    padding: 8px 0 8px 32px;
    border-left: 1px solid #a5a5a5;
  }
  .platform-summary span {
    display: block;
  }
  .platform-summary:hover {
    color: var(--signal);
  }
  .computing-intro {
    margin: 50px 0 48px;
    max-width: 820px;
    font-size: 26px;
    line-height: 1.45;
    letter-spacing: -0.02em;
  }
  .capabilities article {
    display: grid;
    grid-template-columns: 0.8fr 1.4fr 0.65fr;
    align-items: baseline;
    gap: 50px;
    padding: 28px 0;
    border-top: 1px solid #333;
  }
  .capabilities h3 {
    font-size: 23px;
    line-height: 1.25;
    font-weight: 600;
    letter-spacing: -0.025em;
    margin: 0;
  }
  .capabilities p {
    color: var(--muted);
    font-size: 16px;
    line-height: 1.6;
    margin: 0;
  }
  .capabilities a {
    justify-self: end;
    display: flex;
    align-items: center;
    gap: 12px;
    font-size: 14px;
    white-space: nowrap;
  }
  .capabilities a:hover {
    color: var(--signal);
  }
  .capabilities svg,
  .inline-link svg {
    width: 18px;
    height: 18px;
    flex-shrink: 0;
  }
  .numerics {
    display: grid;
    grid-template-columns: 1.15fr 1fr;
    gap: 40px 110px;
    padding-block: 80px;
    border-bottom: 1px solid var(--rule);
  }
  .numerics h2,
  .platforms h2,
  .onboarding h2 {
    font-size: clamp(38px, 4.7vw, 66px);
    margin: 0 0 28px;
  }
  .numerics-copy > p,
  .section-intro,
  .onboarding p {
    color: var(--muted);
    font-size: 17px;
    line-height: 1.7;
    max-width: 70ch;
    margin: 0 0 20px;
  }
  .inline-link {
    display: inline-flex;
    align-items: center;
    gap: 14px;
    text-decoration: underline;
    text-underline-offset: 6px;
    font-size: 15px;
    line-height: 1.5;
  }
  .inline-link:hover {
    color: var(--signal);
  }
  .numerics-copy .inline-link {
    margin-top: 10px;
  }
  .operation-reference > div {
    border-top: 1px solid #444;
    padding: 18px 0;
  }
  .operation-reference code {
    font-size: 19px;
    color: var(--signal);
  }
  .operation-reference p {
    color: var(--muted);
    font-size: 15px;
    line-height: 1.6;
    margin: 8px 0 0;
  }
  .operation-reference > .inline-link {
    margin-top: 16px;
  }
  .scope-note {
    grid-column: 1/-1;
    font-size: 13px;
    line-height: 1.7;
    color: var(--muted);
    margin: 0;
    max-width: 100ch;
  }
  .platforms {
    padding-block: 80px;
    border-bottom: 1px solid var(--rule);
    scroll-margin-top: 24px;
  }
  .platform-table {
    margin: 36px 0 24px;
    overflow-x: auto;
    scrollbar-width: thin;
    scrollbar-color: #777 var(--page);
  }
  table {
    border-collapse: collapse;
    width: 100%;
    min-width: 560px;
    text-align: left;
    font-size: 16px;
  }
  th,
  td {
    border-top: 1px solid #444;
    padding: 21px 24px 21px 0;
    line-height: 1.6;
  }
  thead th {
    font-size: 12px;
    font-weight: 400;
    letter-spacing: 0.04em;
    color: var(--muted);
    padding-block: 13px;
  }
  tbody th {
    font-weight: 550;
  }
  tbody td {
    color: var(--muted);
  }
  .onboarding {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 90px;
    padding-block: 80px;
    border-bottom: 1px solid var(--rule);
  }
  .onboarding .button {
    margin-top: 16px;
    font-size: 18px;
  }
  .editor-link {
    display: flex;
    width: fit-content;
    margin-top: 24px;
  }
  .terminal {
    align-self: start;
    min-width: 0;
    border: 1px solid var(--rule);
  }
  .terminal > p {
    margin: 0;
    padding: 12px 20px;
    border-bottom: 1px solid var(--rule);
    font-size: 12px;
    color: var(--muted);
  }
  .terminal pre {
    margin: 0;
    padding: 24px;
    font-size: 13px;
    line-height: 2.1;
    overflow-x: auto;
    scrollbar-color: #777 var(--page);
    scrollbar-width: thin;
  }
  .open-source {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 90px;
    padding-block: 80px;
  }
  .open-source h2 {
    font-size: clamp(44px, 5.5vw, 80px);
    margin: 0;
  }
  .open-source p {
    max-width: 460px;
    color: var(--muted);
    font-size: 18px;
    line-height: 1.7;
    margin: 0 0 28px;
  }
  .open-source .inline-link {
    display: flex;
    width: fit-content;
    margin-top: 20px;
  }
  footer {
    border-top: 1px solid var(--rule);
    padding-block: 32px 40px;
    display: flex;
    align-items: center;
    gap: 32px;
  }
  footer p {
    font-size: 12px;
    color: var(--muted);
    margin: 0;
  }
  footer nav {
    margin-left: auto;
    gap: 26px;
    font-size: 13px;
  }
  @media (max-width: 1050px) {
    .examples {
      gap: 36px;
    }
    .filebar {
      padding-inline: 12px;
      gap: 8px;
    }
    .code-actions {
      gap: 12px;
    }
    .code-example pre {
      padding: 20px;
      font-size: 13px;
    }
    .example-intro > p {
      font-size: 18px;
    }
    .computing-heading {
      grid-template-columns: 1fr 190px;
      gap: 24px;
    }
    .platform-summary {
      padding-left: 24px;
      font-size: 12px;
    }
    .capabilities article {
      gap: 30px;
    }
    .capabilities h3 {
      font-size: 21px;
    }
    .capabilities p {
      font-size: 15px;
    }
    .capabilities a {
      font-size: 12px;
      gap: 7px;
    }
    .numerics,
    .onboarding,
    .open-source {
      gap: 60px;
    }
    .operation-reference code {
      font-size: 17px;
    }
    footer {
      flex-wrap: wrap;
      gap: 20px;
    }
  }
  @media (max-width: 760px) {
    .examples {
      grid-template-columns: 1fr;
      gap: 28px;
      padding-block: 32px;
    }
    .example-intro h2 {
      font-size: clamp(38px, 7vw, 56px);
    }
    .example-intro > p {
      font-size: 17px;
      max-width: 520px;
      margin-top: 20px;
    }
    .filebar {
      padding: 4px 14px;
    }
    .filebar select,
    .code-actions a,
    .code-actions button {
      min-height: 44px;
    }
    .code-example pre {
      font-size: 12px;
      line-height: 1.8;
      padding: 18px;
      min-height: 140px;
      max-height: 300px;
    }
    .computing {
      padding-block: 25px 48px;
    }
    .computing-heading {
      grid-template-columns: 1fr;
      gap: 24px;
    }
    .computing-heading h2 {
      font-size: clamp(35px, 7.5vw, 56px);
      max-width: 550px;
    }
    .platform-summary {
      padding: 0 0 0 16px;
      font-size: 12px;
    }
    .computing-intro {
      margin: 32px 0;
      font-size: 21px;
    }
    .capabilities article {
      grid-template-columns: 1fr;
      gap: 12px;
      padding-block: 24px;
    }
    .capabilities h3 {
      font-size: 23px;
    }
    .capabilities p {
      font-size: 16px;
    }
    .capabilities a {
      justify-self: start;
      font-size: 14px;
      margin-top: 5px;
    }
    .numerics,
    .onboarding,
    .open-source {
      grid-template-columns: 1fr;
      gap: 32px;
      padding-block: 48px;
    }
    .numerics h2,
    .platforms h2,
    .onboarding h2 {
      font-size: 38px;
      margin-bottom: 24px;
    }
    .numerics-copy > p,
    .section-intro,
    .onboarding p {
      font-size: 16px;
    }
    .scope-note {
      font-size: 12px;
    }
    .platforms {
      padding-block: 48px;
    }
    .platform-table {
      margin-top: 26px;
    }
    .platform-table th,
    .platform-table td {
      font-size: 14px;
    }
    .onboarding .button {
      font-size: 17px;
    }
    .terminal pre {
      font-size: 11px;
      padding: 18px;
    }
    .open-source h2 {
      font-size: clamp(42px, 10vw, 64px);
    }
    .open-source p {
      font-size: 17px;
    }
    footer {
      padding-block: 28px;
      gap: 18px;
    }
    footer p {
      width: 100%;
      font-size: 12px;
    }
    footer nav {
      display: flex;
      flex-direction: row;
      flex-wrap: wrap;
      width: 100%;
      margin: 0;
      padding: 0;
      gap: 24px;
    }
    footer nav a {
      border: 0;
      padding: 8px 0;
      font-size: 14px;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    *,
    *::before,
    *::after {
      transition: none !important;
      animation: none !important;
    }
  }
</style>
