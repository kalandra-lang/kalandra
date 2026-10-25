# Equivalent source algorithm comparison

Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, runtime entry/cancellation and physical stack checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.

Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.

| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| polynomial | 9 / 512 | 0.977 | 1.211 | 1.104 | 1.303 | 63.607 |
| polynomial | 10 / 1,024 | 1.886 | 2.465 | 2.224 | 2.573 | 130.199 |
| polynomial | 11 / 2,048 | 3.637 | 4.838 | 4.338 | 5.232 | 262.381 |
| polynomial | 12 / 4,096 | 7.290 | 9.739 | 8.804 | 10.444 | 522.848 |
| polynomial | 13 / 8,192 | 14.441 | 19.702 | 17.583 | 21.040 | 1065.178 |
| polynomial | 14 / 16,384 | 28.916 | 39.316 | 34.787 | 41.706 | 2090.870 |
| branches | 9 / 512 | 1.301 | 1.538 | 1.660 | 1.533 | 71.728 |
| branches | 10 / 1,024 | 2.515 | 3.087 | 3.289 | 3.130 | 147.455 |
| branches | 11 / 2,048 | 5.014 | 6.211 | 6.639 | 6.213 | 299.465 |
| branches | 12 / 4,096 | 9.840 | 12.404 | 13.119 | 12.320 | 598.601 |
| branches | 13 / 8,192 | 19.728 | 24.655 | 26.238 | 24.904 | 1188.776 |
| branches | 14 / 16,384 | 39.436 | 50.278 | 52.424 | 49.782 | 2362.644 |
| euclid | 9 / 512 | 4.988 | 3.628 | 4.733 | 4.371 | 218.480 |
| euclid | 10 / 1,024 | 10.160 | 7.462 | 9.770 | 8.876 | 440.202 |
| euclid | 11 / 2,048 | 20.838 | 14.978 | 19.895 | 19.379 | 869.024 |
| euclid | 12 / 4,096 | 42.374 | 29.739 | 40.090 | 38.283 | 1753.568 |
| euclid | 13 / 8,192 | 83.098 | 60.483 | 81.254 | 78.641 | 3500.800 |
| euclid | 14 / 16,384 | 174.210 | 124.559 | 162.837 | 157.803 | 7020.870 |

This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.
