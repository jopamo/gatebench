<div style="text-align:center;">
  <div style="background-color:#1e1e1e; padding:1em; display:inline-block; border-radius:8px;">
    <img src="assets/gatebench.png" alt="logo" width="300" style="display:block;">
  </div>
</div>

`gatebench` is a Linux CLI for exercising and benchmarking `tc gate` (`act_gate`) control-plane operations over rtnetlink.
It is not a packet-forwarding performance tool and does not measure end-to-end traffic latency.

## Why this exists

If you are working on kernel/networking behavior around `tc gate`, you need repeatable control-plane load with known message shapes, plus a way to stress timing-sensitive races.

A practical example: before and after a kernel change, you can run the same create/replace loop and compare observed ops/sec and failure patterns. If behavior regresses, you can switch to `--race` mode to increase overlap between conflicting netlink operations and inspect where failures concentrate.

## Quickstart (fastest path to success)

### 1) Build

```bash
meson setup build-meson-release --buildtype=release
meson compile -C build-meson-release
```

For a full static build, run:

```bash
# one-time tooling (package names vary by distro):
# git meson ninja pkg-config make autoconf automake libtool flex bison

# 1) restore bundled dependency sources
git submodule sync --recursive
git submodule update --init --recursive libmnl libpcap

# 2) build local static libmnl/libpcap into deps/install
./tools/build_deps.sh

# 3) configure + compile gatebench as a static binary
rm -rf build-meson-release
meson setup build-meson-release --buildtype=release -Ddeps_prefix=deps/install
meson compile -C build-meson-release

# 4) verify static linkage
file build-meson-release/src/gatebench
ldd build-meson-release/src/gatebench || true
```

Expected verification output shape:
- `file ...`: contains `statically linked` or `static-pie linked`
- `ldd ...`: prints `statically linked` or `not a dynamic executable`

To use Clang explicitly instead, prefix setup with `CC=clang`.

If you want to force system libraries instead of local deps:

```bash
meson setup build-meson-release --reconfigure -Ddeps_prefix=""
meson compile -C build-meson-release
```

### 2) Run a 1-second race smoke test

```bash
./build-meson-release/src/gatebench --race --seconds=1
```

Expected output shape:

```text
Running race mode for 1 seconds...
Race thread CPUs: replace=... dump=... get=... traffic=... basetime=... delete=... invalid=... traffic_sync=...
Race fuzzy sync: replace/delete + basetime/invalid + dump/get + traffic/traffic_sync pairs enabled
Race mode completed (1 seconds)
  Replace ops: ..., errors: ...
  ...
  <thread> error breakdown:
```

What just happened: gatebench launched concurrent worker threads, synchronized key thread pairs with fuzzy timing windows, then printed per-thread operation and error summaries.

## Common workflows

### Workflow 1: pre-flight your environment before benchmarking

Goal: verify whether your host/kernel permissions and `tc gate` support are usable.

```bash
./build-meson-release/src/gatebench --verbose
```

Look for:
- `Selftests: OK` before benchmark starts.

Common mistake + fix:
- Mistake: seeing many selftest failures with `got -1` / `Operation not permitted`.
- Fix: run with `CAP_NET_ADMIN` (for example via `sudo`) and ensure your kernel includes `tc gate` support.

### Workflow 2: run a controlled benchmark sweep on one CPU

Goal: compare control-plane throughput under fixed schedule size.

```bash
sudo ./build-meson-release/src/gatebench \
  --cpu=2 --iters=2000 --warmup=200 --runs=5 \
  --entries=32 --interval-ns=1000000 --index=12000
```

Look for:
- per-run lines like `Run 1/5... done (<number> ops/sec)`.
- terminal `Benchmark completed successfully`.

Common mistake + fix:
- Mistake: `--sample-every` greater than `--iters`.
- Fix: keep `sample-every <= iters`.

### Workflow 3: hunt race windows, not average speed

Goal: maximize overlap between conflicting operations.

```bash
./build-meson-release/src/gatebench --race --seconds=30 --verbose
```

Look for:
- per-thread `ops/errors` totals.
- error/extack breakdown concentration by thread.
- verbose fuzzy-sync logs indicating sampling completed and random delay range activation.

Common mistake + fix:
- Mistake: treating all non-zero errors as tool failure.
- Fix: inspect breakdown; `Operation not permitted (1)` means privilege issue, not race detection.

### Workflow 4: verify dump multipart behavior and capture nlmon traffic

Goal: validate dump semantics and save packet capture for inspection.

```bash
sudo ./build-meson-release/src/gatebench \
  --dump-proof --pcap=/tmp/gatebench-nlmon.pcap --nlmon-iface=nlmon0
```

Look for:
- `Dump proof summary:` with multipart and payload counters.
- `pcap capture: /tmp/gatebench-nlmon.pcap (iface nlmon0)`.

Common mistake + fix:
- Mistake: pcap requested but binary lacks pcap support.
- Fix: rebuild with pcap enabled (`-Dpcap=enabled`) and confirm build includes libpcap.

## Concepts you must understand

### 1) Control-plane benchmark, not data-plane benchmark

This tool measures netlink control transactions (create/replace/delete/dump/get), including userspace message build, kernel parse/apply, and ack handling. It does not generate forwarding-path traffic measurements.

Wrong assumption: "ops/sec here equals packet forwarding throughput."
Correction: it only describes control-plane update behavior.

### 2) Selftests are a gate before normal benchmark/dump-proof mode

In non-race mode, gatebench runs an internal/stable/historical/unpatched selftest suite first. Hard failures stop execution; soft-fails are reported and can still allow continuation.

Wrong assumption: "`--dump-proof` skips selftests."
Correction: selftests still run first unless you use `--race` mode.

### 3) Index ownership is real state, not a label

`--index` selects the tc action index gatebench creates/replaces/deletes. Reusing an index that another process manages causes collisions and misleading errors.

Wrong assumption: "index only changes output grouping."
Correction: index is the kernel object identity for operations.

### 4) Race mode is synchronized contention, not deterministic replay

`--race` mode runs several worker threads and uses fuzzy synchronization windows to increase overlap probability across operation pairs. It improves race exposure probability but does not guarantee identical timing across runs.

Wrong assumption: "same seed/time always reproduces same interleaving."
Correction: scheduler/kernel timing still dominates exact ordering.

## Configuration

Most impactful options:

| Option | Default | Semantics |
|---|---:|---|
| `--iters` | `1000` | benchmark iterations per run; each iteration performs create+replace. |
| `--warmup` | `100` | warmup loop count before timed benchmark phase. |
| `--runs` | `5` | number of independent benchmark runs. |
| `--entries` | `64` (capped at 64) | schedule entry count for generated gate list. |
| `--interval-ns` | `1000000` | interval per entry in ns (`>0`; very large values can fail validation paths). |
| `--index` | `1000` | tc action index used for create/replace/delete/get/dump. |
| `--timeout-ms` | `1000` | netlink receive timeout per request. |
| `--cpu` | `-1` | pin main thread to one CPU (`-1` disables pinning). |
| `--sample-every` | `0` (off) | record every Nth benchmark iteration sample (`N <= iters`). |
| `--race` + `--seconds` | off / `60` | run concurrent race workload for fixed duration. |
| `--dump-proof` | off | run dump multipart proof harness after selftests. |
| `--pcap` + `--nlmon-iface` | off / `nlmon0` | enable nlmon capture during dump-proof. |
| `--clockid`, `--base-time`, `--cycle-time`, `--cycle-time-ext` | `CLOCK_TAI`, `0`, `0`, `0` | gate schedule timing fields passed into action messages. |

Safe config example (repeatable and moderate resource use):

```bash
sudo ./build-meson-release/src/gatebench \
  --cpu=1 --iters=2000 --warmup=200 --runs=5 \
  --entries=32 --interval-ns=1000000 --timeout-ms=1000
```

Dangerous config example (very long runtime + large sample pressure):

```bash
./build-meson-release/src/gatebench \
  --iters=50000000 --runs=20 --sample-every=1 --timeout-ms=10000
```

Why dangerous: very high iteration and run counts increase total wall time and memory consumed by stored latency samples.

## Operational notes

- Performance model:
  - benchmark mode performs two timed netlink transactions per iteration (`create` + `replace`), plus warmup and cleanup calls.
  - race mode uses 8 worker threads with paired fuzzy-sync windows.
- Memory behavior:
  - benchmark samples are stored in memory for percentile/stat calculation.
  - rough sample count is `2 * iters` when sampling is off, or `~2 * (iters / sample_every)` when sampling is on.
- Logging controls:
  - `--verbose` enables detailed config/environment + detailed selftest output.
  - in race mode, `--verbose` also enables fuzzy-sync sampling/delay diagnostics.
- State/artifacts:
  - kernel state: tc gate actions at selected `--index` values (tool attempts cleanup).
  - filesystem artifacts: optional pcap output path only; no persistent app DB/cache.

## Troubleshooting

- **Symptom:** many failures show `Operation not permitted (1)`.
  - Likely cause: missing `CAP_NET_ADMIN`.
  - Confirm: selftests show many `got -1`; race breakdown dominated by errno `1`.
  - Fix: run with sufficient privileges and verify namespace/capabilities.

- **Symptom:** `Selftests failed: Invalid argument (-22)` even with privileges.
  - Likely cause: kernel lacks expected `tc gate` behavior/support.
  - Confirm: stable selftests fail on basic create/replace semantics.
  - Fix: run on a kernel with `act_gate` support aligned with expected behavior.

- **Symptom:** dump-proof with `--pcap` fails early.
  - Likely cause: binary built without libpcap support or bad nlmon interface.
  - Confirm: stderr prints `pcap support not built; rebuild with -Dpcap=enabled` or `pcap_open_live(...) failed`.
  - Fix: rebuild with pcap enabled; ensure `nlmon` interface exists and is up.

- **Symptom:** CLI rejects sampling config.
  - Likely cause: invalid relationship between `--sample-every` and `--iters`.
  - Confirm: `Error: sample-every cannot exceed iterations`.
  - Fix: choose `sample-every <= iters`.

- **Symptom:** command appears to run with `--json`, but downstream JSON parser fails.
  - Likely cause: current JSON output path is incomplete/placeholder for results.
  - Confirm: output contains config header but no structured benchmark/race result object.
  - Fix: use text mode for operational runs today; treat JSON mode as unstable.

- **Symptom:** build fails during static link probing.
  - Likely cause: static libmnl/libpcap dependency chain not available in system pkg-config metadata.
  - Confirm: meson reports static link probe failure.
  - Fix: run `./tools/build_deps.sh`, then reconfigure/rebuild.

## Limitations and non-goals

- Does not measure data-plane forwarding performance.
- No skip-selftests mode for normal benchmark/dump-proof paths.
- Benchmark summary output is minimal in current CLI flow (run progress + success line), not a full rendered report.
- JSON output is not yet a stable, complete result schema.
- Entry count is capped to `64`.
- Race mode increases race probability; it does not provide deterministic replay of exact interleavings.

## Compatibility & stability

- Platform: Linux only.
- Kernel expectation: `tc gate` (`act_gate`) support required for meaningful non-race benchmark/proof execution.
- Build compatibility: GCC or Clang via Meson/Ninja; optional libpcap support.
- Stability promises:
  - CLI/output contracts should be treated as evolving in current `0.1.0` state.
  - JSON output and human-readable benchmark report format are not yet stable interfaces.

## Acknowledgments

- Linux kernel networking team for the `tc gate` action
- `libmnl` authors for the netlink library
- `iproute2` maintainers for `tc` reference behavior
- Inspired by kernel selftests and benchmarking tools
