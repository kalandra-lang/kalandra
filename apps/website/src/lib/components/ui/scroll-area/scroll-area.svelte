<script lang="ts">
  import { cn } from "#lib/utils.ts";
  import type { Snippet } from "svelte";

  let {
    class: className,
    children,
    ...restProps
  }: {
    class?: string;
    children?: Snippet;
  } = $props();
</script>

<div class={cn("relative overflow-hidden", className)} {...restProps}>
  <div
    class="h-full w-full overflow-y-auto overscroll-contain [&::-webkit-scrollbar]:w-1.5 [&::-webkit-scrollbar-thumb]:rounded-full [&::-webkit-scrollbar-thumb]:bg-border [&::-webkit-scrollbar-track]:bg-transparent"
  >
    {#if children}{@render children()}{/if}
  </div>
</div>
