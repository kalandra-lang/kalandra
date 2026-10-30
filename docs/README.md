# Documentation in this repository

The published documentation lives in [`apps/website/src/content/native`](../apps/website/src/content/native) as Tiramisu sources. It serves language users and external contributors: tutorials, language rules, built-in API reference, practical guides, tool usage and contribution instructions. Keep this material complete without requiring readers to interpret planning or qualification reports. Installation instructions must direct users to GitHub Releases; Actions artifacts are for internal build and qualification workflows.

This directory holds repository documentation:

- [`design/`](design): language and compiler designs, implementation contracts and their supporting evidence. Link relevant designs from the contributor guide; distinguish implemented behavior from proposals.
- [`roadmap.md`](roadmap.md) and the backend/interop roadmaps: project planning, not a reference for available APIs.
- [`native-ai-performance-plan.md`](native-ai-performance-plan.md): optimization planning, experiments and acceptance decisions. Benchmark methodology and results belong in the repository benchmark guides.

Release qualification, per-device kernel selection, experiment outcomes, internal tracking identifiers and organization work plans belong here or in the relevant repository guide, not in published user lessons. User-visible numerical behavior, resource limits, errors, portability requirements and compiler architecture useful to contributors remain appropriate public documentation.

For example, backend selection/licensing details are in [`native/README.md`](../native/README.md) and [`native/third_party/mlx-steel/README.md`](../native/third_party/mlx-steel/README.md); release evidence is in [`design/native-distribution-qualification.md`](design/native-distribution-qualification.md); parser tracking is in [`design/frontend.md`](design/frontend.md). The website explains observable behavior and actionable workarounds without embedding those internal reports.
