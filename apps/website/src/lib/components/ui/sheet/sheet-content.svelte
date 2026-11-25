<script lang="ts">
  import { cn } from "#lib/utils.ts";
  import { fly, fade } from "svelte/transition";
  import type { Snippet } from "svelte";

  let {
    open = false,
    onclose,
    side = "left",
    class: className,
    children,
  }: {
    open?: boolean;
    onclose?: () => void;
    side?: "left" | "right" | "top" | "bottom";
    class?: string;
    children?: Snippet;
  } = $props();

  function handleKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") onclose?.();
  }

  const sideStyles: Record<string, string> = {
    left: "inset-y-0 left-0 h-full w-3/4 max-w-sm border-r",
    right: "inset-y-0 right-0 h-full w-3/4 max-w-sm border-l",
    top: "inset-x-0 top-0 w-full border-b",
    bottom: "inset-x-0 bottom-0 w-full border-t",
  };

  const flyParams: Record<string, { x?: number; y?: number }> = {
    left: { x: -300 },
    right: { x: 300 },
    top: { y: -300 },
    bottom: { y: 300 },
  };
</script>

{#if open}
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="fixed inset-0 z-50" onkeydown={handleKeydown}>
    <button
      class="fixed inset-0 bg-background/80 backdrop-blur-sm"
      onclick={onclose}
      aria-label="Close"
      transition:fade={{ duration: 200 }}
    ></button>
    <div
      class={cn(
        "fixed z-50 gap-4 bg-background p-6 shadow-lg",
        sideStyles[side],
        className,
      )}
      transition:fly={{ ...flyParams[side], duration: 250, opacity: 1 }}
    >
      <button
        onclick={onclose}
        class="absolute right-4 top-4 rounded-sm opacity-70 ring-offset-background transition-opacity hover:opacity-100 focus:outline-none focus:ring-2 focus:ring-ring focus:ring-offset-2"
        aria-label="Close"
      >
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
          class="h-4 w-4"
        >
          <path d="M18 6 6 18"></path>
          <path d="m6 6 12 12"></path>
        </svg>
      </button>
      {#if children}{@render children()}{/if}
    </div>
  </div>
{/if}
