<div style="background-color:#1e1e1e; padding:1em; display:inline-block; border-radius:8px; text-align:left;">
  <img src="assets/gatebench.png" alt="logo" width="300" style="display:block; margin:0;">
</div>

A small, self-contained benchmark suite for Linux `tc gate` (act_gate) control-plane operations over rtnetlink.

## Overview

`gatebench` measures the latency and throughput of tc gate action control-plane operations:
- **RTM_NEWACTION replace** cost vs schedule size
- Repeatable measurements with CPU pinning, consistent timing
- Strong error-path coverage via selftests
- Clean separation between benchmark harness and netlink message construction
- Human-readable and JSON output for regression tracking

## Building

### Dependencies
- `libmnl` (libmnl-dev on Debian/Ubuntu)
- `gcc` or `clang`
- `meson`
- `ninja`

### Build commands
```bash
# Setup build directory
meson setup build

# Compile
meson compile -C build

# Debug build (with symbols, no optimizations)
meson setup build_debug --buildtype=debug
meson compile -C build_debug

# AddressSanitizer build
meson setup build_asan -Db_sanitize=address
meson compile -C build_asan

# Clean
meson compile -C build --clean

# Install (requires root)
sudo meson install -C build
```

## Usage

### Basic benchmark
```bash
# Benchmark with 10 gate entries (default)
sudo ./build/src/gatebench --entries 10

# Benchmark with 100 entries, 1000 iterations
sudo ./build/src/gatebench --entries 100 --iters 1000

# Run selftests before benchmark
sudo ./build/src/gatebench --entries 50 --selftest

# Output JSON for machine processing
sudo ./build/src/gatebench --entries 20 --json > results.json
```

### Command-line options
```
Required options:
  -e, --entries=NUM       Number of gate entries (default: 10)

Benchmark options:
  -i, --iters=NUM         Iterations per run (default: 1000)
  -w, --warmup=NUM        Warmup iterations (default: 100)
  -r, --runs=NUM          Number of runs (default: 5)
  -I, --interval-ns=NS    Gate interval in nanoseconds (default: 1000000)
  -x, --index=NUM         Starting index for gate actions (default: 1000)

System options:
  -c, --cpu=NUM           CPU to pin to (-1 for no pinning, default: -1)
  -t, --timeout-ms=MS     Netlink timeout in milliseconds (default: 1000)

Gate shape options:
  --clockid=ID            Clock ID (3=CLOCK_TAI, default: 3)
  --base-time=NS          Base time for gate schedule (default: 0)
  --cycle-time=NS         Cycle time for gate schedule (default: 0)

Mode options:
  -s, --selftest          Run selftests before benchmark (default: off)
  -j, --json              Output JSON format (default: off)
  --sample-every=N        Sample every N iterations (default: 0 = no sampling)

Other options:
  -h, --help              Show this help message
  -v, --version           Show version information
```

### Sweep mode
The `tools/bench_sweep.sh` script runs benchmarks across a range of entry counts:

```bash
# Make the script executable
chmod +x tools/bench_sweep.sh

# Run sweep (requires root for netlink access)
sudo tools/bench_sweep.sh

# Customize sweep parameters
export ENTRIES_START=1
export ENTRIES_END=256
export CPU=0  # Pin to CPU 0
export ITERATIONS=5000
sudo -E tools/bench_sweep.sh
```

### Comparing results
Use `tools/bench_compare.py` to analyze multiple benchmark runs:

```bash
# Compare JSON results
python3 tools/bench_compare.py results/*.json

# Generate CSV and plots
python3 tools/bench_compare.py results/*.json --csv comparison.csv --plot
```

## Architecture

### Repository layout
```
gatebench/
├── README.md
├── meson.build
├── include/
│   ├── gatebench.h          # Core data structures
│   ├── gatebench_cli.h      # Command-line interface
│   ├── gatebench_nl.h       # Netlink I/O
│   ├── gatebench_gate.h     # Gate message building
│   ├── gatebench_stats.h    # Statistics calculations
│   ├── gatebench_util.h     # Utilities (CPU pinning, timing)
│   ├── gatebench_bench.h    # Benchmark runner
│   └── gatebench_selftest.h # Selftests
├── src/
│   ├── main.c              # Program entry point
│   ├── cli.c               # Command-line parsing
│   ├── bench.c             # Benchmark runner
│   ├── selftest.c          # Negative tests
│   ├── nl.c                # Netlink I/O implementation
│   ├── gate_msg.c          # Netlink message building
│   ├── stats.c             # Statistics implementation
│   └── util.c              # Utilities implementation
├── tools/
│   ├── bench_sweep.sh      # Entry count sweep script
│   └── bench_compare.py    # Result comparison tool
└── tests/
    └── expected.json       # Expected test results
```

### Core design principles

1. **Separate TX and RX buffers**: Prevents artificial coupling between request and response sizes
2. **Dynamic message templates**: All messages built into `malloc()` buffers with explicit capacity tracking
3. **Consistent error returns**: Always return `-errno` style errors
4. **Stateful selftests**: Each test uses fresh indices to avoid state bleed
5. **Reproducible measurements**: CPU pinning, warmup phases, multiple runs

### Measurement methodology
- Measures end-to-end control-plane update latency:
  - Userspace send
  - Kernel parse/validate/apply
  - Netlink ack
  - Userspace receive
- Uses `CLOCK_MONOTONIC_RAW` for timing
- Includes warmup phase to stabilize measurements
- Multiple runs with statistical aggregation

### Selftests
The selftest suite validates error paths and kernel behavior:
- Create missing entry list → `-EINVAL`
- Create empty entry list → `-EINVAL`
- Create zero interval → `-EINVAL`
- Create bad clockid → `-EINVAL`
- Replace without existing → `-ENOENT`
- Duplicate create → `-EEXIST`

Each test uses unique indices to avoid state contamination.

## Output formats

### Human-readable output
```
Configuration:
  Iterations per run: 1000
  Warmup iterations:  100
  Runs:               5
  Gate entries:       10
  Gate interval:      1000000 ns
  Starting index:     1000
  CPU pinning:        no
  Netlink timeout:    1000 ms
  Selftest:           no
  JSON output:        no
  Sampling:           no
  Clock ID:           3
  Base time:          0 ns
  Cycle time:         0 ns

Environment:
  Kernel: Linux 6.8.0 x86_64
  Current CPU: 3
  Clock source: CLOCK_MONOTONIC_RAW

Running benchmark...
Run 1/5... done (125000.5 ops/sec)
Run 2/5... done (126000.2 ops/sec)
Run 3/5... done (124500.8 ops/sec)
Run 4/5... done (125800.3 ops/sec)
Run 5/5... done (125200.7 ops/sec)

Summary:
  Median ops/sec: 125000.5
  Min ops/sec:    124500.8
  Max ops/sec:    126000.2
  Stddev ops/sec: 520.3
  
  Latency percentiles (median across runs):
    p50:   15.8 μs
    p95:   22.4 μs
    p99:   28.7 μs
    p999:  35.2 μs
```

### JSON output
```json
{
  "version": "0.1.0",
  "environment": {
    "sysname": "Linux",
    "release": "6.8.0",
    "machine": "x86_64"
  },
  "current_cpu": 3,
  "config": {
    "iters": 1000,
    "warmup": 100,
    "runs": 5,
    "entries": 10,
    "interval_ns": 1000000,
    "index": 1000,
    "cpu": -1,
    "timeout_ms": 1000,
    "selftest": false,
    "sample_mode": false,
    "sample_every": 0,
    "clockid": 3,
    "base_time": 0,
    "cycle_time": 0
  },
  "runs": [
    {
      "secs": 0.016,
      "ops_per_sec": 125000.5,
      "p50_ns": 15800,
      "p95_ns": 22400,
      "p99_ns": 28700,
      "p999_ns": 35200,
      "min_ns": 12000,
      "max_ns": 42000,
      "mean_ns": 16500.2,
      "stddev_ns": 3200.5,
      "create_len": 512,
      "replace_len": 512,
      "del_len": 128
    }
  ],
  "summary": {
    "median_ops_per_sec": 125000.5,
    "min_ops_per_sec": 124500.8,
    "max_ops_per_sec": 126000.2,
    "stddev_ops_per_sec": 520.3,
    "median_p50_ns": 15800,
    "median_p95_ns": 22400,
    "median_p99_ns": 28700,
    "median_p999_ns": 35200
  }
}
```

## Development

### Code style
- C99 with GNU extensions
- 4-space indentation
- Error handling: return `-errno` style
- All functions documented in headers
- No silent truncation or buffer overflows

### Testing
```bash
# Run selftests
sudo ./build/gatebench --selftest

# Build and run with debug symbols
meson setup build_debug --buildtype=debug
meson compile -C build_debug
sudo ./build_debug/gatebench --entries 1 --iters 10 --runs 2

# Build with AddressSanitizer
meson setup build_asan -Db_sanitize=address
meson compile -C build_asan
sudo ./build_asan/gatebench --entries 1 --iters 10
```

### Adding new features
1. Add configuration to `struct gb_config` in `include/gatebench.h`
2. Add CLI parsing in `src/cli.c`
3. Implement feature in appropriate module
4. Update JSON output in `src/main.c`
5. Add tests if applicable

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Acknowledgments

- Linux kernel networking team for the `tc gate` action
- `libmnl` authors for the netlink library
- Inspired by kernel selftests and benchmarking tools
