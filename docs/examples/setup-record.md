# Private MCDMA setup record

Copy this template to `local/setup-record.md` before entering real values.
Do not commit the completed copy or store credentials here.

## Scope and progress

- Date:
- Operator / agent:
- Target: one link / two links / standalone engines:
- Source revision:
- Last passed checkpoint (A-F):
- Current blocker and evidence:
- Next exact action and host:
- Owner actions completed:
- Backup / rollback location:

## Studio

- Management SSH destination:
- OS build / architecture:
- SDK / Xcode:
- Enclosure / NIC / firmware:
- RDMA / security policy state:
- Built driver version / UUID:
- Loaded driver version / UUID:
- Provider hash:
- Client and checker absolute paths / hashes:

## Link inventory

| Value | Spark 1 link | Spark 2 link |
|---|---|---|
| Physical CX5 port label | | |
| Physical CX7 port label | | |
| Studio interface / hardware MAC | | |
| Studio verbs device / GID | | |
| Spark SSH destination / OS | | |
| Spark interface / hardware MAC | | |
| Spark verbs device / GID / RoCE v2 index | | |
| Spark client absolute path / hash | | |
| Link speed / FEC | | |
| Ethernet MTU / RDMA path MTU | | |
| Mac and Spark reciprocal neighbors verified | | |
| Existing inter-Spark interface to preserve | | |

## Transfer evidence

| Link / posting mode | Studio WRITE | Studio READ | Spark WRITE | Spark READ | Private result path |
|---|---|---|---|---|---|
| 1 / kernel | | | | | |
| 1 / direct | | | | | |
| 1 / BF64 | | | | | |
| 2 / kernel | | | | | |
| 2 / direct | | | | | |
| 2 / BF64 | | | | | |

- Concurrent test conditions and result:
- Benchmark payload / depth / MTUs / warmups / repetitions:
- GPU keepalive state:
- Timing boundary and raw samples:
- Persistence installed and post-restart check:

## Optional engines

- vLLM image digest / version:
- oMLX repository / revision / version:
- MLX / MLX-LM versions:
- Model IDs / revisions / quantization / tokenizer:
- Context limit / memory budget:
- Local API ports and request/result files:
- Standalone baselines:
- Actual communication backend:
- MCDMA connector implementation and evidence, or explicitly absent:
