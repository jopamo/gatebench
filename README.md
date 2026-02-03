<div style="background-color:#1e1e1e; padding:1em; display:inline-block; border-radius:8px; text-align:center;">
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

# Clean
meson compile -C build --clean

# Install
sudo meson install -C build
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

## License

GPL-2

## Acknowledgments

- Linux kernel networking team for the `tc gate` action
- `libmnl` authors for the netlink library
- `iproute2` maintainers for `tc` reference behavior
- Inspired by kernel selftests and benchmarking tools
