# Native call convention: paired comparison

Identical source algorithms and driver, Clang -O2 and strict floating order. Each compiler links its own frozen optimized runtime. Runtime layout is unchanged. Invocation lifecycle, arithmetic checks, cancellation and physical admission are included. The baseline retains its old logical limit; current uses the new internal convention without that limit. All checksums match independent Fraction/integer/GCD references. Observed counters are diagnostic controls, not a promise of unchanged poll counts. This is a compiler/runtime comparison, not a cross-language or GPU ranking.

Nine adjacent ABBA/BAAB blocks per algorithm, mode and depth; fresh processes, two warmups and three measured batches. Raw paired observations, hashes and commands are retained in the adjacent JSON.

| Algorithm | Mode | Depth | Before µs | After µs | Time change |
| --- | --- | ---: | ---: | ---: | ---: |
| polynomial | unbound | 9 | 1.120 | 0.977 | -12.8% |
| polynomial | unbound | 10 | 1.703 | 1.863 | +9.4% |
| polynomial | unbound | 11 | 2.347 | 3.661 | +56.0% |
| polynomial | unbound | 12 | 16.877 | 7.240 | -57.1% |
| polynomial | unbound | 13 | 16.807 | 14.316 | -14.8% |
| polynomial | unbound | 14 | 26.323 | 28.687 | +9.0% |
| polynomial | bound | 9 | 1.830 | 2.512 | +37.3% |
| polynomial | bound | 10 | 4.476 | 4.904 | +9.6% |
| polynomial | bound | 11 | 7.152 | 9.756 | +36.4% |
| polynomial | bound | 12 | 17.730 | 19.520 | +10.1% |
| polynomial | bound | 13 | 28.117 | 38.817 | +38.1% |
| polynomial | bound | 14 | 70.649 | 78.220 | +10.7% |
| polynomial | observed | 9 | 4.898 | 2.990 | -39.0% |
| polynomial | observed | 10 | 10.155 | 5.908 | -41.8% |
| polynomial | observed | 11 | 19.457 | 11.786 | -39.4% |
| polynomial | observed | 12 | 40.160 | 23.378 | -41.8% |
| polynomial | observed | 13 | 77.269 | 46.700 | -39.6% |
| polynomial | observed | 14 | 160.381 | 93.379 | -41.8% |
| branches | unbound | 9 | 1.089 | 1.305 | +19.9% |
| branches | unbound | 10 | 1.978 | 2.547 | +28.8% |
| branches | unbound | 11 | 3.040 | 4.950 | +62.8% |
| branches | unbound | 12 | 16.214 | 9.849 | -39.3% |
| branches | unbound | 13 | 16.353 | 19.771 | +20.9% |
| branches | unbound | 14 | 30.801 | 39.443 | +28.1% |
| branches | bound | 9 | 2.126 | 2.508 | +17.9% |
| branches | bound | 10 | 4.793 | 4.917 | +2.6% |
| branches | bound | 11 | 8.266 | 9.848 | +19.1% |
| branches | bound | 12 | 19.082 | 19.561 | +2.5% |
| branches | bound | 13 | 32.622 | 38.810 | +19.0% |
| branches | bound | 14 | 75.318 | 77.073 | +2.3% |
| branches | observed | 9 | 4.906 | 3.178 | -35.2% |
| branches | observed | 10 | 10.148 | 6.229 | -38.6% |
| branches | observed | 11 | 19.305 | 12.402 | -35.8% |
| branches | observed | 12 | 40.471 | 24.658 | -39.1% |
| branches | observed | 13 | 76.866 | 49.158 | -36.0% |
| branches | observed | 14 | 161.434 | 98.757 | -38.8% |
| euclid | unbound | 9 | 9.220 | 5.344 | -42.0% |
| euclid | unbound | 10 | 23.766 | 10.809 | -54.5% |
| euclid | unbound | 11 | 42.550 | 21.363 | -49.8% |
| euclid | unbound | 12 | 102.237 | 45.071 | -55.9% |
| euclid | unbound | 13 | 179.141 | 88.664 | -50.5% |
| euclid | unbound | 14 | 417.783 | 177.760 | -57.5% |
| euclid | bound | 9 | 14.969 | 6.976 | -53.4% |
| euclid | bound | 10 | 32.773 | 14.300 | -56.4% |
| euclid | bound | 11 | 63.467 | 29.024 | -54.3% |
| euclid | bound | 12 | 135.870 | 57.701 | -57.5% |
| euclid | bound | 13 | 257.852 | 116.662 | -54.8% |
| euclid | bound | 14 | 545.548 | 233.194 | -57.3% |
| euclid | observed | 9 | 39.911 | 20.835 | -47.8% |
| euclid | observed | 10 | 81.930 | 41.837 | -48.9% |
| euclid | observed | 11 | 162.892 | 84.733 | -48.0% |
| euclid | observed | 12 | 330.178 | 170.921 | -48.2% |
| euclid | observed | 13 | 645.023 | 342.716 | -46.9% |
| euclid | observed | 14 | 1327.898 | 677.945 | -48.9% |
