# Building OCUDU with jbpf (Janus integration)

This fork integrates [jbpf](https://github.com/microsoft/jbpf) (userspace eBPF)
into the gNB. When built with `ENABLE_JBPF=ON`, jbpf is compiled **in-tree**
from the `external/jbpf` submodule (statically linked — no prebuilt jbpf tree
is required) and the gNB exposes instrumentation hooks that codelets can
attach to:

| Hook | Location | Data |
|---|---|---|
| `capture_xran_packet` | `lib/ofh/receiver/ofh_message_receiver_impl.cpp` | per-eCPRI-packet U-plane IQ (BFP-compressed) |
| `capture_uplink_slot` | `lib/phy/upper/upper_phy_rx_symbol_handler_impl.cpp` | per-UL-slot resource-grid IQ (`cbf16_t`) |

The gNB runs as jbpf **IPC secondary**; codelet output is consumed by an IPC
primary such as the [E3Controller](https://github.com/wineslab/E3Controller),
which forwards it to dApps over the E3 protocol.

## 1. Dependencies

Everything the stock ocudu build needs (see
[docker/scripts/install_dependencies.sh](docker/scripts/install_dependencies.sh)),
plus jbpf's own build dependencies. On Ubuntu 22.04:

```bash
sudo apt-get install -y --no-install-recommends \
    ca-certificates git patch \
    cmake make build-essential pkg-config \
    libfftw3-dev libmbedtls-dev libsctp-dev libyaml-cpp-dev libgtest-dev \
    libzmq3-dev \
    libboost-dev libboost-program-options-dev libboost-filesystem-dev \
    libasan6 python3 python3-pip clang gcc-multilib \
    libelf-dev libpcap-dev doxygen gcovr
```

DPDK (for the OFH/eCPRI fronthaul path) and ZeroMQ are optional and follow
the standard ocudu instructions.

## 2. Sources and submodules

```bash
git clone --branch e3 https://github.com/wineslab/ocudu-e3.git ocudu-jbpf
cd ocudu-jbpf
git submodule update --init --recursive
```

The `--recursive` matters: jbpf vendors its own third-party submodules
(`3p/ck`, `3p/ubpf`, `3p/ebpf-verifier`, `3p/mimalloc`, and their nested
submodules). The repo-root `./init_modules.sh` is **not** recursive, so it is
not sufficient on its own.

### Patch jbpf's third-party submodules

jbpf ships patches that make its C submodules usable from C++ TUs. They must
be applied once after the submodule init:

```bash
./apply_jbpf_patches.sh
```

The script ([apply_jbpf_patches.sh](apply_jbpf_patches.sh)) applies
`ck.patch`, `mimalloc.patch`, and `ebpf_verifier.patch` to the corresponding
`external/jbpf/3p/*` trees. It is idempotent (already-patched trees are
skipped) and is equivalent to `external/jbpf/init_and_patch_submodules.sh`
minus its `rm` + re-clone step (unnecessary after a fresh `--recursive`
init). Note that jbpf's usual configure-time auto-init is deliberately
disabled by our top-level CMake (`INITIALIZE_SUBMODULES=OFF`), so this step
is required.

## 3. Build

```bash
mkdir build && cd build
cmake -DENABLE_JBPF=ON -DENABLE_ZEROMQ=ON -DENABLE_UHD=OFF \
      -DENABLE_DPDK=True -DASSERT_LEVEL=MINIMAL \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF ..
make -j"$(nproc)" gnb
```

The binary lands at `build/apps/gnb/gnb`. Adjust `ENABLE_ZEROMQ` /
`ENABLE_UHD` / `ENABLE_DPDK` to your radio front-end as usual; `ENABLE_JBPF`
is orthogonal to them.

What `ENABLE_JBPF=ON` does (see the "Janus (jbpf) integration" block in
[CMakeLists.txt](CMakeLists.txt)): builds `external/jbpf` via
`add_subdirectory` with `JBPF_STATIC=ON` and
`JBPF_EXPERIMENTAL_FEATURES=ON` forced, jbpf's tests silenced, and the
hooks above compiled into the OFH receiver and upper PHY.

## 4. Running with the E3Controller

The jbpf IPC **primary must be running before the gNB starts**:

1. Start the [E3Controller](https://github.com/wineslab/E3Controller)
   (default IPC name `e3_controller`, run path `/dev/shm`).
2. Start the gNB with jbpf enabled in its YAML config.
3. Codelets are loaded on demand over LCM IPC (default socket
   `/tmp/jbpf/jbpf_lcm_ipc`) when a dApp subscribes — the gNB only has to be
   up by the time the first subscription arrives.

## 5. Containerized reference build

A Dockerfile that reproduces this entire recipe end-to-end (both ocudu
variants + E3Controller + libe3, on Ubuntu 22.04 with DPDK 25.11) is
maintained in the workspace next to this repo at
`ocudu_project/docker/Dockerfile.ocudu-jbpf`. When in doubt, that file is the
executable version of this guide.
