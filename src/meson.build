# Source files for gatebench
gatebench_sources = files(
  'main.c',
  'cli.c',
  'bench.c',
  'selftest.c',
  'proof.c',
  'race.c',
  'nl.c',
  'gate_msg.c',
  'stats.c',
  'util.c',
  'selftests/selftest_common.c',
  'selftests/test_internal_schedule.c',
  'selftests/test_gate_timer_start_logic.c',
  'selftests/test_create_missing_parms.c',
  'selftests/test_create_missing_entries.c',
  'selftests/test_create_empty_entries.c',
  'selftests/test_create_zero_interval.c',
  'selftests/test_create_bad_clockid.c',
  'selftests/test_replace_without_existing.c',
  'selftests/test_duplicate_create.c',
  'selftests/test_dump_correctness.c',
  'selftests/test_replace_persistence.c',
  'selftests/test_replace_preserve_attrs.c',
  'selftests/test_clockid_variants.c',
  'selftests/test_cycle_time_derivation.c',
  'selftests/test_cycle_time_ext_parsing.c',
  'selftests/test_cycle_time_supplied.c',
  'selftests/test_replace_preserve_schedule.c',
  'selftests/test_replace_append_entries.c',
  'selftests/test_base_time_update.c',
  'selftests/test_multiple_entries.c',
  'selftests/test_entry_corner_cases.c',
  'selftests/test_malformed_nesting.c',
  'selftests/test_invalid_action_control.c',
  'selftests/test_invalid_entry_attrs.c',
  'selftests/test_bad_attribute_size.c',
  'selftests/test_param_validation.c',
  'selftests/test_replace_invalid.c',
  'selftests/test_large_dump.c',
  'selftests/test_entry_defaults.c',
  'selftests/test_priority_flags.c',
  'selftests/test_attr_matrix.c',
  'selftests/test_entry_index_cases.c',
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
  '../include/gatebench_proof.h',
  '../include/gatebench_race.h',
)

# Build targets
gatebench_link_args = []
if get_option('b_sanitize') == 'none'
  gatebench_link_args = ['-static']
endif

gatebench_exe = executable(
  'gatebench',
  gatebench_sources,
  include_directories: incdir,
  dependencies: gatebench_deps,
  link_args: gatebench_link_args,
  install: true,
  install_dir: get_option('bindir'),
)

if get_option('buildtype') == 'debug'
  summary({'Debug build enabled via buildtype=debug': true}, section: 'Build Targets')
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
