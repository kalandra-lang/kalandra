<script lang="ts">
  import { DropdownMenu } from "bits-ui";
  import type { OpenLink } from "@tiramisu-docs/kit";

  let {
    links = [],
    markdown = undefined,
  }: { links?: OpenLink[]; markdown?: string } = $props();

  const openLinks = $derived(links.filter((l: OpenLink) => l.type === "open"));
  const editLinks = $derived(links.filter((l: OpenLink) => l.type === "edit"));
  const mcpLinks = $derived(links.filter((l: OpenLink) => l.type === "mcp"));

  let copied = $state(false);
  let mdCopied = $state(false);

  async function copyToClipboard(text: string) {
    await navigator.clipboard.writeText(text);
    copied = true;
    setTimeout(() => (copied = false), 2000);
  }

  async function copyMarkdown() {
    if (!markdown) return;
    await navigator.clipboard.writeText(markdown);
    mdCopied = true;
    setTimeout(() => (mdCopied = false), 2000);
  }
</script>

{#snippet icon(name: string)}
  {#if name === "chatgpt"}
    <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="currentColor"
      ><path
        d="M22.2819 9.8211a5.9847 5.9847 0 0 0-.5157-4.9108 6.0462 6.0462 0 0 0-6.5098-2.9A6.0651 6.0651 0 0 0 4.9807 4.1818a5.9847 5.9847 0 0 0-3.9977 2.9 6.0462 6.0462 0 0 0 .7427 7.0966 5.98 5.98 0 0 0 .511 4.9107 6.051 6.051 0 0 0 6.5146 2.9001A5.9847 5.9847 0 0 0 13.2599 24a6.0557 6.0557 0 0 0 5.7718-4.2058 5.9894 5.9894 0 0 0 3.9977-2.9001 6.0557 6.0557 0 0 0-.7475-7.0729zm-9.022 12.6081a4.4755 4.4755 0 0 1-2.8764-1.0408l.1419-.0804 4.7783-2.7582a.7948.7948 0 0 0 .3927-.6813v-6.7369l2.02 1.1686a.071.071 0 0 1 .038.052v5.5826a4.504 4.504 0 0 1-4.4945 4.4944zm-9.6607-4.1254a4.4708 4.4708 0 0 1-.5346-3.0137l.142.0852 4.783 2.7582a.7712.7712 0 0 0 .7806 0l5.8428-3.3685v2.3324a.0804.0804 0 0 1-.0332.0615L9.74 19.9502a4.4992 4.4992 0 0 1-6.1408-1.6464zM2.3408 7.8956a4.485 4.485 0 0 1 2.3655-1.9728V11.6a.7664.7664 0 0 0 .3879.6765l5.8144 3.3543-2.0201 1.1685a.0757.0757 0 0 1-.071 0l-4.8303-2.7865A4.504 4.504 0 0 1 2.3408 7.872zm16.5963 3.8558L13.1038 8.364 15.1192 7.2a.0757.0757 0 0 1 .071 0l4.8303 2.7913a4.4944 4.4944 0 0 1-.6765 8.1042v-5.6772a.79.79 0 0 0-.407-.667zm2.0107-3.0231l-.142-.0852-4.7735-2.7818a.7759.7759 0 0 0-.7854 0L9.409 9.2297V6.8974a.0662.0662 0 0 1 .0284-.0615l4.8303-2.7866a4.4992 4.4992 0 0 1 6.6802 4.66zM8.3065 12.863l-2.02-1.1638a.0804.0804 0 0 1-.038-.0567V6.0742a4.4992 4.4992 0 0 1 7.3757-3.4537l-.142.0805L8.704 5.459a.7948.7948 0 0 0-.3927.6813zm1.0976-2.3654l2.602-1.4998 2.6069 1.4998v2.9994l-2.5974 1.4997-2.6067-1.4997Z"
      /></svg
    >
  {:else if name === "claude"}
    <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="currentColor"
      ><path
        d="m4.7144 15.9555 4.7174-2.6471.079-.2307-.079-.1275h-.2307l-.7893-.0486-2.6956-.0729-2.3375-.0971-2.2646-.1214-.5707-.1215-.5343-.7042.0546-.3522.4797-.3218.686.0608 1.5179.1032 2.2767.1578 1.6514.0972 2.4468.255h.3886l.0546-.1579-.1336-.0971-.1032-.0972L6.973 9.8356l-2.55-1.6879-1.3356-.9714-.7225-.4918-.3643-.4614-.1578-1.0078.6557-.7225.8803.0607.2246.0607.8925.686 1.9064 1.4754 2.4893 1.8336.3643.3035.1457-.1032.0182-.0728-.164-.2733-1.3539-2.4467-1.445-2.4893-.6435-1.032-.17-.6194c-.0607-.255-.1032-.4674-.1032-.7285L6.287.1335 6.6997 0l.9957.1336.419.3642.6192 1.4147 1.0018 2.2282 1.5543 3.0296.4553.8985.2429.8318.091.255h.1579v-.1457l.1275-1.706.2368-2.0947.2307-2.6957.0789-.7589.3764-.9107.7468-.4918.5828.2793.4797.686-.0668.4433-.2853 1.8517-.5586 2.9021-.3643 1.9429h.2125l.2429-.2429.9835-1.3053 1.6514-2.0643.7286-.8196.85-.9046.5464-.4311h1.0321l.759 1.1293-.34 1.1657-1.0625 1.3478-.8804 1.1414-1.2628 1.7-.7893 1.36.0729.1093.1882-.0183 2.8535-.607 1.5421-.2794 1.8396-.3157.8318.3886.091.3946-.3278.8075-1.967.4857-2.3072.4614-3.4364.8136-.0425.0304.0486.0607 1.5482.1457.6618.0364h1.621l3.0175.2247.7892.522.4736.6376-.079.4857-1.2142.6193-1.6393-.3886-3.825-.9107-1.3113-.3279h-.1822v.1093l1.0929 1.0686 2.0035 1.8092 2.5075 2.3314.1275.5768-.3218.4554-.34-.0486-2.2039-1.6575-.85-.7468-1.9246-1.621h-.1275v.17l.4432.6496 2.3436 3.5214.1214 1.0807-.17.3521-.6071.2125-.6679-.1214-1.3721-1.9246L14.38 17.959l-1.1414-1.9428-.1397.079-.674 7.2552-.3156.3703-.7286.2793-.6071-.4614-.3218-.7468.3218-1.4753.3886-1.9246.3157-1.53.2853-1.9004.17-.6314-.0121-.0425-.1397.0182-1.4328 1.9672-2.1796 2.9446-1.7243 1.8456-.4128.164-.7164-.3704.0667-.6618.4008-.5889 2.386-3.0357 1.4389-1.882.929-1.0868-.0062-.1579h-.0546l-6.3385 4.1164-1.1293.1457-.4857-.4554.0608-.7467.2307-.2429 1.9064-1.3114Z"
      /></svg
    >
  {:else if name === "cursor"}
    <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="currentColor"
      ><path
        d="M11.503.131 1.891 5.678a.84.84 0 0 0-.42.726v11.188c0 .3.162.575.42.724l9.609 5.55a1 1 0 0 0 .998 0l9.61-5.55a.84.84 0 0 0 .42-.724V6.404a.84.84 0 0 0-.42-.726L12.497.131a1.01 1.01 0 0 0-.996 0M2.657 6.338h18.55c.263 0 .43.287.297.515L12.23 22.918c-.062.107-.229.064-.229-.06V12.335a.59.59 0 0 0-.295-.51l-9.11-5.257c-.109-.063-.064-.23.061-.23"
      /></svg
    >
  {:else if name === "github"}
    <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="currentColor"
      ><path
        d="M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61C4.422 18.07 3.633 17.7 3.633 17.7c-1.087-.744.084-.729.084-.729 1.205.084 1.838 1.236 1.838 1.236 1.07 1.835 2.809 1.305 3.495.998.108-.776.417-1.305.76-1.605-2.665-.3-5.466-1.332-5.466-5.93 0-1.31.465-2.38 1.235-3.22-.135-.303-.54-1.523.105-3.176 0 0 1.005-.322 3.3 1.23.96-.267 1.98-.399 3-.405 1.02.006 2.04.138 3 .405 2.28-1.552 3.285-1.23 3.285-1.23.645 1.653.24 2.873.12 3.176.765.84 1.23 1.91 1.23 3.22 0 4.61-2.805 5.625-5.475 5.92.42.36.81 1.096.81 2.22 0 1.606-.015 2.896-.015 3.286 0 .315.21.69.825.57C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12"
      /></svg
    >
  {:else if name === "mcp"}
    <svg
      class="size-4 shrink-0"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="2"
      stroke-linecap="round"
      stroke-linejoin="round"
      ><path
        d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"
      /><path
        d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"
      /></svg
    >
  {:else if name === "vscode"}
    <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="currentColor"
      ><path
        d="M23.15 2.587L18.21.21a1.494 1.494 0 0 0-1.705.29l-9.46 8.63-4.12-3.128a.999.999 0 0 0-1.276.057L.327 7.261A1 1 0 0 0 .326 8.74L3.899 12 .326 15.26a1 1 0 0 0 .001 1.479L1.65 17.94a.999.999 0 0 0 1.276.057l4.12-3.128 9.46 8.63a1.492 1.492 0 0 0 1.704.29l4.942-2.377A1.5 1.5 0 0 0 24 20.06V3.939a1.5 1.5 0 0 0-.85-1.352zm-5.146 14.861L10.826 12l7.178-5.448v10.896z"
      /></svg
    >
  {/if}
{/snippet}

{#if links.length > 0 || markdown}
  <DropdownMenu.Root>
    <DropdownMenu.Trigger class="site-control shrink-0">
      Open
      <svg
        width="12"
        height="12"
        viewBox="0 0 12 12"
        fill="none"
        class="opacity-60"
      >
        <path
          d="M3 4.5L6 7.5L9 4.5"
          stroke="currentColor"
          stroke-width="1.5"
          stroke-linecap="round"
          stroke-linejoin="round"
        />
      </svg>
    </DropdownMenu.Trigger>
    <DropdownMenu.Content
      align="end"
      class="z-50 min-w-48 border bg-background p-2"
    >
      {#each openLinks as link}
        <DropdownMenu.Item
          class="cursor-pointer px-3 py-2 text-sm outline-none data-highlighted:bg-muted"
        >
          <a
            href={link.href}
            target="_blank"
            rel="noopener noreferrer"
            class="flex w-full items-center gap-2"
          >
            {@render icon(link.icon)}
            {link.label}
          </a>
        </DropdownMenu.Item>
      {/each}
      {#if editLinks.length > 0}
        <DropdownMenu.Separator />
        {#each editLinks as link}
          <DropdownMenu.Item
            class="cursor-pointer px-3 py-2 text-sm outline-none data-highlighted:bg-muted"
          >
            <a
              href={link.href}
              target="_blank"
              rel="noopener noreferrer"
              class="flex w-full items-center gap-2"
            >
              {@render icon(link.icon)}
              {link.label}
            </a>
          </DropdownMenu.Item>
        {/each}
      {/if}
      {#if mcpLinks.length > 0}
        <DropdownMenu.Separator />
        {#each mcpLinks as link}
          <DropdownMenu.Item
            class="cursor-pointer px-3 py-2 text-sm outline-none data-highlighted:bg-muted"
          >
            {#if link.copy}
              <button
                onclick={(e: MouseEvent) => {
                  e.preventDefault();
                  e.stopPropagation();
                  copyToClipboard(link.copy!);
                }}
                class="flex w-full items-center gap-2"
              >
                {#if copied}
                  <svg
                    class="size-4 shrink-0 text-primary"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                    stroke-linecap="round"
                    stroke-linejoin="round"><path d="M20 6 9 17l-5-5" /></svg
                  >
                  Copied!
                {:else}
                  {@render icon(link.icon)}
                  {link.label}
                {/if}
              </button>
            {:else}
              <a href={link.href} class="flex w-full items-center gap-2">
                {@render icon(link.icon)}
                {link.label}
                <svg
                  class="size-3 opacity-60"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  ><path d="M7 7h10v10" /><path d="M7 17 17 7" /></svg
                >
              </a>
            {/if}
          </DropdownMenu.Item>
        {/each}
      {/if}
      {#if markdown}
        <DropdownMenu.Separator />
        <DropdownMenu.Item
          class="cursor-pointer px-3 py-2 text-sm outline-none data-highlighted:bg-muted"
        >
          <button
            onclick={(e: MouseEvent) => {
              e.preventDefault();
              e.stopPropagation();
              copyMarkdown();
            }}
            class="flex w-full items-center gap-2"
          >
            {#if mdCopied}
              <svg
                class="size-4 shrink-0 text-primary"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"><path d="M20 6 9 17l-5-5" /></svg
              >
              Copied!
            {:else}
              <svg
                class="size-4 shrink-0"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
                ><rect width="8" height="4" x="8" y="2" rx="1" ry="1" /><path
                  d="M16 4h2a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h2"
                /></svg
              >
              Copy as Markdown
            {/if}
          </button>
        </DropdownMenu.Item>
      {/if}
    </DropdownMenu.Content>
  </DropdownMenu.Root>
{/if}
