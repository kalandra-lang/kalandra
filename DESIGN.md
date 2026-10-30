---
name: Kalandra
description: Swiss editorial design for native CPU and GPU programming.
colors:
  signal: "#f86737"
  signal-hover: "#ff8057"
  page: "#080808"
  ink: "#f6f6f6"
  muted: "#b5b5b5"
  rule: "#555"
  docs-panel: "#101010"
  docs-border: "#444444"
  light-primary: "#b53a17"
typography:
  display:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "clamp(56px, 11.56vw, 184px)"
    fontWeight: 800
    lineHeight: 0.86
    letterSpacing: "-0.04em"
  headline:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "clamp(38px, 4.7vw, 66px)"
    fontWeight: 750
    lineHeight: 1.02
    letterSpacing: "-0.04em"
  title:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "23px"
    fontWeight: 600
    lineHeight: 1.25
    letterSpacing: "-0.025em"
  body:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "17px"
    lineHeight: 1.7
  docs-title:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "clamp(40px, 4.7vw, 70px)"
    fontWeight: 750
    lineHeight: 1.02
    letterSpacing: "-0.04em"
  docs-body:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "16px"
    lineHeight: 1.8
  action:
    fontFamily: '"Geist Variable", sans-serif'
    fontSize: "22px"
    lineHeight: 1.2
  source:
    fontFamily: '"Fira Code Variable", monospace'
    fontSize: "15px"
    lineHeight: 1.6
    fontFeature: '"calt" 1, "liga" 1'
rounded:
  rectangular: "0px"
spacing:
  mobile-gutter: "20px"
  desktop-gutter: "48px"
  section: "80px"
components:
  button-primary:
    backgroundColor: "{colors.signal}"
    textColor: "{colors.page}"
    rounded: "{rounded.rectangular}"
    padding: "16px 32px"
  button-primary-hover:
    backgroundColor: "{colors.signal-hover}"
    textColor: "{colors.page}"
  source-panel:
    backgroundColor: "{colors.page}"
    textColor: "{colors.ink}"
    rounded: "{rounded.rectangular}"
  navigation:
    backgroundColor: "{colors.page}"
    textColor: "{colors.ink}"
---

# Design System: Kalandra

## Overview

**Creative North Star: "One language. CPU and GPU."**

Kalandra uses a precise Swiss editorial system: large, tightly set sans typography, an asymmetric grid and direct geometric illustration. Near-black is the page field; white carries the hierarchy and vermilion marks actions and computational activity. The approved homepage replaces the former violet sculpture without changing the selected offset-merge identity.

Keep native source and factual explanation prominent. The website is expressive through proportion, alignment and contrast; documentation carries the same near-black, white and vermilion identity in a quieter reading layout. The homepage composition is recorded in its surface brief rather than imposed on every future screen.

**Key Characteristics:**

- Heavy Geist display typography and readable Fira Code source.
- Near-black ground, white hierarchy, restrained vermilion signals.
- Fine rules, flat rectangular controls and asymmetric editorial grids.
- Real native examples and a clearly illustrative CPU/GPU execution field.

## Colors

### Primary

Vermilion signal anchors primary actions and selected computational paths. Its lighter hover variant gives direct feedback without changing geometry.

### Neutral

Near-black page, white ink, neutral gray supporting text and fine gray rules form the homepage. Documentation shares the homepage page, ink, muted and signal values. Neutral panels and slightly darker rules organize source, search and navigation. These values live in `apps/website/src/app.css`; an explicit light-mode preference uses a darker orange interaction color for contrast.

**The Signal Rule.** Use vermilion for primary actions, keyboard focus, source keywords and execution emphasis; keep long-form prose neutral.

## Typography

Self-hosted **Geist Variable** supplies display, prose and controls. **Fira Code Variable** supplies code and technical captions, with contextual and common ligatures enabled. The hero uses the display token; its second line is slightly larger (1.04em). Source-section headlines have their own responsive large scale (`clamp(38px, 6vw, 88px)`) while ordinary section headlines use the frontmatter headline token. Supporting paragraphs use comfortable line lengths and generous leading.

**The Source Voice Rule.** Use Geist for prose and controls, and Fira Code for source, signatures, commands and diagram captions.

## Layout

The desktop shell is capped at 1440px with 48px side gutters. The hero introduction occupies the right half of the grid; the execution field spans its width. Two-column source and numerical sections give way to full-width editorial rows and a factual platform table. Main lower sections use 80px vertical padding.

At 1050px, navigation and internal gaps compact. At 760px, side gutters become 20px, navigation becomes a disclosure menu, the introduction and content columns stack, and hero display type becomes 12.35vw with 0.94 line height. Below 359px, gutters reduce to 16px and actions stack. Code and platform tables scroll inside their own regions rather than widening the page.

Documentation uses a 1440px shell with 48px gutters and a three-column navigation/reading/contents grid (205px, flexible, 170px) with 48px gaps. At 1600px the side columns become 225px and 190px with 64px gaps. Below 1280px the contents column disappears; below 1024px the sidebar becomes an inline menu disclosure and reading width is capped at 760px with 32px gutters. Below 601px gutters become 20px and the sticky header reduces from 72px to 64px. Reading prose is capped at 75ch, set at 16px/1.8 on desktop and 15px/1.8 on phones. Documentation titles use the docs-title token, scaling to `clamp(38px, 9.2vw, 54px)` on phones. Body subheads use 29px and 22px desktop sizes. Tables and source retain local horizontal scrolling.

## Elevation & Depth

The homepage is flat. It uses no drop shadows or simulated physical material. Thin borders, large empty fields and contrasting typography define hierarchy. The vector diagram remains crisp at every size.

**The Flat Field Rule.** Separate content with spacing, alignment and thin rules rather than shadows, gradients or raised cards.

## Shapes

Homepage actions and source/terminal frames have square corners. Fine straight rules organize content. The selected offset-merge logo is a fixed identity asset, not a shape to redraw or reinterpret. Documentation shares this square-corner form language, including source frames, pagination, search and callouts.

## Components

### Primary actions

Vermilion rectangular links with dark text, an inline SVG arrow and generous internal spacing. Hover lightens the fill. Keyboard focus uses a visible 2px signal outline offset from the control. Mobile actions remain at least 52px tall.

### Navigation

A neutral wordmark/selected mark and plain text links sit on the page field above a fine rule. At narrow widths a Menu/Close button discloses a vertical list. Escape closes the disclosure and restores focus to its trigger. No floating navigation card or overlay is required.

### Source panel

A flat bordered frame contains a filename select, complete-source link, copy control and scrollable native code. Selecting another fixture changes both displayed and copied content. Copy success/failure uses a live status message. Keywords and return arrows use signal; ordinary source remains neutral.

### Execution field

A vector CPU stem connects to ten lanes and a 20-column by ten-row GPU cell field. Captions distinguish CPU task coordination from GPU tensor computation. Pointer movement, arrow keys and activation briefly emphasize a lane and cells; there is no continuous automatic animation or simulated telemetry. Color/opacity transitions last 160ms; a moving dash uses 200ms. Reduced motion disables interaction and leaves the static field. Touch scrolling and pinch zoom remain available.

### Documentation components and overrides

Documentation uses the same selected identity, restrained signal color, rules and rectangular controls. A sticky header leads to a navigation sidebar, reading column and optional table of contents. Local wrappers preserve the kit's page metadata, source tools and navigation semantics. Search retains the kit SearchDialog inside a modal layer with keyboard focus containment and restoration; Escape and the search shortcut close it. The menu disclosure closes on navigation and Escape. A saved theme choice remains available, with dark as the default. Automatic documentation entrance animation is disabled; reduced motion also removes transitions and smooth scrolling.

Source frames use a thin border, compact language/copy bar, neutral source background and Fira Code at 13px/1.8 (12px on phones). Copy provides success or error feedback through a live status message. Prev/next links are flat, large text paths rather than cards. Callouts use horizontal rules, a semantic label and muted prose rather than colored panels or side stripes.

The local component barrel at `apps/website/src/lib/components/index.ts` exports `DocsLayout`, `DocPage`, `PrevNextNav`, `Callout` and `CodeBlock` overrides while re-exporting unmodified kit components. This follows Tiramisu's [built-in component override pattern](https://tiramisudocs.com/docs/en/framework/customization/custom-components#overriding-built-in-components): keep the expected component export names and compatible props/snippets, and import reusable kit components from `@tiramisu-docs/kit/components` inside local wrappers. Shared presentation is in `apps/website/src/lib/docs.css`; documentation content remains Tiramisu source.

### Editorial rows and tables

Capabilities use horizontal ruled rows, not cards. Numerical operations use Fira Code signatures beside concise factual descriptions. Platform data uses a semantic table with local horizontal scrolling on narrow screens.

## Do's and Don'ts

### Do:

- **Do** preserve the exact selected offset-merge logo geometry.
- **Do** keep factual native CPU/GPU examples, readable contrast and clear documentation paths.
- **Do** preserve keyboard focus, local code/table scrolling and static reduced-motion artwork.
- **Do** carry the Swiss palette, rules and typography into documentation while preserving comfortable reading sizes and kit semantics.

### Don't:

- **Don't** introduce green, emojis, cartoon processors or mascot treatments.
- **Don't** restore violet pills, sculptural hero imagery or JavaScript-era public content.
- **Don't** imply the illustration is live execution, telemetry or a performance result.
- **Don't** claim model loading, complete LLM inference or CUDA support.
