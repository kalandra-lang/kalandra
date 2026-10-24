# Equivalent source algorithm comparison

**Timing limitation:** concurrent external compilation caused substantial scheduling variance. These retained values do not establish an absolute Rust/C++ ranking or achievement of the 10% target. The [adjacent paired controls](tail-return-2026-10-24.md) isolate the token-return improvement more reliably.

Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, invocation admission/cancellation and call-depth checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.

Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.

| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| polynomial | 10 / 1,024 | 4.612 | 4.079 | 2.877 | 3.859 | 246.177 |
| polynomial | 14 / 16,384 | 72.962 | 84.382 | 40.901 | 67.923 | 3822.155 |
| branches | 10 / 1,024 | 4.913 | 3.176 | 3.970 | 3.601 | 283.198 |
| branches | 14 / 16,384 | 80.576 | 52.807 | 130.958 | 79.428 | 5169.859 |
| euclid | 10 / 1,024 | 24.098 | 14.153 | 19.613 | 9.195 | 859.252 |
| euclid | 14 / 16,384 | 413.886 | 270.144 | 356.708 | 288.513 | 12113.474 |

This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.
