# Kalandra

<!-- impeccable:product-schema 1 -->

## Platform

Web. This repository's SvelteKit website introduces the native Kalandra programming language and hosts its native-only Tiramisu documentation.

## Users

Developers exploring native CPU/GPU programming and the numerical foundations of AI and LLM workloads. Explain what Kalandra is, show real source, and lead directly to documentation and tooling.

## Product Purpose

Kalandra is a native mixed CPU/GPU language. Typed CPU workers, explicit tensor transfers, placed GPU tasks and structured parallel scopes share one language.

## Capabilities and Constraints

Native sources use `.k`. Apple Metal is the current GPU backend, targeting macOS 26+ on Apple silicon. Linux and Windows support CPU execution. GPU numerical programs support ten numeric types, dense tensor ranks zero through eight, rank-two matrix products, balanced sums, explicit conversions and resident numerical/control graphs. The toolchain includes a compiler, checker, formatter, language server and VS Code extension. Hardware float32 matrix products support optimized accumulation and an explicit fixed-tree policy; GPU float64 requires software fallback. Model loading, a full LLM inference runtime and CUDA remain outside current release scope. Do not fabricate benchmarks, customers, release availability or inference support.

## Brand Commitments

Retain the Kalandra name and the exact selected round-four B offset-merge mark in website, documentation and favicon assets. Emphasize native CPU/GPU programming and AI/LLM foundations. Website and documentation default to black/dark. No green, emojis or JavaScript-era public content; the earlier implementation's history belongs in a future blog article.

## Current Approved Direction

On 2026-10-30 the user requested clean Swiss design, supplied `textures.dev` and `https://x.com/0xhammermann/status/2115840592144207889` as references, reviewed an image-generated proposal, and approved implementation. This supersedes the previous violet sculptural landing page and all earlier rejected visual explorations.

The implemented homepage uses near-black, white and vermilion; heavy Geist typography; an asymmetric editorial grid; fine rules; rectangular actions; and a countable CPU-to-GPU execution illustration. The illustration represents task coordination and tensor computation, not live execution or measured telemetry. Interaction is brief and user initiated, with a static reduced-motion state. Use the approved comp and the current surface brief for homepage composition; DESIGN.md records reusable visual rules. Documentation now inherits the approved Swiss identity through local Tiramisu component overrides: near-black, white and vermilion, square controls, a ruled header and readable column grid. Preserve search, navigation, source actions, theme choice and page metadata rather than rebuilding documentation behavior.

## Stack

SvelteKit 5, static adapter, Bun build, self-hosted Geist and Fira Code fonts, native-only Tiramisu documentation. The local component barrel overrides DocsLayout, DocPage, PrevNextNav, Callout and CodeBlock; unchanged kit components remain re-exported. Preview deployments use the existing Kalandra Vercel project. Preserve semantic HTML, keyboard access, responsive layouts and reduced-motion behavior.

## Evidence on Hand

- `native/README.md` and the current GPU/platform documentation establish product capabilities.
- `native/metal-source/matrix-graph.k`, `native/metal-source/pre-matmul.k` and `native/benchmarks/gpu.k` supply homepage code examples.
- `apps/website/src/routes/+page.svelte` and `apps/website/src/lib/components/ExecutionField.svelte` implement the approved homepage.
- `apps/website/.impeccable/surfaces/apps-website-src-routes-page-svelte.md` records the current surface contract.
- Local approval evidence: `.impeccable/mocks/swiss/approved.png` and its approval sidecar.
