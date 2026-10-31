# Benchmark video

8 seconds, 1080×1350, 60 fps, silent H.264 MP4. Ball positions use primary process medians from ../evidence/results.json at real time (1×): each outward leg lasts that language’s measured duration, followed immediately by a return at the same speed, repeating throughout the video. No speed difference is exaggerated. The code panel has no highlighted row and is a static, reformatted version of ../benchmark.k; no algorithms are changed. Kalandra, C++ and Rust are effectively tied on this workload.

Requires Python 3 with Pillow and FFmpeg on PATH. Uses macOS Helvetica Neue/Menlo, or Linux DejaVu fonts.

```sh
python3 native/benchmarks/billion-loops/video/render.py
```

Outputs MP4 and preview PNGs to ignored build/billion-loops/video/. Use --still for previews only. The primary report documents correctness, ordinary and checked controls, background maintenance and limitations. This animation is not a new timing run.
