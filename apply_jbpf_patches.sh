#!/usr/bin/env bash
#
# Apply the patches jbpf ships for its vendored third-party submodules
# (3p/ck, 3p/mimalloc, 3p/ebpf-verifier) so their headers compile from C++
# TUs. Required once after `git submodule update --init --recursive`, before
# configuring with -DENABLE_JBPF=ON.
#
# Equivalent to external/jbpf/init_and_patch_submodules.sh minus its rm +
# re-clone of the submodules (unnecessary after a fresh recursive init).
# Idempotent: already-patched trees are detected and skipped, so re-running
# is always safe.
set -euo pipefail

JBPF_DIR="$(cd "$(dirname "$0")" && pwd)/external/jbpf"
PATCH_DIR="${JBPF_DIR}/patches"

if [ ! -f "${JBPF_DIR}/CMakeLists.txt" ]; then
  echo "ERROR: jbpf submodule at ${JBPF_DIR} is not populated." >&2
  echo "       Run: git submodule update --init --recursive" >&2
  exit 1
fi

apply_patch() {
  local subdir="$1" patch="$2"
  local tree="${JBPF_DIR}/${subdir}"
  local patch_file="${PATCH_DIR}/${patch}"

  if [ ! -f "${tree}/.git" ] && [ ! -d "${tree}/.git" ]; then
    echo "ERROR: submodule ${subdir} is not populated." >&2
    echo "       Run: git submodule update --init --recursive" >&2
    exit 1
  fi

  if git -C "${tree}" apply --reverse --check "${patch_file}" 2>/dev/null; then
    echo "==> ${subdir}: ${patch} already applied, skipping"
  elif git -C "${tree}" apply --check "${patch_file}" 2>/dev/null; then
    echo "==> ${subdir}: applying ${patch}"
    git -C "${tree}" apply "${patch_file}"
  else
    echo "ERROR: ${patch} does not apply cleanly to ${subdir} (nor is it" >&2
    echo "       already applied). The tree may have local modifications;" >&2
    echo "       inspect it or re-init with:" >&2
    echo "       git submodule update --init --recursive --force" >&2
    exit 1
  fi
}

apply_patch 3p/ck            ck.patch
apply_patch 3p/mimalloc      mimalloc.patch
apply_patch 3p/ebpf-verifier ebpf_verifier.patch

echo "==> All jbpf third-party patches applied."
