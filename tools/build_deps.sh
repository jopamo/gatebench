#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX_DIR="${ROOT_DIR}/deps/install"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

build_autotools() {
  local name="$1"
  shift
  local src="${ROOT_DIR}/${name}"
  local configure_args=("$@")

  if [[ ! -d "${src}" ]]; then
    echo "Missing ${name} source at ${src}" >&2
    exit 1
  fi

  pushd "${src}" >/dev/null

  if [[ -f "Makefile" ]]; then
    make distclean >/dev/null 2>&1 || make clean >/dev/null 2>&1 || true
  fi
  rm -f config.cache

  if [[ ! -x "./configure" ]]; then
    if [[ -x "./autogen.sh" ]]; then
      ./autogen.sh
    else
      autoreconf -fi
    fi
  fi

  ./configure --prefix="${PREFIX_DIR}" "${configure_args[@]}"
  make -j"${JOBS}"
  make install

  popd >/dev/null
}

mkdir -p "${PREFIX_DIR}"

build_autotools "libmnl" --disable-shared --enable-static
build_autotools "libpcap" --disable-shared --without-libnl --disable-dbus

PCAP_PC="${PREFIX_DIR}/lib/pkgconfig/libpcap.pc"
if [[ -f "${PCAP_PC}" ]]; then
  if rg -q "dbus-1|libnl" "${PCAP_PC}"; then
    echo "libpcap.pc still references dbus/libnl; rebuild failed to disable those deps." >&2
    echo "Contents:" >&2
    cat "${PCAP_PC}" >&2
    exit 1
  fi
fi

ENV_SH="${PREFIX_DIR}/env.sh"
cat <<EOF > "${ENV_SH}"
export PKG_CONFIG_PATH="${PREFIX_DIR}/lib/pkgconfig:\$PKG_CONFIG_PATH"
EOF

cat <<EOF
Local deps installed to: ${PREFIX_DIR}
Meson defaults to deps/install when present. To override:
  meson setup -Ddeps_prefix="${PREFIX_DIR}" build
  meson setup -Ddeps_prefix="" build   # use system libs
If you want pkg-config to prefer these deps in other builds:
  export PKG_CONFIG_PATH="${PREFIX_DIR}/lib/pkgconfig:\$PKG_CONFIG_PATH"
Or source: ${ENV_SH}
EOF
