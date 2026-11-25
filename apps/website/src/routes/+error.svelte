<script lang="ts">
  import { page } from "$app/state";
</script>

<div
  class="flex min-h-screen flex-col items-center justify-center bg-background px-6 text-foreground"
>
  <p class="text-8xl font-bold tracking-tighter text-muted-foreground/50">
    {page.status}
  </p>
  <h1 class="mt-1 text-xl font-semibold">
    {page.error?.message ?? "Something went wrong"}
  </h1>
  <a
    href="/docs"
    class="mt-12 inline-flex h-10 items-center justify-center rounded-lg border px-6 text-sm font-medium transition-colors hover:bg-accent hover:text-accent-foreground"
  >
    Back to docs
  </a>
</div>
