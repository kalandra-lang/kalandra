<script lang="ts">
  import type { Snippet } from "svelte";
  let { type = "info", children }: { type?: string; children: Snippet } =
    $props();
  const labels: Record<string, string> = {
    info: "Note",
    warning: "Warning",
    error: "Error",
    success: "Success",
  };
</script>

<aside class="docs-callout" aria-label={labels[type] ?? "Note"}>
  <strong>{labels[type] ?? "Note"}</strong>
  <div>{@render children()}</div>
</aside>
