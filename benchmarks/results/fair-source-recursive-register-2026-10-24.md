# Equivalent source algorithm comparison

Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, invocation admission/cancellation and call-depth checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.

Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.

| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| polynomial | 9 / 512 | 1.149 | 1.234 | 1.134 | 1.301 | 64.044 |
| polynomial | 10 / 1,024 | 1.839 | 2.498 | 2.188 | 2.607 | 129.227 |
| polynomial | 11 / 2,048 | 2.409 | 4.889 | 4.348 | 5.177 | 263.144 |
| polynomial | 12 / 4,096 | 16.832 | 9.745 | 8.810 | 10.422 | 526.133 |
| polynomial | 13 / 8,192 | 17.058 | 19.978 | 17.327 | 21.167 | 1040.689 |
| polynomial | 14 / 16,384 | 28.522 | 38.836 | 35.866 | 41.951 | 2144.610 |
| branches | 9 / 512 | 1.140 | 1.578 | 1.655 | 1.574 | 72.814 |
| branches | 10 / 1,024 | 2.225 | 3.109 | 3.364 | 3.142 | 148.994 |
| branches | 11 / 2,048 | 3.083 | 6.187 | 6.706 | 6.342 | 295.587 |
| branches | 12 / 4,096 | 16.401 | 12.664 | 13.176 | 12.574 | 596.882 |
| branches | 13 / 8,192 | 17.546 | 24.986 | 26.367 | 25.138 | 1204.055 |
| branches | 14 / 16,384 | 33.269 | 50.219 | 53.099 | 49.977 | 2422.500 |
| euclid | 9 / 512 | 9.472 | 3.742 | 4.862 | 4.577 | 218.545 |
| euclid | 10 / 1,024 | 23.383 | 7.754 | 10.045 | 9.113 | 433.899 |
| euclid | 11 / 2,048 | 45.229 | 15.206 | 19.258 | 19.579 | 883.335 |
| euclid | 12 / 4,096 | 99.624 | 31.044 | 40.393 | 38.804 | 1779.950 |
| euclid | 13 / 8,192 | 183.043 | 61.641 | 81.158 | 78.511 | 3534.886 |
| euclid | 14 / 16,384 | 424.540 | 125.247 | 159.376 | 157.721 | 7138.756 |

This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.
