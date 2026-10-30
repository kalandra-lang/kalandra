<script lang="ts">
  let { compact = false }: { compact?: boolean } = $props();
</script>

<span class="brand" class:compact>
  <span class="mark" aria-hidden="true"></span>
  <span class="name">Kalandra</span>
</span>

<style>
  .brand {
    display: inline-flex;
    align-items: center;
    gap: 11px;
    color: var(--foreground);
  }
  .mark {
    display: block;
    flex: none;
    width: 40px;
    height: 28px;
    background: currentColor;
    mask: url("/brand/kalandra-mark.svg") center / contain no-repeat;
  }
  .name {
    font-size: 26px;
    font-weight: 550;
    letter-spacing: -1.2px;
  }
  .compact .mark {
    width: 32px;
    height: 23px;
  }
  .compact .name {
    font-size: 21px;
  }
  @media (max-width: 520px) {
    .mark {
      width: 34px;
      height: 24px;
    }
    .name {
      font-size: 23px;
    }
    .brand {
      gap: 8px;
    }
  }
</style>
