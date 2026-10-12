#!/usr/bin/env bash
set -euo pipefail

# Build Gufo natively on Fedora and install it under ~/install/opt/gufo.
#
# ROCm must already be installed. ROCm 10 keeps its tree in
# /opt/rocm/core-<version>; ROCm 7 used /opt/rocm directly. build.sh finds
# either layout. Required ROCm components: HIP, hipBLAS, hipBLASLt, rocBLAS,
# hipCUB, rocPRIM, and rocWMMA.

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="${HOME}/install/opt/gufo"
JOBS="${BUILD_JOBS:-$(nproc)}"

# Packages Gufo needs to compile. Installed names are skipped, so a re-run
# does not break the dnf transaction.
HOST_PACKAGES=(
  gcc
  gcc-c++
  make
  cmake
  ninja-build
  pkgconf-pkg-config
  libicu-devel
  libcurl-devel
  openssl-devel
  libpng-devel
  libjpeg-turbo-devel
  libwebp-devel
)

# Gufo runs the ffmpeg and ffprobe binaries; it links no FFmpeg library.
# Fedora ships both in ffmpeg-free. Never ask dnf for plain "ffmpeg": that
# name resolves to the RPM Fusion build, which conflicts with an installed
# ffmpeg-free and blocks the whole transaction.
if ! command -v ffmpeg >/dev/null || ! command -v ffprobe >/dev/null; then
  HOST_PACKAGES+=(ffmpeg-free)
fi

MISSING_PACKAGES=()
for package in "${HOST_PACKAGES[@]}"; do
  rpm -q "${package}" >/dev/null 2>&1 || MISSING_PACKAGES+=("${package}")
done
if ((${#MISSING_PACKAGES[@]} > 0)); then
  sudo dnf install "${MISSING_PACKAGES[@]}"
fi

# ROCm 10: /opt/rocm/core-<version>. ROCm 7: /opt/rocm.
find_rocm_prefix() {
  if [[ -n "${ROCM_PATH:-}" ]]; then
    printf '%s\n' "${ROCM_PATH}"
    return 0
  fi
  local candidates=() candidate picked=""
  while IFS= read -r candidate; do
    [[ -n "${candidate}" ]] && candidates+=("${candidate}")
  done < <(find /opt/rocm/ -maxdepth 1 -type d -name 'core-*' 2>/dev/null | sort -V)

  # Prefer a tree that carries the hipBLASLt dev files. A runtime-only tree,
  # for example a second ROCm series installed beside this one, configures but
  # then fails to link.
  for candidate in "${candidates[@]:-}"; do
    [[ -d "${candidate}/lib/cmake/hipblaslt" ]] && picked="${candidate}"
  done
  if [[ -z "${picked}" ]] && ((${#candidates[@]} > 0)); then
    picked="${candidates[$((${#candidates[@]} - 1))]}"
  fi
  if [[ -n "${picked}" ]]; then
    printf '%s\n' "${picked}"
    return 0
  fi
  printf '/opt/rocm\n'
}

# ROCm 10 puts the compiler in lib/llvm, ROCm 7 in llvm.
find_hip_compiler() {
  local root="$1" candidate
  for candidate in \
    "${root}/lib/llvm/bin/amdclang++" \
    "${root}/llvm/bin/amdclang++" \
    "${root}/llvm/bin/clang++"; do
    if [[ -x "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

ROCM_PREFIX="$(find_rocm_prefix)"
if [[ ! -d "${ROCM_PREFIX}" ]]; then
  echo "ROCm not found at ${ROCM_PREFIX}. Install ROCm first or set ROCM_PATH." >&2
  exit 1
fi
if ! HIP_COMPILER="$(find_hip_compiler "${ROCM_PREFIX}")"; then
  echo "No HIP compiler under ${ROCM_PREFIX}. Install the ROCm LLVM component." >&2
  exit 1
fi
echo "Using ROCm at ${ROCM_PREFIX}"
echo "Using HIP compiler ${HIP_COMPILER}"

cmake --preset release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DCMAKE_PREFIX_PATH="${ROCM_PREFIX}" \
  -DCMAKE_HIP_COMPILER="${HIP_COMPILER}"
cmake --build --preset release --parallel "${JOBS}"

"${ROOT_DIR}/build/release/gufo" diagnose
cmake --install "${ROOT_DIR}/build/release"

echo "Gufo installed to ${INSTALL_PREFIX}/bin/gufo"
