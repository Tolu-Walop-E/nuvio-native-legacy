# Network, seeking and failover implementation results

Date: 14 October 2026 (measurements), 15 October 2026 (rebased onto `master` on top of #452)

Measurements below were taken with the original implementation (`5e35200b`, based on `17b7769d`). The transport code in this branch is that implementation rebased onto current `master` and #452; the constants, session cache, host budget and retry bounds are unchanged. The measurements were not repeated on the rebased branch.

## Implemented

- F1 position-preserving source failover was already present upstream and was verified in `app.c`, `player.c`, and its existing resume tests.
- F2 rejects automatic replacement sources below the failed source's resolution or Dolby Vision level. Candidate ordering still uses the existing automatic ranking.
- F3 classifies and bounds parallel transport failures. Connect timeout/refusal is tried twice, chunk reads once plus one retry, and exhausted parallel failures bypass Media3's additional retry loop.
- F4 passes the failed final host to the native core, pauses it for candidate validation, and skips same-URL reconnect after the bounded transport confirmation.
- `AJ_REDE_MODO` adds device-local `Parallel` and `Single connection` modes. Default remains `Parallel`; neither mode changes source quality, bitrate, or decoder selection.
- Parallel seeking retains the resolved URL and up to 32 MB of completed chunks for 45 seconds. Same-chunk seeks can reopen without a resolver or range request.
- Active `HttpURLConnection` objects are disconnected on close; cancellation no longer relies on thread interruption alone.
- New connections are limited to four per host per two seconds. Repeated timeout/refusal pauses a host for 10–60 seconds; HTTP 429/503 honors `Retry-After` up to 15 seconds.
- The measured Stage 7 layout is two 16 MB lanes with two chunks total ahead, replacing four 4 MB lanes with twelve chunks ahead.
- The local range/fault server covers 200/206, redirect chains, 403 expiry, 429/503, ignored Range, mid-read close, stall, redirect target switching, large virtual files, a test-addon manifest, and request/connection accounting.
- Media3 release now stays asynchronous on its application looper, and the old `SurfaceView` is cleared before cleanup, avoiding the reviewed UI block and leaked-view ordering bug.

## Shield measurements

These measurements used an NVIDIA Shield TV on the same LAN as the local fault server (`tests/rede_faltas_server.py`), driven by the instrumentation test `RedeDeviceTest`. They do not contact any debrid service or Internet CDN: they are transport measurements on a LAN, not decoder or real-CDN benchmarks, and LAN latency is far lower than a distant CDN's.

### Stage 7 comparison

| Configuration | 32 MiB parallel throughput | Requests for the parallel open | Result |
|---|---:|---:|---|
| Initial 4 × 4 MiB, depth 3, with host bucket | 63.0 Mbps | 10 observed before close | Failed throughput gate |
| Final 2 × 16 MiB, depth 1 | 220.9 Mbps | 3 | Passed and retained |

The final layout was 3.51 times as fast in the controlled before/after run (+251%). It also reduced the cold-open range plan from one bootstrap plus up to twelve chunks to one bootstrap plus two chunks: 13 to 3 requests, a 77% reduction.

### Repeated final-layout runs

Five consecutive 32 MiB runs produced:

- Single connection: 227.1, 149.3, 168.4, 170.0, 157.2 Mbps; median 168.4 Mbps.
- Optimized parallel: 134.5, 121.2, 175.2, 175.6, 188.0 Mbps; median 175.2 Mbps.

Parallel's median was 4.0% higher in this short LAN test. Variance is high, so the result does not justify changing the default to single connection or adding adaptive mode.

### Recovery and warm seek

- Silent TCP-port timeout: surfaced as `FalhaDownloadParalelo` in 7.333 seconds, under the 10-second F3 gate.
- Backward reopen inside the retained 1 MiB bootstrap chunk: 1 ms and zero new requests in the fixture accounting log.
- Final cold parallel open: three range requests and at most two parallel transport connections.
- Passive Shield socket monitor smoke test completed and produced a valid CSV.

## Decision

Keep `Parallel` as the default and expose `Single connection` as an opt-in device setting. Implement Stages 4–7 because the controlled measurements justified session reuse, real cancellation, host backoff, and larger/lower-depth chunks. Do not implement Stage 8 adaptive switching or Stage 9 OkHttp: the evidence does not justify their complexity, and an OkHttp transport is left as a separate decision.

## Verification completed (original implementation)

- Android build (arm64-v8a and armeabi-v7a): pass.
- Kotlin/JVM transport tests: 6 pass.
- Shield instrumentation: 3 pass (single/parallel transfer, bounded refusal, warm backward seek).
- Local fault-server regression: pass.
- Python syntax checks: pass.

Verification of this rebased branch is listed in the pull request.

## Remaining real-media validation

The transport and failure paths are tested, but a logged-in, ordinary-viewing pass is still required for the media-specific gates: five minutes each of Dolby Vision MKV, Dolby Vision MP4 and TrueHD passthrough; normal and rapid seeks; and a 30-minute viewing session. Those checks need a real debrid account, addons and real media, and must not be replaced with synthetic claims.
