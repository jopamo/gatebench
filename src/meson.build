# Source files for gatebench
gatebench_sources = files(
  'main.c',
  'cli.c',
  'bench.c',
  'selftest.c',
  'nl.c',
  'gate_msg.c',
  'stats.c',
  'util.c',
  'selftests/selftest_common.c',
  'selftests/test_create_missing_parms.c',
  'selftests/test_create_missing_entries.c',
  'selftests/test_create_empty_entries.c',
  'selftests/test_create_zero_interval.c',
  'selftests/test_create_bad_clockid.c',
  'selftests/test_replace_without_existing.c',
  'selftests/test_duplicate_create.c',
  'selftests/test_dump_correctness.c',
  'selftests/test_replace_persistence.c',
  'selftests/test_clockid_variants.c',
  'selftests/test_cycle_time_derivation.c',
  'selftests/test_cycle_time_ext_parsing.c',
  'selftests/test_replace_preserve_schedule.c',
  'selftests/test_base_time_update.c',
  'selftests/test_multiple_entries.c',
  'selftests/test_malformed_nesting.c',
  'selftests/test_bad_attribute_size.c',
)

# Headers (for dependency tracking)
gatebench_headers = files(
  '../include/gatebench.h',
  '../include/gatebench_cli.h',
  '../include/gatebench_nl.h',
  '../include/gatebench_gate.h',
  '../include/gatebench_stats.h',
  '../include/gatebench_util.h',
  '../include/gatebench_bench.h',
  '../include/gatebench_selftest.h',
)

# Build targets
gatebench_exe = executable(
  'gatebench',
  gatebench_sources,
  include_directories: incdir,
  dependencies: [libmnl_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)

if get_option('buildtype') == 'debug'
  summary('Debug build enabled via buildtype=debug', section: 'Build Targets')
endif

# Install headers (optional - for development)
if get_option('install_headers')
  install_headers(
    gatebench_headers,
    subdir: meson.project_name(),
  )
endif

# Generate pkg-config file (optional)
pkg = import('pkgconfig')
pkg.generate(
  name: meson.project_name(),
  description: 'Benchmark suite for Linux tc gate control-plane operations',
  version: meson.project_version(),
)
