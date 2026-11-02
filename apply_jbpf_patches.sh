#!/usr/bin/env bash
#
# Apply the patches jbpf ships for its vendored third-party submodules
# (3p/ck, 3p/mimalloc, 3p/ebpf-verifier) so their headers compile from C++
# TUs. Required once after `git submodule update --init --recursive`, before
# configuring with -DENABLE_JBPF=ON.
#
# Equivalent to external/jbpf/init_and_patch_submodules.sh minus its rm +
# re-clone of the submodules (unnecessary after a fresh recursive init), plus
# the core patches that script knows nothing about.
# Idempotent: already-patched trees are detected and skipped, so re-running
# is always safe.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
JBPF_DIR="${REPO_DIR}/external/jbpf"
PATCH_DIR="${JBPF_DIR}/patches"    # jbpf's own, for its 3p submodules
OURS_DIR="${REPO_DIR}/jbpf_patches" # ours, tracked by this repo

if [ ! -f "${JBPF_DIR}/CMakeLists.txt" ]; then
  echo "ERROR: jbpf submodule at ${JBPF_DIR} is not populated." >&2
  echo "       Run: git submodule update --init --recursive" >&2
  exit 1
fi

# run_patch <git|patch> <tree> <patch_file> <forward|reverse> <dry|real>
#
# One call site for both back-ends so the two never drift in what they
# consider "applies". Returns the tool's exit status; prints nothing.
run_patch() {
  local mode="$1" tree="$2" file="$3" dir="$4" kind="$5"
  local -a args=()
  if [ "${dir}" = reverse ]; then args+=(--reverse); fi

  # ${args[@]+...}: a forward+real call leaves args empty, and bash 3.2 (still
  # the /bin/bash on macOS) treats "${args[@]}" as unbound under `set -u`.
  if [ "${mode}" = git ]; then
    if [ "${kind}" = dry ]; then args+=(--check); fi
    git -C "${tree}" apply ${args[@]+"${args[@]}"} "${file}" >/dev/null 2>&1
  else
    if [ "${kind}" = dry ]; then args+=(--dry-run); fi
    ( cd "${tree}" && patch -p1 --silent --force --no-backup-if-mismatch \
        ${args[@]+"${args[@]}"} <"${file}" ) >/dev/null 2>&1
  fi
}

# apply_patch <subdir-under-jbpf> <patch-file-name> [patch-dir]
#
# `.` as the subdir means jbpf's own tree rather than one of its 3p
# submodules. The patch dir defaults to jbpf's; pass OURS_DIR for ours.
apply_patch() {
  local subdir="$1" patch="$2" from="${3:-${PATCH_DIR}}"
  local tree="${JBPF_DIR}/${subdir}"
  local patch_file="${from}/${patch}"
  local label="${subdir}"
  [ "${label}" = "." ] && label="jbpf core"

  if [ ! -d "${tree}" ]; then
    echo "ERROR: ${subdir} is not populated." >&2
    echo "       Run: git submodule update --init --recursive" >&2
    exit 1
  fi

  if [ ! -f "${patch_file}" ]; then
    echo "ERROR: ${patch} is missing from ${from}." >&2
    exit 1
  fi

  # git apply where the tree is a real checkout, patch(1) where it is not.
  # The fallback is not hypothetical: deployment rsyncs these trees into the
  # pod without .git, and there `git apply` fails at repository DISCOVERY --
  # indistinguishable, from the exit status alone, from a conflicting patch.
  local mode=git
  git -C "${tree}" rev-parse --git-dir >/dev/null 2>&1 || mode=patch

  if run_patch "${mode}" "${tree}" "${patch_file}" reverse dry; then
    echo "==> ${label}: ${patch} already applied, skipping"
  elif run_patch "${mode}" "${tree}" "${patch_file}" forward dry; then
    echo "==> ${label}: applying ${patch}"
    run_patch "${mode}" "${tree}" "${patch_file}" forward real
  else
    echo "ERROR: ${patch} does not apply cleanly to ${label} (nor is it" >&2
    echo "       already applied). The tree may have local modifications;" >&2
    echo "       inspect it or re-init with:" >&2
    echo "       git submodule update --init --recursive --force" >&2
    exit 1
  fi
}

apply_patch 3p/ck            ck.patch
apply_patch 3p/mimalloc      mimalloc.patch
apply_patch 3p/ebpf-verifier ebpf_verifier.patch

# jbpf core, not a 3p submodule: jbpf_time_get_ns() stamps CLOCK_REALTIME
# upstream, and that is the clock the E3 slot codelet writes into
# codelet_ts_ns. The gNB (upper_phy_rx_symbol_handler_impl.cpp), the
# E3Controller and the dApp all read CLOCK_MONOTONIC, so leaving this
# unpatched puts codelet_ts_ns in a different epoch from gnb_ts_ns:
# gnb_to_codelet_us then comes out silently wrong rather than failing.
apply_patch .                jbpf_monotonic_time.patch "${OURS_DIR}"

# jbpf core: jbpf_io_utils.c calls send() and recv() without <sys/socket.h>,
# which GCC 14 and later reject as implicit declarations.
apply_patch .                jbpf_io_socket_include.patch "${OURS_DIR}"

echo "==> All jbpf patches applied."
