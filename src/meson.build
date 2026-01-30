# Source files for gatebench
sources = files(
  'main.c',
  'cli.c',
  'bench.c',
  'selftest.c',
  'nl.c',
  'gate_msg.c',
  'stats.c',
  'util.c',
)

# Headers (for dependency tracking)
headers = files(
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
  sources,
  include_directories: incdir,
  dependencies: [libmnl_dep, math_dep],
  install: true,
  install_dir: get_option('bindir'),
)

# Custom targets for debug and asan builds
if get_option('buildtype') == 'debug'
  # Debug build already configured via buildtype
  summary('Debug build enabled via buildtype=debug', section: 'Build Targets')
endif

# Test targets (if we had tests)
# test('selftest', gatebench_exe, args: ['--selftest'])

# Install headers (optional - for development)
if get_option('install_headers')
  install_headers(
    headers,
    subdir: meson.project_name(),
  )
endif

# Generate pkg-config file (optional)
pkg = import('pkgconfig')
pkg.generate(
  name: meson.project_name(),
  description: 'Benchmark suite for Linux tc gate control-plane operations',
  version: meson.project_version(),
  # filebase: meson.project_name(),
  # subdirs: [meson.project_name()],
  # libraries: gatebench_exe, # Not a library, so skip
)