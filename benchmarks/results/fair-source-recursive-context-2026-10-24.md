# Equivalent source algorithm comparison

Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, invocation admission/cancellation and call-depth checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.

Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.

| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| polynomial | 10 / 1,024 | 4.477 | 2.393 | 2.162 | 2.544 | 128.091 |
| polynomial | 14 / 16,384 | 69.479 | 38.840 | 34.997 | 40.864 | 2087.092 |
| branches | 10 / 1,024 | 4.733 | 3.071 | 3.265 | 3.049 | 143.262 |
| branches | 14 / 16,384 | 75.106 | 48.708 | 51.838 | 48.784 | 2355.758 |
| euclid | 10 / 1,024 | 23.343 | 7.263 | 9.472 | 8.750 | 432.508 |
| euclid | 14 / 16,384 | 417.756 | 120.818 | 159.016 | 152.280 | 6916.662 |

This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.
