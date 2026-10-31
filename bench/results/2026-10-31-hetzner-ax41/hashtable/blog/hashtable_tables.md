## Hardware counters per operation, real day (operations back to back)

| Container | cycles/op | instructions/op | loads from L2/op | loads from L3/op | loads from DRAM/op | dTLB misses/op | page faults |
|---|---|---|---|---|---|---|---|
| SwissTable (SSE2) | 114 | 73 | 0.22 | 1.34 | 0.55 | 1.84 | 0 |
| LinearProbing | 139 | 57 | 0.10 | 0.44 | 0.64 | 0.94 | 0 |
| SwissTable (AVX2) | 155 | 99 | 0.22 | 1.52 | 0.57 | 1.84 | 0 |
| SwissTable (SWAR) | 164 | 115 | 0.22 | 1.37 | 0.55 | 1.86 | 6 |
| SwissTable (scalar) | 271 | 350 | 0.21 | 1.29 | 0.55 | 1.84 | 0 |
| std::unordered_map | 377 | 121 | 0.60 | 1.28 | 0.95 | 2.65 | 15,030 |
| std::map | 719 | 405 | 4.33 | 3.68 | 0.54 | 17.12 | 30,062 |

## Latency of a single operation, real day (each operation timed on its own)

| Container | p50 | p99 | p99.9 |
|---|---|---|---|
| SwissTable (SSE2) | 51 ns | 355 ns | 675 ns |
| SwissTable (AVX2) | 51 ns | 444 ns | 675 ns |
| SwissTable (SWAR) | 51 ns | 426 ns | 675 ns |
| SwissTable (scalar) | 62 ns | 462 ns | 675 ns |
| LinearProbing | 102 ns | 551 ns | 675 ns |
| std::unordered_map | 102 ns | 818 ns | 995 ns |
| std::map | 142 ns | 995 ns | 1635 ns |
