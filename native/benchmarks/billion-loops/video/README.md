# Benchmark video

12 seconds, 1080×1350, 60 fps, silent H.264 MP4. Ball positions use primary process medians from ../evidence/results.json with a common linear 10× slow-motion scale. No speed difference is exaggerated. The code panel is a static, reformatted version of ../benchmark.k; no algorithms are changed. Kalandra, C++ and Rust are effectively tied on this workload.

Requires Python 3 with Pillow and FFmpeg on PATH. Uses macOS Helvetica Neue/Menlo, or Linux DejaVu fonts.

```sh
python3 native/benchmarks/billion-loops/video/render.py
```

Outputs MP4 and preview PNGs to ignored build/billion-loops/video/. Use --still for previews only. The primary report documents correctness, ordinary and checked controls, background maintenance and limitations. This animation is not a new timing run.
