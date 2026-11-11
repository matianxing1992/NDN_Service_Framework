Publication includes packet generation, grant, durable INSERT, Producer withdrawal and one current writer-bound manifest; no automatic full readback. Retrieval is an independent Repo fetch of every group, hashing original wires and complete payload. CPU is benchmark process CPU; Server CPU/interfaces/I/O use enclosing 1-second samples, overlap group boundaries and must not be summed. Interfaces include control, retries and framing. CS is zero; temporary Producer is withdrawn. Application stores start empty; OS page cache is uncontrolled. Publication uses at most 3 concurrent commits. Per-group publication and publication-commit walls/process CPU overlap; only phase totals give throughput. Failed or ambiguous commit geometry describes attempted bytes, not verified transfer.

Additive native partition (group rows below are nested):

| Interval | Wall s | Client CPU s |
| --- | ---: | ---: |
| bootstrap | 0.220725 | 0.061118 |
| publication-total | 57.386606 | 37.824542 |
| retrieval-total | 45.429499 | 22.615368 |
| Native orchestration/record residual | 0.003944 | 0.001353 |

Owner launch/materialization and final actor teardown are outside this native partition; their separate times/CPU are unavailable. Driver wall/CPU are in driver.log. The native complete scope ends before final ServiceUser teardown.


| Phase/group | Payload MB | Original-wire MB | Wall s | Client CPU s | Payload MB/s | Wire MB/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| bootstrap | unavailable | unavailable | 0.221 | 0.061 | unavailable | unavailable |
| publication-commit/0 | 28.672000 | 29.146880 | 8.569 | 6.386 | 3.346 | 3.401 |
| publication/0 | 28.672000 | 29.146880 | 8.978 | 6.800 | 3.194 | 3.246 |
| publication-commit/1 | 28.672000 | 29.146880 | 8.157 | 5.775 | 3.515 | 3.573 |
| publication/1 | 28.672000 | 29.146880 | 9.009 | 7.238 | 3.183 | 3.235 |
| publication-commit/2 | 28.672000 | 29.146880 | 15.402 | 8.710 | 1.862 | 1.892 |
| publication/2 | 28.672000 | 29.146880 | 15.862 | 10.006 | 1.808 | 1.838 |
| publication-commit/3 | 28.672000 | 29.146880 | 7.811 | 4.643 | 3.671 | 3.731 |
| publication-commit/4 | 28.672000 | 29.146880 | 7.403 | 3.900 | 3.873 | 3.937 |
| publication/3 | 28.672000 | 29.146880 | 15.825 | 9.720 | 1.812 | 1.842 |
| publication/4 | 28.672000 | 29.146880 | 8.628 | 6.629 | 3.323 | 3.378 |
| publication-commit/5 | 28.672000 | 29.146880 | 7.954 | 6.036 | 3.605 | 3.664 |
| publication/5 | 28.672000 | 29.146880 | 15.265 | 9.566 | 1.878 | 1.909 |
| publication-commit/6 | 28.672000 | 29.146880 | 7.606 | 5.235 | 3.770 | 3.832 |
| publication/6 | 28.672000 | 29.146880 | 8.611 | 7.257 | 3.330 | 3.385 |
| publication-commit/7 | 28.672000 | 29.146880 | 15.698 | 9.126 | 1.826 | 1.857 |
| publication/7 | 28.672000 | 29.146880 | 16.161 | 10.392 | 1.774 | 1.804 |
| publication-commit/9 | 28.672000 | 29.146880 | 7.970 | 4.143 | 3.598 | 3.657 |
| publication-commit/8 | 28.672000 | 29.146880 | 8.636 | 5.389 | 3.320 | 3.375 |
| publication/8 | 28.672000 | 29.146880 | 16.118 | 9.966 | 1.779 | 1.808 |
| publication/9 | 28.672000 | 29.146880 | 9.498 | 7.418 | 3.019 | 3.069 |
| publication-commit/11 | 28.672000 | 29.150976 | 7.788 | 4.962 | 3.681 | 3.743 |
| publication-commit/10 | 28.672000 | 29.150976 | 8.382 | 5.968 | 3.421 | 3.478 |
| publication/10 | 28.672000 | 29.150976 | 16.358 | 10.120 | 1.753 | 1.782 |
| publication/11 | 28.672000 | 29.150976 | 8.861 | 6.881 | 3.236 | 3.290 |
| publication-commit/12 | 28.672000 | 29.150976 | 13.538 | 8.304 | 2.118 | 2.153 |
| publication/12 | 28.672000 | 29.150976 | 13.995 | 9.544 | 2.049 | 2.083 |
| publication-commit/13 | 28.672000 | 29.150976 | 6.024 | 4.397 | 4.759 | 4.839 |
| publication/13 | 28.672000 | 29.150976 | 13.993 | 9.154 | 2.049 | 2.083 |
| publication-commit/14 | 28.672000 | 29.150976 | 8.148 | 6.407 | 3.519 | 3.578 |
| publication/14 | 28.672000 | 29.150976 | 8.624 | 7.318 | 3.325 | 3.380 |
| publication-commit/15 | 28.672000 | 29.150976 | 8.286 | 4.602 | 3.460 | 3.518 |
| publication/15 | 28.672000 | 29.150976 | 13.838 | 8.098 | 2.072 | 2.107 |
| publication-commit/16 | 28.672000 | 29.150976 | 7.838 | 3.768 | 3.658 | 3.719 |
| publication/16 | 28.672000 | 29.150976 | 8.293 | 4.616 | 3.457 | 3.515 |
| publication-commit/17 | 12.576000 | 12.785993 | 7.061 | 1.954 | 1.781 | 1.811 |
| publication/17 | 12.576000 | 12.785993 | 9.209 | 4.031 | 1.366 | 1.388 |
| publication-total | 500.000000 | 508.311625 | 57.387 | 37.825 | 8.713 | 8.858 |
| retrieval/0 | 28.672000 | 29.146880 | 2.297 | 1.287 | 12.483 | 12.690 |
| retrieval/1 | 28.672000 | 29.146880 | 2.622 | 1.284 | 10.935 | 11.116 |
| retrieval/2 | 28.672000 | 29.146880 | 2.310 | 1.285 | 12.410 | 12.615 |
| retrieval/3 | 28.672000 | 29.146880 | 2.359 | 1.273 | 12.154 | 12.355 |
| retrieval/4 | 28.672000 | 29.146880 | 2.334 | 1.309 | 12.285 | 12.489 |
| retrieval/5 | 28.672000 | 29.146880 | 2.633 | 1.292 | 10.891 | 11.071 |
| retrieval/6 | 28.672000 | 29.146880 | 2.717 | 1.304 | 10.553 | 10.728 |
| retrieval/7 | 28.672000 | 29.146880 | 2.434 | 1.284 | 11.782 | 11.977 |
| retrieval/8 | 28.672000 | 29.146880 | 2.626 | 1.263 | 10.920 | 11.101 |
| retrieval/9 | 28.672000 | 29.146880 | 3.142 | 1.347 | 9.125 | 9.276 |
| retrieval/10 | 28.672000 | 29.150976 | 2.528 | 1.263 | 11.343 | 11.533 |
| retrieval/11 | 28.672000 | 29.150976 | 2.606 | 1.279 | 11.002 | 11.186 |
| retrieval/12 | 28.672000 | 29.150976 | 2.809 | 1.427 | 10.208 | 10.379 |
| retrieval/13 | 28.672000 | 29.150976 | 3.254 | 1.310 | 8.810 | 8.958 |
| retrieval/14 | 28.672000 | 29.150976 | 2.557 | 1.270 | 11.214 | 11.401 |
| retrieval/15 | 28.672000 | 29.150976 | 2.540 | 1.255 | 11.288 | 11.476 |
| retrieval/16 | 28.672000 | 29.150976 | 2.507 | 1.283 | 11.438 | 11.629 |
| retrieval/17 | 12.576000 | 12.785993 | 1.100 | 0.564 | 11.430 | 11.621 |
| retrieval-total | 500.000000 | 508.311625 | 45.429 | 22.615 | 11.006 | 11.189 |
| complete | unavailable | 508.311625 | 103.041 | 60.502 | unavailable | unavailable |
