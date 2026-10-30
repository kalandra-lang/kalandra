<script lang="ts">
  import { onDestroy, type Snippet } from "svelte";
  let {
    language = "",
    icon = "",
    code = "",
    children,
  }: {
    language?: string;
    icon?: string;
    code?: string;
    children?: Snippet;
  } = $props();
  let copied = $state(false);
  let error = $state(false);
  let timer: ReturnType<typeof setTimeout>;
  let codeElement: HTMLElement;
  async function copyCode() {
    clearTimeout(timer);
    copied = false;
    error = false;
    try {
      await navigator.clipboard.writeText(codeElement.textContent ?? "");
      copied = true;
    } catch {
      error = true;
    }
    timer = setTimeout(() => {
      copied = false;
      error = false;
    }, 2500);
  }
  onDestroy(() => clearTimeout(timer));
</script>

<div class="docs-code">
  <div class="docs-code-bar">
    <span>{language || "Source"}</span><button
      onclick={copyCode}
      aria-label="Copy code"
      >{copied ? "Copied" : error ? "Try again" : "Copy"}<svg
        width="14"
        height="14"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.5"
        aria-hidden="true"
        >{#if copied}<path d="m5 12 4 4L19 6" />{:else}<path
            d="M8 8h12v12H8zM16 8V4H4v12h4"
          />{/if}</svg
      ></button
    >
  </div>
  <!-- Scrollable source needs a keyboard stop for horizontal navigation. -->
  <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
  <pre
    role="region"
    tabindex="0"
    aria-label={`${language || "Source"} code`}><code bind:this={codeElement}
      >{@html code}</code
    ></pre>
  <span class="sr-only" role="status"
    >{copied
      ? "Code copied to clipboard."
      : error
        ? "Could not copy. Select the code to copy it manually, or try again."
        : ""}</span
  >
</div>
