# Disaggregated inference: Qwen3-4B across the Spark and the Studio

On 15 September 2026 one request was served by both machines: the prompt was prefilled by vLLM on the DGX Spark, the resulting KV cache was pulled over the CX5 link with RDMA READs, and the reply was decoded by MLX on the M3 Ultra Mac Studio. This note publishes that result with its stage timings and every limitation. It is a first-version hand-off with file staging on both sides, not a finished integration and not a bandwidth or RDMA-versus-TCP benchmark.

The scripts that ran it are not in this repository.

## What ran where

1. **One quantisation, two layouts.** The BF16 `Qwen/Qwen3-4B-Instruct-2507` linear layers were quantised once with MLX's MXFP4 quantiser (group size 32, E8M0 scales) and written twice: as an mlx-lm checkpoint and as a compressed-tensors `mxfp4-pack-quantized` checkpoint holding the same codes and scales. Embeddings and norms stayed BF16 in both. Checked layers dequantised identically in MLX and in an independent decoder of the vLLM bytes, with a difference of 0.0. The relative weight error of MXFP4 itself is about 12 %.
2. **Prefill on the Spark.** vLLM (image `vllm/vllm-openai:qwen38-flash-next`, sha256 `d464f3b4…`) served the compressed-tensors checkpoint with `VLLM_DISABLED_KERNELS=FlashInferMxFp4LinearKernel,…` so that the weight-only Marlin kernel ran; FlashInfer on this GPU would quantise activations to FP4 and change the cache. A KV connector ran a flagged request with `max_tokens=1` and wrote every layer's keys and values for the prompt, in MLX order, to a tmpfs file on the Spark.
3. **KV over RDMA.** A small C program registered that file on the Spark as one memory region; the Studio pulled it with 2 MiB RDMA READs through the native provider on the BlueFlame-64 userspace path at RDMA path MTU 4096, then copied it into a new file mapping.
4. **Decode on the Studio.** The transferred tensors were loaded into mlx-lm's KV cache, the token vLLM sampled was fed in, and MLX decoded the reply.

Software: lab driver 0.1.17 on the Studio; MLX 0.32.2 and mlx-lm 0.31.3 from oMLX on the Studio; vLLM in the container above on the Spark. Hashes of every tool and checkpoint were recorded with the run and remain outside this repository.

## Results

Same needle-in-a-haystack prompt three ways, greedy sampling, 128 generated tokens. **Split**: first token from the Spark prefill, decode on the Studio after the pull. **Studio-only**: MLX prefill of the same tokens, then the same MLX decode. **Spark-only**: one streamed vLLM request with a fresh cache salt (compiled, CUDA graphs).

| Prompt tokens | KV MiB | RDMA pull s (Gbit/s) | Split first token s | Studio-only first token s | Spark-only first token s | Studio decode tok/s | Spark decode tok/s | 128-token reply: split / Studio-only / Spark-only s | Answer |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 977 | 137 | 0.04 (29.0) | 0.20 | 0.39 | 0.15 | 161 | 68 | 1.05 / 1.19 / 2.01 | correct |
| 3,852 | 542 | 0.13 (35.2) | 0.76 | 1.63 | 0.53 | 147 | 60 | 1.84 / 2.50 / 2.63 | correct |
| 7,702 | 1,083 | 0.24 (37.4) | 1.56 | 3.62 | 1.15 | 131 | 53 | 2.97 / 4.62 / 3.57 | wrong, same as local MLX |
| 15,402 | 2,166 | 0.48 (37.7) | 3.52 | 8.81 | 2.69 | 109 | 42 | 5.55 / 10.03 / 5.72 | correct |
| 28,852 | 4,057 | 0.90 (38.0) | 8.08 | 22.32 | 6.51 | 83 | 31 | 11.21 / 23.94 / 10.62 | wrong, same as local MLX |

What the table shows:

- MLX decode on the Studio ran at 2.4–2.7× the Spark's decode rate at every context length. Studio-only prefill of the same tokens took 2.0–2.8× as long as the split first token, transfer included.
- The split gave the fastest 128-token reply up to 15,402 tokens. At that length it took 5.55 s against 10.03 s Studio-only, about 1.8× the reply speed. At 28,852 tokens Spark-only finished 0.591 s sooner than the split because of the hand-off overhead itemised below; longer replies shift that balance back toward the split, because decode on the Studio is faster.
- The hand-off did not change the model's answers. In every run the first token matched MLX's own prefill, the KV cosine similarity was at least 0.986 on every layer, and the needle answer from the transferred cache was the same as from a fully local MLX run. The two wrong answers are the 4B model missing the needle; local MLX got them wrong too.
- The greedy outputs from the transferred cache and from the fully local run shared identical prefixes of 55, 70, 128, 11 and 128 tokens across the five contexts. Full 128-token output was identical at 7,702 and 28,852 tokens only; this note makes no claim of identical full output at every length.
- The largest transfer moved a 4,057 MiB cache in 0.90 s, a reported 37.97 Gbit/s, rounded to 38 in the README. That is a workload observation from first-version plumbing, not a bandwidth benchmark; see [the README on throughput](../README.md#5-verify-transfers-and-check-your-speeds).

## Where the hand-off time goes (28,852 tokens)

| Stage | Seconds |
|---|---:|
| Connector dump into fresh tmpfs pages on the Spark | 1.40 |
| RDMA pull, of which copying into a new file mapping | 0.90 (0.45) |
| Reading the file back into MLX on the Studio | 0.61 |
| First decode step | 0.11 |

All of that is first-version plumbing: page faults on fresh buffers, a file on each side and an extra copy. Persistent pre-registered buffers on the Spark, pulling straight into preallocated MLX buffers on the Studio, and streaming each layer while prefill is still running would remove most of it. None of those changes has been made or measured yet.

## Limitations

- **Reply times are estimated from measured stages.** Each reported reply time is the sum of separately measured stages; it excludes SSH setup and reuses the split run's MLX decode rate for the Studio-only baseline. No complete client request was timed independently end to end.
- **This is not RDMA versus TCP.** No matched TCP transport of the same cache was measured, so the table does not isolate what RDMA contributes over an ordinary network hand-off.
- **File-staged hand-off.** The cache went through a tmpfs file on the Spark and a new file mapping on the Studio. The RDMA pull is one stage of four, and the other three are copies and page faults that a later version is meant to remove.
- **One model, one prompt family, one sweep.** Five prompt lengths with a needle-in-a-haystack prompt, greedy sampling and 128 output tokens, from a single sweep with no repeat count reported; no other models and no concurrent requests. The 4B model gave two wrong answers on its own; that is the model, not the transport.
- **Not an installed integration.** The connector, transfer program and decode driver are separate scripts outside this repository. Installing MCDMA does not connect vLLM or MLX to this path.
- **Not a GPU-memory result.** The Studio side read the cache from a file mapping into MLX. This run does not extend the separately validated Metal shared-buffer path, and the throughput figure is not a NIC benchmark.

The [README](../README.md#integration-with-inference-engines) describes the engine integration work that remains.
