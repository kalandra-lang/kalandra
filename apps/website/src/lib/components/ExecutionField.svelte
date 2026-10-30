<script lang="ts">
  import { onMount } from "svelte";
  let active = $state(false);
  let activeRow = $state(3);
  let activeColumn = $state(8);
  let reducedMotion = $state(false);
  onMount(() => {
    const preference = window.matchMedia("(prefers-reduced-motion: reduce)");
    const update = () => {
      reducedMotion = preference.matches;
      if (reducedMotion) active = false;
    };
    update();
    preference.addEventListener("change", update);
    return () => preference.removeEventListener("change", update);
  });
  function explore(event: PointerEvent) {
    if (reducedMotion || event.pointerType === "touch") return;
    const bounds = (
      event.currentTarget as HTMLButtonElement
    ).getBoundingClientRect();
    const x = ((event.clientX - bounds.left) / bounds.width) * 1140;
    const y = ((event.clientY - bounds.top) / bounds.height) * 180;
    activeRow = Math.max(0, Math.min(9, Math.round((y - 6) / 17.5)));
    activeColumn = Math.max(
      0,
      Math.min(
        19,
        x >= 742
          ? Math.floor((x - 742) / 20.3)
          : Math.floor(((x - 200) / 534) * 20),
      ),
    );
    active = true;
  }
  function step(event: KeyboardEvent) {
    if (reducedMotion) return;
    if (
      ![
        "ArrowLeft",
        "ArrowRight",
        "ArrowUp",
        "ArrowDown",
        "Home",
        "End",
      ].includes(event.key)
    )
      return;
    event.preventDefault();
    if (event.key === "ArrowLeft") activeColumn = Math.max(0, activeColumn - 1);
    if (event.key === "ArrowRight")
      activeColumn = Math.min(19, activeColumn + 1);
    if (event.key === "ArrowUp") activeRow = Math.max(0, activeRow - 1);
    if (event.key === "ArrowDown") activeRow = Math.min(9, activeRow + 1);
    if (event.key === "Home") activeColumn = 0;
    if (event.key === "End") activeColumn = 19;
    active = true;
  }
  function activate() {
    if (reducedMotion) return;
    activeRow = (activeRow + 1) % 10;
    activeColumn = (activeColumn + 1) % 20;
    active = true;
  }

  const rows = Array.from({ length: 10 }, (_, row) => row);
  const cells = Array.from({ length: 200 }, (_, index) => {
    const row = Math.floor(index / 20);
    const column = index % 20;
    const signal =
      (column >= 4 && column < 10 && (row + column) % 4 !== 0) ||
      (row >= 5 && column >= 10 && column < 17 && (row * 3 + column) % 5 < 3);
    return { row, column, signal, gap: (row * 7 + column * 3) % 23 === 0 };
  });
</script>

<figure
  class="execution-field"
  aria-label="Illustration of CPU task coordination connected to parallel GPU tensor computation"
>
  <button
    class="execution-trigger"
    class:active
    type="button"
    disabled={reducedMotion}
    aria-label="Explore the illustrative CPU and GPU execution field"
    aria-describedby="execution-help"
    onpointermove={explore}
    onpointerleave={() => (active = false)}
    onfocus={() => (active = !reducedMotion)}
    onblur={() => (active = false)}
    onkeydown={step}
    onclick={activate}
  >
    <svg
      class="execution-graphic"
      viewBox="0 0 1140 180"
      fill="none"
      aria-hidden="true"
    >
      <path d="M0 77H204V103H0Z" fill="currentColor" />
      {#each rows as row}
        <path
          d={`M200 90C263 90 287 ${6 + row * 17.5} 365 ${6 + row * 17.5}H734`}
          class:lane-active={active && row === activeRow}
          stroke={row === 3 || row === 5 ? "var(--signal)" : "currentColor"}
          stroke-width="2"
        />
      {/each}
      {#each cells as cell}
        <rect
          x={742 + cell.column * 20.3}
          y={cell.row * 17.5}
          width="11"
          height="11"
          class:cell-active={active &&
            (cell.column === activeColumn ||
              (cell.row === activeRow &&
                Math.abs(cell.column - activeColumn) <= 2))}
          fill={cell.gap
            ? "var(--page)"
            : cell.signal
              ? "var(--signal)"
              : "currentColor"}
        />
      {/each}
      {#if active}
        <path
          class="moving-signal"
          d={`M200 90C263 90 287 ${6 + activeRow * 17.5} 365 ${6 + activeRow * 17.5}H734`}
          pathLength="1"
          stroke="var(--signal)"
          stroke-width="3"
          stroke-dasharray="0.055 1"
          stroke-dashoffset={-activeColumn / 24}
        />
      {/if}
    </svg>
  </button>
  <span class="sr-only" id="execution-help"
    >Illustration, not live computation. Move the pointer or use arrow keys to
    explore the lanes and cells. Enter selects another lane.</span
  >
  <figcaption>
    <span>CPU / task coordination</span><span>GPU / tensor compute</span>
  </figcaption>
</figure>

<style>
  .execution-field {
    margin: 44px 0 0;
  }
  .execution-trigger {
    display: block;
    width: 100%;
    padding: 0;
    border: 0;
    background: transparent;
    color: inherit;
    cursor: crosshair;
    touch-action: pan-y pinch-zoom;
  }
  .execution-trigger:disabled {
    cursor: default;
    opacity: 1;
  }
  .execution-trigger:focus-visible {
    outline: 2px solid var(--signal);
    outline-offset: 9px;
  }
  .execution-trigger svg path,
  .execution-trigger svg rect {
    transition:
      stroke 160ms ease-out,
      fill 160ms ease-out,
      opacity 160ms ease-out;
  }
  .execution-trigger.active svg rect {
    opacity: 0.62;
  }
  .execution-trigger.active svg rect.cell-active {
    opacity: 1;
    fill: var(--signal);
  }
  .execution-trigger.active svg path.lane-active {
    stroke: var(--signal);
  }
  .execution-trigger .moving-signal {
    transition: stroke-dashoffset 200ms cubic-bezier(0.16, 1, 0.3, 1);
  }
  @media (prefers-reduced-motion: reduce) {
    .execution-trigger svg path,
    .execution-trigger svg rect {
      transition: none;
    }
  }
  .execution-graphic {
    display: block;
    width: 100%;
    height: auto;
    color: var(--ink);
  }
  figcaption {
    display: grid;
    grid-template-columns: 65% 35%;
    margin-top: -14px;
    padding-left: 4px;
    font:
      12px/1.5 "Fira Code Variable",
      monospace;
    letter-spacing: 0.04em;
    color: #d2d2d2;
  }
  figcaption span {
    padding: 9px 16px;
    border-left: 1px solid #8b8b8b;
  }
  @media (max-width: 760px) {
    .execution-field {
      margin-top: 40px;
    }
    figcaption {
      font-size: 10px;
      grid-template-columns: 53% 47%;
      padding-left: 0;
      margin-top: 12px;
    }
    figcaption span {
      padding: 7px 9px;
      letter-spacing: 0;
    }
  }
  @media (max-width: 359px) {
    figcaption {
      font-size: 9px;
    }
  }
</style>
