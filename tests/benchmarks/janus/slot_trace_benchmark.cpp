// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief What recording a slot's stage boundaries costs, and whether that is
/// separable from the work of a slot.
///
/// The ablation the instrumentation has to answer: the same code with tracing
/// on and off, and the delta in per-slot processing time and in deadline misses.
/// Because LIBE3_ENABLE_LATREC is a build-time decision carried in from libe3,
/// "on" and "off" are two different binaries, so this is run twice and the two
/// results compared - it cannot switch itself.
///
/// It includes slot_trace.h, the same header the upper PHY includes, rather
/// than a copy of the calls: what is measured here is the code that runs on the
/// radio. It deliberately does *not* include anything jbpf, so it builds and
/// runs anywhere, which is what lets CI run the ablation without a gNB.
///
/// The arms are interleaved and rotated per batch. Run-to-run drift on a shared
/// runner is larger than the effect being measured, so measuring all of one arm
/// and then all of the other would mostly measure the drift.

#include "ocudu/janus/slot_trace.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// One 4-port, 273-PRB, 14-symbol slot of cbf16_t: 4 * 14 * 3276 * 4 bytes.
/// The number matters because the interval the stamps bracket is dominated by
/// the codelet's copy of exactly this much memory, so a stamp cost is only
/// meaningful next to it.
constexpr std::size_t slot_bytes = 4u * 14u * 3276u * 4u;

/// Slot deadline at 30 kHz SCS. Used only to count misses; nothing here is
/// scheduled against it.
constexpr std::uint64_t slot_deadline_ns = 500'000;

std::uint64_t now_ns()
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// Stand-in for the codelet's copy of the resource grid out of the gNB-owned
/// storage. Not a model of the codelet - it is bounds-checked chunk by chunk
/// under a verifier and this is not - but it is the same order of magnitude of
/// the same operation, which is all that is needed to say whether a stamp is
/// visible next to it.
void copy_slot(const std::uint8_t* src, std::uint8_t* dst)
{
  std::memcpy(dst, src, slot_bytes);
}

struct arm_result {
  std::uint64_t median_ns;
  std::uint64_t deadline_misses;
};

/// One batch of slots. \p traced decides whether the stamps are issued, which
/// on an untraced build is a distinction without a difference - that is the
/// point of running it there too.
arm_result run_arm(bool traced, unsigned slots, const std::uint8_t* src, std::uint8_t* dst)
{
  namespace trace = ocudu::janus::slot_trace;

  std::vector<std::uint64_t> samples;
  samples.reserve(slots);
  std::uint64_t misses = 0;

  for (unsigned i = 0; i != slots; ++i) {
    const std::uint16_t sfn        = static_cast<std::uint16_t>(i / 20u % 1024u);
    const std::uint16_t slot_index = static_cast<std::uint16_t>(i % 20u);
    const std::uint64_t gnb_ts_ns  = now_ns();

    const std::uint64_t t0  = now_ns();
    std::uint64_t       seq = 0;
    if (traced) {
      seq = trace::record_begin(sfn, slot_index, gnb_ts_ns);
    }
    copy_slot(src, dst);
    if (traced) {
      trace::process_begin(seq, static_cast<std::uint32_t>(slot_bytes), gnb_ts_ns);
      trace::heartbeat();
    }
    const std::uint64_t elapsed = now_ns() - t0;

    samples.push_back(elapsed);
    misses += (elapsed > slot_deadline_ns) ? 1 : 0;
  }

  std::sort(samples.begin(), samples.end());
  return {samples[samples.size() / 2], misses};
}

} // namespace

int main(int argc, char** argv)
{
  unsigned batches = 15;
  unsigned slots   = 400;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--batches" && i + 1 < argc) {
      batches = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--slots" && i + 1 < argc) {
      slots = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "usage: %s [--batches N] [--slots N]\n", argv[0]);
      return 2;
    }
  }

#ifdef LIBE3_ENABLE_LATREC
  const char* build = "traced";
#else
  const char* build = "untraced";
#endif

  std::vector<std::uint8_t> src(slot_bytes), dst(slot_bytes);
  for (std::size_t i = 0; i != slot_bytes; ++i) {
    src[i] = static_cast<std::uint8_t>(i);
  }

  // Off the measured path, exactly as on the radio: the open pre-faults its
  // whole mapping and would otherwise be attributed to the first slot.
  ocudu::janus::slot_trace::open_ring();

  // Warm the buffers so the first batch is not measuring page faults.
  run_arm(false, 64, src.data(), dst.data());
  run_arm(true, 64, src.data(), dst.data());

  std::vector<std::uint64_t> on, off;
  std::uint64_t              misses_on = 0, misses_off = 0;
  for (unsigned b = 0; b != batches; ++b) {
    // Rotate which arm goes first so a per-batch warm-up effect does not land
    // on the same arm every time.
    const bool on_first = (b % 2) == 0;
    for (int pass = 0; pass != 2; ++pass) {
      const bool traced = on_first ? (pass == 0) : (pass == 1);
      const auto r      = run_arm(traced, slots, src.data(), dst.data());
      if (traced) {
        on.push_back(r.median_ns);
        misses_on += r.deadline_misses;
      } else {
        off.push_back(r.median_ns);
        misses_off += r.deadline_misses;
      }
    }
  }

  std::sort(on.begin(), on.end());
  std::sort(off.begin(), off.end());
  const std::uint64_t med_on  = on[on.size() / 2];
  const std::uint64_t med_off = off[off.size() / 2];
  const double        delta   = static_cast<double>(med_on) - static_cast<double>(med_off);

  // A machine-readable line first, because CI parses it, then the prose.
  std::printf("RESULT build=%s slots=%u batches=%u per_slot_on_ns=%llu per_slot_off_ns=%llu "
              "delta_ns=%.1f misses_on=%llu misses_off=%llu\n",
              build,
              slots,
              batches,
              static_cast<unsigned long long>(med_on),
              static_cast<unsigned long long>(med_off),
              delta,
              static_cast<unsigned long long>(misses_on),
              static_cast<unsigned long long>(misses_off));

  std::printf("\n  build                 %s\n", build);
  std::printf("  per-slot, stamps on   %llu ns\n", static_cast<unsigned long long>(med_on));
  std::printf("  per-slot, stamps off  %llu ns\n", static_cast<unsigned long long>(med_off));
  std::printf("  delta                 %+.1f ns for 2 stamps + 1 heartbeat check\n", delta);
  std::printf("  deadline misses       %llu on / %llu off (deadline %llu ns)\n",
              static_cast<unsigned long long>(misses_on),
              static_cast<unsigned long long>(misses_off),
              static_cast<unsigned long long>(slot_deadline_ns));

#ifndef LIBE3_ENABLE_LATREC
  std::printf("\n  This build has the recorder compiled out, so both arms ran the same\n"
              "  code and the delta above is this machine's noise floor. Read the\n"
              "  traced build's delta against it: a delta inside this band is not a\n"
              "  measurement of the stamps, it is a bound on them.\n");
#endif

  if (delta < 0) {
    std::printf("\n  The traced arm measured faster than the untraced one. That is what\n"
                "  \"inside the resolution\" looks like; it is not a speed-up.\n");
  }
  return 0;
}
