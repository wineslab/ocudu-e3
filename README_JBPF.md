# Building OCUDU with jbpf (Janus integration)

This fork integrates [jbpf](https://github.com/microsoft/jbpf) (userspace eBPF)
into the gNB. When built with `ENABLE_JBPF=ON`, jbpf is compiled **in-tree**
from the `external/jbpf` submodule (statically linked — no prebuilt jbpf tree
is required) and the gNB exposes instrumentation hooks that codelets can
attach to:

| Hook | Location | Data |
|---|---|---|
| `capture_xran_packet` | `lib/ofh/receiver/ofh_message_receiver_impl.cpp` | per-eCPRI-packet U-plane IQ (BFP-compressed) |
| `capture_uplane_iq` | `lib/ofh/receiver/ofh_data_flow_uplane_uplink_data_impl.cpp` | per-U-plane-section IQ, already decompressed (`cbf16_t`), with slot/symbol/eAxC metadata |
| `capture_uplink_slot` | `lib/phy/upper/upper_phy_rx_symbol_handler_impl.cpp` | per-UL-slot resource-grid IQ (`cbf16_t`) |

The gNB runs as jbpf **IPC secondary**; codelet output is consumed by an IPC
primary such as the [E3Controller](https://github.com/wineslab/E3Controller),
which forwards it to dApps over the E3 protocol. `ENABLE_JBPF=ON` also builds
the gNB-side E3 publish helper (`ocudu_janus/e3/`): the E3Controller's slot
codelet calls it to write each uplink slot straight into the shared memory the
dApp reads.

## 1. Dependencies

Everything the stock ocudu build needs (see
[docker/scripts/install_dependencies.sh](docker/scripts/install_dependencies.sh)),
plus jbpf's own build dependencies. On Ubuntu 22.04 (x86_64):

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

- On arm64, install `libc6-dev` instead of `gcc-multilib`, as jbpf's own image
  does.
- DPDK (for the OFH/eCPRI fronthaul path) and ZeroMQ are optional and follow
  the standard ocudu instructions. The build command in section 3 enables
  both; drop the ones you do not use.
- CPU: the E3 publish helper needs an x86_64 CPU with AVX2 and F16C (Haswell,
  2013, or newer) or an aarch64 CPU (NEON). On any other architecture configure
  stops: the scalar fallback is about 47x slower, and this code runs in the PHY
  RX thread.

## 2. Sources and submodules

```bash
git clone --branch e3 https://github.com/wineslab/ocudu-e3.git
cd ocudu-e3
git submodule update --init --recursive
```

Keep the folder name `ocudu-e3` and clone it next to your E3Controller
checkout: the E3Controller build looks for `../ocudu-e3` by default
(`OCUDU_DIR`), and this build looks for `../E3Controller` (see section 3).

The `--recursive` matters: jbpf vendors its own third-party submodules
(`3p/ck`, `3p/ubpf`, `3p/ebpf-verifier`, `3p/mimalloc`, and their nested
submodules). The repo-root `./init_modules.sh` is **not** recursive, so it is
not sufficient on its own.

### Patch jbpf

Run once after the submodule init:

```bash
./apply_jbpf_patches.sh
```

The script ([apply_jbpf_patches.sh](apply_jbpf_patches.sh)) applies:

- jbpf's own patches for its C submodules, so their headers compile from C++
  TUs: `ck.patch`, `mimalloc.patch` and `ebpf_verifier.patch`, to the
  `external/jbpf/3p/*` trees;
- this repository's `jbpf_patches/jbpf_monotonic_time.patch`, to jbpf itself.
  With it, `jbpf_time_get_ns()` reads `CLOCK_MONOTONIC`, the clock the gNB
  hook, the E3Controller and the dApp use. Without it the codelet's timestamps
  are in another epoch, and latency numbers come out silently wrong;
- this repository's `jbpf_patches/jbpf_io_socket_include.patch`, to jbpf
  itself, so jbpf builds with GCC 14 and later.

It is idempotent (already-patched trees are skipped), and it uses `git apply`,
or `patch(1)` on trees copied without `.git`. It replaces
`external/jbpf/init_and_patch_submodules.sh`, minus that script's `rm` +
re-clone step (unnecessary after a fresh `--recursive` init) and plus the
two patches of this repository. jbpf's usual configure-time auto-init is
deliberately disabled by our top-level CMake (`INITIALIZE_SUBMODULES=OFF`), so
this step is required. The E3Controller's `build.sh` runs the same script over
this tree, which does nothing once it has run.

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
`JBPF_EXPERIMENTAL_FEATURES=ON` forced, jbpf's tests silenced, the hooks
above compiled into the OFH receiver and upper PHY, and the E3 publish helper
in `ocudu_janus/e3/`.

**E3 header check.** The E3 publish helper shares three headers with the
E3Controller: `jbpf_e3_ids.h`, `jbpf_e3_slot_api.h` and `e3_slot_convert.h`.
Configure compares them byte for byte with the E3Controller checkout at
`../E3Controller` (pass `-DE3CONTROLLER_DIR=/path/to/E3Controller` for another
place) and stops if they differ: build both sides from matching versions.
Without an E3Controller checkout the check is skipped.

The E3Controller builds against this tree's jbpf, and jbpf builds part of
itself inside its source tree, so do not build ocudu and the E3Controller at
the same time. Run one after the other.

## 4. Running with the E3Controller

1. Start the [E3Controller](https://github.com/wineslab/E3Controller) first:
   the jbpf IPC **primary must be running before the gNB starts**. Its
   defaults: IPC name `e3_controller`, run path `/dev/shm`, 1 GiB of IPC
   memory.
2. Start the gNB with a `jbpf:` section that matches the controller. The gNB's
   own defaults do not (standalone mode, IPC name `ocudu_ipc_app`, run path
   `/tmp`), so set them:

   ```yaml
   jbpf:
     jbpf_enable_ipc: 1                  # IPC secondary (default 0 = standalone)
     jbpf_ipc_mem_name: "e3_controller"  # = the controller's jbpf.ipc_name
     jbpf_run_path: "/dev/shm"           # = the controller's jbpf.run_path
     jbpf_namespace: "jbpf"
     jbpf_io_mem_size_mb: 1024           # = the controller's jbpf.mem_size_bytes (1 GiB)
     jbpf_enable_lcm_ipc: 1
     jbpf_lcm_ipc_name: "jbpf_lcm_ipc"
   ```

3. Codelets are loaded on demand over LCM IPC when a dApp subscribes, so the
   gNB only has to be up by the time the first subscription arrives. The gNB
   serves LCM at `<jbpf_run_path>/<jbpf_namespace>/<jbpf_lcm_ipc_name>`, here
   `/dev/shm/jbpf/jbpf_lcm_ipc`. The E3Controller derives the same path from
   its run path unless `jbpf.lcm_socket_path` is set in its config.

<!-- ## 5. Containerized reference build

A Dockerfile that reproduces this entire recipe end-to-end (both ocudu
variants + E3Controller + libe3, on Ubuntu 22.04 with DPDK 25.11) is
maintained in the workspace next to this repo at
`ocudu_project/docker/Dockerfile.ocudu-jbpf`. When in doubt, that file is the
executable version of this guide. -->
