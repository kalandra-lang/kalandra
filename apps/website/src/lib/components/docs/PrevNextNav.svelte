<script lang="ts">
  import type { ComponentProps } from "svelte";
  import { PrevNextNav } from "@tiramisu-docs/kit/components";
  let props: ComponentProps<typeof PrevNextNav> = $props();
</script>

<div class="docs-pagination"><PrevNextNav {...props} /></div>
