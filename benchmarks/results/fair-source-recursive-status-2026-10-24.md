# Equivalent source algorithm comparison

Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, invocation admission/cancellation and call-depth checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.

Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.

| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| polynomial | 9 / 512 | 1.137 | 1.232 | 1.093 | 1.313 | 63.603 |
| polynomial | 10 / 1,024 | 1.746 | 2.467 | 2.192 | 2.593 | 132.813 |
| polynomial | 11 / 2,048 | 2.367 | 4.919 | 4.408 | 5.239 | 264.470 |
| polynomial | 12 / 4,096 | 17.026 | 9.953 | 8.855 | 10.581 | 544.888 |
| polynomial | 13 / 8,192 | 17.077 | 19.711 | 17.984 | 20.882 | 1078.887 |
| polynomial | 14 / 16,384 | 26.809 | 39.313 | 35.479 | 41.949 | 2110.066 |
| branches | 9 / 512 | 1.102 | 1.545 | 1.648 | 1.554 | 72.536 |
| branches | 10 / 1,024 | 1.997 | 3.128 | 3.305 | 3.140 | 146.520 |
| branches | 11 / 2,048 | 3.087 | 6.319 | 6.667 | 6.205 | 305.430 |
| branches | 12 / 4,096 | 16.463 | 12.860 | 13.224 | 12.602 | 602.861 |
| branches | 13 / 8,192 | 16.487 | 24.917 | 26.585 | 24.817 | 1206.258 |
| branches | 14 / 16,384 | 31.186 | 49.860 | 53.158 | 50.245 | 2412.004 |
| euclid | 9 / 512 | 9.449 | 3.768 | 4.870 | 4.558 | 220.475 |
| euclid | 10 / 1,024 | 24.145 | 7.757 | 10.193 | 8.958 | 442.555 |
| euclid | 11 / 2,048 | 43.533 | 15.581 | 19.713 | 19.547 | 892.260 |
| euclid | 12 / 4,096 | 103.002 | 30.790 | 40.954 | 38.954 | 1790.632 |
| euclid | 13 / 8,192 | 179.216 | 62.037 | 83.000 | 79.773 | 3505.399 |
| euclid | 14 / 16,384 | 419.778 | 124.780 | 166.423 | 162.625 | 7113.768 |

This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.
