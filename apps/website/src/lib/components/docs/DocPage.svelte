<script lang="ts">
  import type { ComponentProps } from "svelte";
  import { DocPage } from "@tiramisu-docs/kit/components";
  let props: ComponentProps<typeof DocPage> = $props();
</script>

<div class="swiss-doc-page"><DocPage {...props} /></div>
