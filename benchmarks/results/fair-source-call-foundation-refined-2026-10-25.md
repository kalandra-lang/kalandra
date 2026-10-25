# Equivalent source algorithm comparison

Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, runtime entry/cancellation and physical stack checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.

Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.

| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| polynomial | 9 / 512 | 0.981 | 1.223 | 1.091 | 1.283 | 63.998 |
| polynomial | 10 / 1,024 | 1.891 | 2.444 | 2.211 | 2.594 | 129.686 |
| polynomial | 11 / 2,048 | 3.682 | 4.899 | 4.418 | 5.153 | 263.554 |
| polynomial | 12 / 4,096 | 7.290 | 9.762 | 8.839 | 10.382 | 526.732 |
| polynomial | 13 / 8,192 | 14.469 | 19.671 | 17.661 | 20.852 | 1053.257 |
| polynomial | 14 / 16,384 | 28.861 | 39.047 | 35.262 | 41.757 | 2109.005 |
| branches | 9 / 512 | 1.319 | 1.535 | 1.653 | 1.537 | 73.626 |
| branches | 10 / 1,024 | 2.570 | 3.089 | 3.374 | 3.076 | 144.628 |
| branches | 11 / 2,048 | 5.000 | 6.156 | 6.627 | 6.206 | 297.889 |
| branches | 12 / 4,096 | 10.010 | 12.265 | 13.226 | 12.444 | 599.122 |
| branches | 13 / 8,192 | 19.866 | 24.768 | 26.609 | 24.768 | 1210.620 |
| branches | 14 / 16,384 | 39.715 | 49.063 | 52.736 | 49.928 | 2403.049 |
| euclid | 9 / 512 | 3.660 | 3.625 | 4.804 | 4.308 | 219.293 |
| euclid | 10 / 1,024 | 7.472 | 7.432 | 9.720 | 8.960 | 442.635 |
| euclid | 11 / 2,048 | 14.930 | 14.906 | 19.602 | 19.032 | 883.170 |
| euclid | 12 / 4,096 | 29.691 | 29.459 | 39.535 | 39.046 | 1773.517 |
| euclid | 13 / 8,192 | 58.681 | 59.900 | 80.504 | 77.062 | 3544.027 |
| euclid | 14 / 16,384 | 120.488 | 119.442 | 161.215 | 157.760 | 7090.146 |

This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.
