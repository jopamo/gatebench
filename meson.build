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
    'default_library=static',
    'prefer_static=true',
  ],
  meson_version: '>=0.57.0',
)

# Compiler and flags
cc = meson.get_compiler('c')
fs = import('fs')
sanitizer = get_option('b_sanitize')
static_link_requested = sanitizer == 'none'

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
deps_prefix = get_option('deps_prefix')
deps_prefix_abs = deps_prefix
if deps_prefix != '' and not deps_prefix.startswith('/')
  deps_prefix_abs = join_paths(meson.project_source_root(), deps_prefix)
endif
deps_use_local = false
deps_inc_args = []
deps_incdir = ''
deps_libdir = ''
libmnl_local_used = false
libpcap_local_used = false

if deps_prefix_abs != ''
  if fs.is_dir(deps_prefix_abs)
    deps_incdir = join_paths(deps_prefix_abs, 'include')
    deps_libdir = join_paths(deps_prefix_abs, 'lib')
    if fs.is_dir(deps_incdir) and fs.is_dir(deps_libdir)
      deps_use_local = true
      deps_inc_args = ['-I' + deps_incdir]
    endif
  endif
endif

libmnl_dep = disabler()
if deps_use_local
  libmnl_local = cc.find_library('mnl', dirs: [deps_libdir], required: false)
  if libmnl_local.found()
    libmnl_dep = declare_dependency(
      compile_args: deps_inc_args,
      dependencies: libmnl_local,
    )
    libmnl_local_used = true
  endif
endif
if not libmnl_dep.found()
  libmnl_dep = dependency('libmnl', required: true, static: true)
endif

math_dep = cc.find_library('m', required: true)
threads_dep = dependency('threads')

pcap_opt = get_option('pcap')
libpcap_dep = disabler()
if not pcap_opt.disabled()
  if deps_use_local
    libpcap_local = cc.find_library('pcap', dirs: [deps_libdir], required: false)
    if libpcap_local.found()
      libpcap_dep = declare_dependency(
        compile_args: deps_inc_args,
        dependencies: libpcap_local,
      )
      libpcap_local_used = true
    endif
  endif
  if not libpcap_dep.found()
    libpcap_dep = dependency('libpcap', required: pcap_opt.enabled(), static: true)
  endif
endif

if static_link_requested and pcap_opt.auto() and libpcap_dep.found()
  if not cc.links(
    'int main(void) { return 0; }',
    dependencies: [math_dep, threads_dep, libpcap_dep],
    args: ['-static'],
    name: 'static libpcap dependency probe'
  )
    warning('Static libpcap dependencies are unavailable in auto mode; disabling pcap support. Run tools/build_deps.sh to install local static deps.')
    libpcap_dep = disabler()
    libpcap_local_used = false
  endif
endif

gatebench_deps = [libmnl_dep, math_dep, threads_dep]
if libpcap_dep.found()
  gatebench_deps += [libpcap_dep]
  add_project_arguments('-DGATEBENCH_HAVE_PCAP', language: 'c')
endif

if static_link_requested
  if not cc.links(
    'int main(void) { return 0; }',
    dependencies: gatebench_deps,
    args: ['-static'],
    name: 'gatebench static link probe'
  )
    if deps_use_local
      error('Failed to resolve a fully static link with deps from ' + deps_prefix_abs + '. Rebuild local deps with tools/build_deps.sh.')
    else
      error('Failed to resolve a fully static link with system dependencies. Run tools/build_deps.sh (defaults to deps/install) or pass -Ddeps_prefix to a prefix with static libmnl/libpcap.')
    endif
  endif
endif

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
  'libmnl (local)': libmnl_local_used,
  'libpcap': libpcap_dep.found(),
  'libpcap (local)': libpcap_local_used,
  'deps prefix': deps_use_local ? deps_prefix_abs : 'disabled',
  'math library': math_dep.found(),
}, section: 'Dependencies')

summary({
  'Install prefix': get_option('prefix'),
  'Binary dir': get_option('bindir'),
  'Data dir': get_option('datadir'),
  'Tools dir': tools_dir,
}, section: 'Install Paths')
