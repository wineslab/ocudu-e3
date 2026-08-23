# Building OCUDU with jbpf (Janus integration)

This fork integrates [jbpf](https://github.com/microsoft/jbpf) (userspace eBPF)
into the gNB. When built with `ENABLE_JBPF=ON`, jbpf is compiled **in-tree**
from the `external/jbpf` submodule (statically linked — no prebuilt jbpf tree
is required) and the gNB exposes instrumentation hooks that codelets can
attach to:

| Hook | Location | Data |
|---|---|---|
| `capture_xran_packet` | `lib/ofh/receiver/ofh_message_receiver_impl.cpp` | per-eCPRI-packet U-plane IQ (BFP-compressed) |
| `capture_uplane_iq` | `lib/ofh/receiver/ofh_data_flow_uplane_uplink_data_impl.cpp` | per-U-plane-section IQ, post-decode (`cbf16_t`) |
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

`ENABLE_JBPF=ON` also needs **libe3's headers**, for the stage recorder on the
per-slot capture path (see [Stage recording](#6-stage-recording-on-the-per-slot-path)).
Headers only — nothing is linked — but the package has to be installed so
pkg-config can find it:

```bash
git clone https://github.com/wineslab/libe3.git
cd libe3
git checkout a995d57          # feat/latency-tracing
./build_libe3 --install
```

It is a hard requirement rather than an optional one because the jbpf capture
path exists to feed an E3 controller, which links libe3 regardless.

> **The branch is temporary.** The stage recorder is not on libe3 `main` yet, so
> the commit above is pinned to `feat/latency-tracing` — a commit rather than the
> branch head, because the branch is unmerged and gets rewritten, and the record
> layout and stage catalog have to match the ones the E3 controller was built
> against. It moves to `main` once libe3's latrec pull request merges.

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

## 5. Which hook is on the measured path

Three hooks are declared, but only one carries the path the E3 controller
measures:

- **`capture_uplink_slot`** — the measured one. Fires once per UL slot from the
  upper PHY with the assembled resource grid, and is the only hook whose context
  carries `gnb_ts_ns`. The `uplink_slot_samples` codelet attaches to it, feeding
  the controller's `SlotIqPipeline` and its L1-KPM service model (RAN function
  2). This is the path instrumented below.
- **`capture_xran_packet`** — the eCPRI-level hook, one fire per U-plane packet
  in the OFH receiver, with the payload still BFP-compressed. The
  `ecpri_iq_samples` codelet attaches to it and feeds the controller's spectrum
  service model (RAN function 1). A different capture point on a different
  service model, at packet rather than slot granularity, and **not** on the
  measured path. It carries no RAN-side timestamp.
- **`capture_uplane_iq`** — declared and fired, post-decode, but no codelet
  currently attaches to it.

## 6. Stage recording on the per-slot path

The capture path stamps its stage boundaries into
[libe3's](https://github.com/wineslab/libe3) `latrec`: a per-thread, lock-free,
mmap-backed ring where a stamp is one `clock_gettime(CLOCK_MONOTONIC)` and four
stores — no syscall, no allocation, no formatting, no lock, no I/O. The mapping
onto libe3's shared stage catalog lives in one header,
[`include/ocudu/janus/slot_trace.h`](include/ocudu/janus/slot_trace.h), which is
the whole of this repository's instrumentation.

Two identifiers are emitted, both in
`upper_phy_rx_symbol_handler_impl::handle_rx_symbol`, on the RU receive thread
that owns the slot:

| Box | Stamp | At |
|---|---|---|
| A1 entry | `RECORD_BEGIN` | the resource grid is complete, immediately after the existing `gnb_ts_ns` capture |
| A1 exit / A2 entry | `PROCESS_BEGIN` | `hook_capture_uplink_slot()` returned — the slot is in jbpf's output ring |

`aux2` on both carries `gnb_ts_ns` unchanged. That value is propagated verbatim
all the way to the controller, which stamps its own `RECORD_BEGIN` with it, so
the two processes' records join on an equality with nothing to negotiate. `aux`
on `RECORD_BEGIN` carries `sfn << 16 | slot_index` as a readable cross-check; it
repeats every 10.24 s, so it is not sufficient on its own.

**The codelet interior is bounded, not decomposed.** Everything between the two
stamps — jbpf dispatch, the codelet body, the copy into the output ring, the
submit — is one interval. A jbpf codelet cannot write into a latrec ring:
Prevail exposes exactly one verifier-tracked memory region per hook context, no
helper hands a codelet a mapping, `latrec.h` is POSIX C99 that does not compile
for the eBPF target, and the only live clock a codelet can read is
`CLOCK_REALTIME`. The header explains this at length. The interval's lower bound
comes from the codelet's own `codelet_ts_ns`, which the controller carries.

### Turning it on

There is no ocudu option and no environment variable for this. Whether the gNB
records is decided entirely by which libe3 its headers came from:

```bash
# records
cd libe3 && ./build_libe3 --install --cmake-opt "-DLIBE3_ENABLE_LATREC=ON"

# does not record - the stamps compile to nothing
cd libe3 && ./build_libe3 --install
```

`LIBE3_ENABLE_LATREC` is `PUBLIC` on libe3's own target and so appears in
`libe3.pc`'s `Cflags`; this repository just consumes it. Configure prints which
one it found:

```
-- Janus: libe3 0.1.1 was built with latrec; slot stages will be recorded
```

Rings are written to latrec's compiled-in default, `/tmp/latrec`, as
`ocudu.upper_phy.<tid>.latrec` — the same directory libe3 uses, so the gNB's
rings land beside the controller's with nothing to configure. Move both with
`-DLATREC_DEFAULT_DIR=` when building libe3. Capacity is 2^18 records (8 MiB per
thread, about 65 s at 30 kHz SCS) and is raised for a longer capture with
`LATREC_ENTRIES_LOG2_OCUDU_UPPER_PHY`; that is a sizing knob, not a gate.

Convert a capture with libe3's own tool — the gNB's records are filed under
`ocudu.csv` alongside the controller's:

```bash
python3 libe3/tools/latrec2csv.py /tmp/latrec -o ./csv
```

Records are only stamped while a codelet is attached. With no dApp subscribed
there is no codelet, the hook is a no-op and the slot goes nowhere, so stamping
it would be 2000 records/s of noise evicting what matters.

### What it costs

```bash
g++ -std=gnu++17 -O2 -Iinclude $(pkg-config --cflags libe3) \
    -o slot_trace_benchmark tests/benchmarks/janus/slot_trace_benchmark.cpp
./slot_trace_benchmark
```

[The benchmark](tests/benchmarks/janus/slot_trace_benchmark.cpp) includes the
same header the upper PHY does, so it measures the code that runs on the radio.
Because the recorder is a build-time decision, run it against both a traced and
an untraced libe3 and compare: the untraced build's delta is the machine's noise
floor, and the traced build's delta has to be read against it.

<!-- ## 7. Containerized reference build

A Dockerfile that reproduces this entire recipe end-to-end (both ocudu
variants + E3Controller + libe3, on Ubuntu 22.04 with DPDK 25.11) is
maintained in the workspace next to this repo at
`ocudu_project/docker/Dockerfile.ocudu-jbpf`. When in doubt, that file is the
executable version of this guide. -->
