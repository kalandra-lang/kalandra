<script lang="ts">
  import { cn } from "#lib/utils.ts";
  import { slide } from "svelte/transition";
  import type { Snippet } from "svelte";

  let {
    open = $bindable(true),
    href,
    class: className,
    trigger,
    children,
  }: {
    open?: boolean;
    href?: string;
    class?: string;
    trigger?: Snippet;
    children?: Snippet;
  } = $props();
</script>

<div class={cn(className)}>
  {#if href}
    <a
      {href}
      onclick={() => (open = !open)}
      class="flex w-full items-center justify-between"
      aria-expanded={open}
    >
      {#if trigger}{@render trigger()}{/if}
      <svg
        xmlns="http://www.w3.org/2000/svg"
        width="24"
        height="24"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="2"
        stroke-linecap="round"
        stroke-linejoin="round"
        class="h-4 w-4 shrink-0 text-muted-foreground transition-transform duration-200 {open
          ? 'rotate-90'
          : ''}"
      >
        <path d="m9 18 6-6-6-6"></path>
      </svg>
    </a>
  {:else}
    <button
      onclick={() => (open = !open)}
      class="flex w-full items-center justify-between"
      aria-expanded={open}
    >
      {#if trigger}{@render trigger()}{/if}
      <svg
        xmlns="http://www.w3.org/2000/svg"
        width="24"
        height="24"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="2"
        stroke-linecap="round"
        stroke-linejoin="round"
        class="h-4 w-4 shrink-0 text-muted-foreground transition-transform duration-200 {open
          ? 'rotate-90'
          : ''}"
      >
        <path d="m9 18 6-6-6-6"></path>
      </svg>
    </button>
  {/if}
  {#if open}
    <div transition:slide={{ duration: 200 }}>
      {#if children}{@render children()}{/if}
    </div>
  {/if}
</div>
