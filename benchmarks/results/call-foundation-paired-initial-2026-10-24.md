# Native call convention: paired comparison

Identical source algorithms and driver, Clang -O2 and strict floating order. Each compiler links its own frozen optimized runtime. Runtime layout is unchanged. Invocation lifecycle, arithmetic checks, cancellation and physical admission are included. The baseline retains its old logical limit; current uses the new internal convention without that limit. All checksums match independent Fraction/integer/GCD references. Observed counters are diagnostic controls, not a promise of unchanged poll counts. This is a compiler/runtime comparison, not a cross-language or GPU ranking.

Nine adjacent ABBA/BAAB blocks per algorithm, mode and depth; fresh processes, two warmups and three measured batches. Raw paired observations, hashes and commands are retained in the adjacent JSON.

| Algorithm | Mode | Depth | Before µs | After µs | Time change |
| --- | --- | ---: | ---: | ---: | ---: |
| polynomial | unbound | 12 | 16.865 | 7.234 | -57.1% |
| polynomial | unbound | 14 | 26.514 | 28.934 | +9.1% |
| polynomial | bound | 12 | 17.870 | 19.521 | +9.2% |
| polynomial | bound | 14 | 70.877 | 77.515 | +9.4% |
| polynomial | observed | 12 | 40.313 | 23.445 | -41.8% |
| polynomial | observed | 14 | 161.550 | 93.590 | -42.1% |
| branches | unbound | 12 | 16.243 | 9.802 | -39.7% |
| branches | unbound | 14 | 30.701 | 39.298 | +28.0% |
| branches | bound | 12 | 18.972 | 19.431 | +2.4% |
| branches | bound | 14 | 74.960 | 77.540 | +3.4% |
| branches | observed | 12 | 40.632 | 24.747 | -39.1% |
| branches | observed | 14 | 161.238 | 98.340 | -39.0% |
| euclid | unbound | 12 | 101.555 | 41.330 | -59.3% |
| euclid | unbound | 14 | 417.774 | 167.141 | -60.0% |
| euclid | bound | 12 | 136.776 | 58.446 | -57.3% |
| euclid | bound | 14 | 554.616 | 237.664 | -57.1% |
| euclid | observed | 12 | 332.083 | 176.459 | -46.9% |
| euclid | observed | 14 | 1323.500 | 693.727 | -47.6% |
