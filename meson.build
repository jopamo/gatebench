# gatebench meson.build
# Benchmark suite for Linux tc gate control-plane operations

project(
  'gatebench',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
    'buildtype=release',
  ],
  meson_version: '>=0.56.0',
)

# Compiler and flags
cc = meson.get_compiler('c')

# Global compiler arguments
add_project_arguments(
  '-D_GNU_SOURCE',
  language: 'c'
)

# Warning flags
warning_flags = [
  '-Wall',
  '-Wextra',
  '-Wpedantic',
  '-Wformat=2',
  '-Wformat-overflow=2',
  '-Wformat-truncation=2',
  '-Wformat-security',
  '-Wnull-dereference',
  '-Wstack-protector',
  '-Wstrict-overflow=5',
  '-Wtrampolines',
  '-Warray-bounds=2',
  '-Wimplicit-fallthrough=5',
  '-Wshift-overflow=2',
  '-Wcast-qual',
  '-Wstringop-overflow=4',
  '-Wconversion',
  '-Warith-conversion',
  '-Wlogical-op',
  '-Wduplicated-cond',
  '-Wduplicated-branches',
  '-Wrestrict',
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wunused-macros',
  '-Wbad-function-cast',
  '-Wcast-align=strict',
  '-Wjump-misses-init',
]

# Add warning flags that are supported by the compiler
foreach flag : warning_flags
  if cc.has_argument(flag)
    add_project_arguments(flag, language: 'c')
  endif
endforeach

# Dependencies
libmnl_dep = dependency('libmnl', required: true)
math_dep = cc.find_library('m', required: true)

# Include directories
incdir = include_directories('include')

# Build options
debug_opts = get_option('debug')
if debug_opts
  add_project_arguments('-DDEBUG', language: 'c')
endif

verbose_logs = get_option('verbose_logs')
if verbose_logs
  add_project_arguments('-DVERBOSE_LOGS', language: 'c')
endif

# Subdirectories
subdir('src')

# Handle sanitizer options
sanitizer = get_option('b_sanitize')
if sanitizer != 'none'
  summary({'Sanitizer enabled': sanitizer}, section: 'Build Configuration')
endif

# Installation
install_data(
  'README.md',
  install_dir: get_option('datadir') / 'doc' / meson.project_name()
)

# Install tools
tools_dir = get_option('datadir') / meson.project_name() / 'tools'
install_data(
  'tools/bench_sweep.sh',
  'tools/bench_compare.py',
  install_dir: tools_dir,
  install_mode: 'rwxr-xr-x'
)

# Summary
summary({
  'Build type': get_option('buildtype'),
  'Debug symbols': get_option('debug'),
  'Optimization': get_option('optimization'),
  'Warning level': get_option('warning_level'),
  'Werror': get_option('werror'),
  'Sanitizer': sanitizer,
  'Verbose logs': verbose_logs,
}, section: 'Build Configuration')

summary({
  'libmnl': libmnl_dep.found(),
  'math library': math_dep.found(),
}, section: 'Dependencies')

summary({
  'Install prefix': get_option('prefix'),
  'Binary dir': get_option('bindir'),
  'Data dir': get_option('datadir'),
  'Tools dir': tools_dir,
}, section: 'Install Paths')
