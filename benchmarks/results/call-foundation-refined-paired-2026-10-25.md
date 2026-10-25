# Native call convention: paired comparison

Identical source algorithms and driver, Clang -O2 and strict floating order. Each compiler links its own frozen optimized runtime. Runtime layout is unchanged. Invocation lifecycle, arithmetic checks, cancellation and physical admission are included. The baseline retains its old logical limit; current uses the new internal convention without that limit. All checksums match independent Fraction/integer/GCD references. Observed counters are diagnostic controls, not a promise of unchanged poll counts. This is a compiler/runtime comparison, not a cross-language or GPU ranking.

Nine adjacent ABBA/BAAB blocks per algorithm, mode and depth; fresh processes, two warmups and three measured batches. Raw paired observations, hashes and commands are retained in the adjacent JSON.

| Algorithm | Mode | Depth | Before µs | After µs | Time change |
| --- | --- | ---: | ---: | ---: | ---: |
| polynomial | unbound | 9 | 1.126 | 0.971 | -13.7% |
| polynomial | unbound | 10 | 1.731 | 1.870 | +8.0% |
| polynomial | unbound | 11 | 2.358 | 3.672 | +55.7% |
| polynomial | unbound | 12 | 16.841 | 7.272 | -56.8% |
| polynomial | unbound | 13 | 16.942 | 14.483 | -14.5% |
| polynomial | unbound | 14 | 26.595 | 28.752 | +8.1% |
| polynomial | bound | 9 | 1.840 | 2.504 | +36.1% |
| polynomial | bound | 10 | 4.553 | 4.941 | +8.5% |
| polynomial | bound | 11 | 7.117 | 9.748 | +37.0% |
| polynomial | bound | 12 | 17.933 | 19.579 | +9.2% |
| polynomial | bound | 13 | 28.324 | 39.013 | +37.7% |
| polynomial | bound | 14 | 71.470 | 78.324 | +9.6% |
| polynomial | observed | 9 | 4.905 | 2.989 | -39.1% |
| polynomial | observed | 10 | 10.184 | 5.941 | -41.7% |
| polynomial | observed | 11 | 19.329 | 11.747 | -39.2% |
| polynomial | observed | 12 | 40.328 | 23.597 | -41.5% |
| polynomial | observed | 13 | 77.708 | 47.229 | -39.2% |
| polynomial | observed | 14 | 162.974 | 94.675 | -41.9% |
| branches | unbound | 9 | 1.093 | 1.304 | +19.3% |
| branches | unbound | 10 | 2.017 | 2.551 | +26.5% |
| branches | unbound | 11 | 3.064 | 5.032 | +64.2% |
| branches | unbound | 12 | 16.408 | 9.945 | -39.4% |
| branches | unbound | 13 | 16.551 | 19.840 | +19.9% |
| branches | unbound | 14 | 30.677 | 39.502 | +28.8% |
| branches | bound | 9 | 2.108 | 2.472 | +17.3% |
| branches | bound | 10 | 4.816 | 4.977 | +3.3% |
| branches | bound | 11 | 8.268 | 9.902 | +19.8% |
| branches | bound | 12 | 19.087 | 19.539 | +2.4% |
| branches | bound | 13 | 32.417 | 38.763 | +19.6% |
| branches | bound | 14 | 75.831 | 77.723 | +2.5% |
| branches | observed | 9 | 4.916 | 3.177 | -35.4% |
| branches | observed | 10 | 10.214 | 6.272 | -38.6% |
| branches | observed | 11 | 19.315 | 12.467 | -35.5% |
| branches | observed | 12 | 40.307 | 24.696 | -38.7% |
| branches | observed | 13 | 77.265 | 49.618 | -35.8% |
| branches | observed | 14 | 162.141 | 98.869 | -39.0% |
| euclid | unbound | 9 | 9.231 | 3.713 | -59.8% |
| euclid | unbound | 10 | 23.261 | 7.701 | -66.9% |
| euclid | unbound | 11 | 42.778 | 15.215 | -64.4% |
| euclid | unbound | 12 | 101.275 | 31.133 | -69.3% |
| euclid | unbound | 13 | 177.681 | 61.408 | -65.4% |
| euclid | unbound | 14 | 414.680 | 125.494 | -69.7% |
| euclid | bound | 9 | 14.502 | 6.124 | -57.8% |
| euclid | bound | 10 | 32.734 | 12.358 | -62.2% |
| euclid | bound | 11 | 61.890 | 25.258 | -59.2% |
| euclid | bound | 12 | 136.163 | 51.618 | -62.1% |
| euclid | bound | 13 | 255.277 | 102.882 | -59.7% |
| euclid | bound | 14 | 541.790 | 204.895 | -62.2% |
| euclid | observed | 9 | 40.095 | 18.361 | -54.2% |
| euclid | observed | 10 | 82.068 | 37.207 | -54.7% |
| euclid | observed | 11 | 163.215 | 75.282 | -53.9% |
| euclid | observed | 12 | 330.526 | 151.124 | -54.3% |
| euclid | observed | 13 | 649.381 | 301.155 | -53.6% |
| euclid | observed | 14 | 1327.862 | 607.880 | -54.2% |
