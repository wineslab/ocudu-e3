// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Stage stamps for the per-slot IQ capture path.
///
/// This header owns exactly one thing: how the gNB's per-slot capture path maps
/// onto libe3's shared stage catalog (\c libe3/latrec.h). It is the whole of
/// this repository's instrumentation - upper_phy_rx_symbol_handler_impl calls
/// these and nothing else.
///
/// The catalog names *operations*, not components: the same identifier is used
/// wherever that operation is performed, and which component performed it is
/// read off the ring that recorded it. There is no per-repository stage block,
/// and inventing one would break every offline tool.
///
/// Boxes covered (libe3 docs/path-a-e3-loop.md numbering), both on the forward
/// leg, and both stamped by the RU receive thread that owns the slot:
///
///   A1  E3 data recording   RECORD_BEGIN -> PROCESS_BEGIN
///   A2  Processing          PROCESS_BEGIN -> (continues in the consumer)
///
/// A1 here is the gNB's own share of it: the resource grid is complete, and the
/// slot has to reach shared memory before anything else can look at it. Its
/// exit, PROCESS_BEGIN, is stamped when the jbpf hook returns - by which point
/// the codelet has copied the slot into jbpf's output ring and submitted it, so
/// SM-side processing can begin. A2 continues in the consumer process and is
/// stamped there.
///
/// This is the deployment libe3's Path A doc calls a *split capture pipeline*:
/// "A1 to A3 span two processes joined through shared memory". Before this
/// header, that doc's "not yet instrumented" list said such pipelines "have
/// identifiers available (RECORD_BEGIN, PROCESS_BEGIN) but no stamps in their
/// own code yet". These are those stamps for the producer half.
///
/// \section slot_trace_codelet What is deliberately not stamped
///
/// Everything between RECORD_BEGIN and PROCESS_BEGIN - the jbpf dispatch, the
/// codelet body, the copy into the output ring, the submit - is one interval,
/// not a decomposition. A jbpf codelet cannot write into a latrec ring:
///
///  - Prevail exposes exactly one verifier-tracked memory region per hook
///    context (jbpf's own docs: "The verifier does not allow us to expose
///    multiple memory regions as it cannot verify more than one"). A ring is a
///    second, foreign mapping, and there is no way to present it as a verified
///    pointer.
///  - No helper hands a codelet a mapping. The whole surface (jbpf_helper.h) is
///    maps, time, hash, printf, the output buffer, control input, the runtime
///    limit and rand.
///  - latrec.h does not compile for the eBPF target: it is POSIX C99 built on
///    mmap/open/ftruncate/clock_gettime, release atomics and sched_getcpu.
///  - Even setting all of that aside, the only live clock a codelet can read is
///    jbpf_time_get_ns() (CLOCK_REALTIME). jbpf_get_sys_time() is not a live
///    read - it returns the hook framework's own per-invocation
///    CLOCK_MONOTONIC_RAW timestamps, a different clock again from latrec's
///    CLOCK_MONOTONIC.
///
/// So the codelet interior is an *inferred* interval bounded on either side:
/// above by RECORD_BEGIN -> PROCESS_BEGIN here, and below by the codelet's own
/// codelet_ts_ns, which it writes into its output struct in CLOCK_REALTIME and
/// the consumer carries on PROCESS_BEGIN.aux. Two clocks, so the inner bound
/// costs a conversion; the outer bound does not.
///
/// \section slot_trace_gate The gate
///
/// Everything here compiles to nothing when libe3 was built without
/// -DLIBE3_ENABLE_LATREC. That flag is libe3's, it is PUBLIC on the libe3
/// target and is therefore carried in libe3.pc's Cflags, and this repository
/// adds no second switch: whether the gNB records is decided entirely by which
/// libe3 its headers came from. There is no environment variable and no ocudu
/// build option.

#ifndef OCUDU_JANUS_SLOT_TRACE_H
#define OCUDU_JANUS_SLOT_TRACE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <libe3/latrec.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ocudu {
namespace janus {
namespace slot_trace {

/// Ring role for the thread that runs the slot capture path.
///
/// The role is the component's identity: libe3's tools/latrec2csv.py maps its
/// first segment through RING_COMPONENTS to decide which per-component CSV the
/// records land in, and \c ocudu is the prefix that files them under ocudu.csv,
/// alongside the E3 controller's own \c e3controller.* rings for the same
/// deployment. A role that table does not recognise lands in other.csv.
inline constexpr const char* ring_role = "ocudu.upper_phy";

#ifdef LIBE3_ENABLE_LATREC

/// Default ring capacity, 2^18 records = 8 MiB per thread. Two records per
/// published slot, so at 30 kHz SCS (2000 slots/s) this holds about 65 s of
/// continuous capture before it wraps. Raise it with
/// LATREC_ENTRIES_LOG2_OCUDU_UPPER_PHY for a longer one.
inline constexpr unsigned default_entries_log2 = 18;

namespace detail {

/// One ring per writer thread, which is what latrec's single-producer design
/// requires: several RU receive threads can run slot capture concurrently, one
/// per sector. Zero-initialised means enabled == 0, so every stamp below is a
/// single predicted-not-taken branch until open_ring() succeeds - there is no
/// null ring to guard against.
inline thread_local latrec_t ring{};

/// Source-side record counter, per thread, per the catalog's "seq = the
/// source's own record counter". It keys this ring's rows only; the join to the
/// consumer's rows is on gnb_ts_ns, not on seq. See the note on record_begin().
inline thread_local std::uint64_t seq{0};

/// Same precedence latrec.c's own TLS layer uses: LATREC_ENTRIES_LOG2_<ROLE>
/// (role uppercased, non-alphanumerics as '_'), else LATREC_ENTRIES_LOG2, else
/// the compiled-in default. Sizing only - none of these can switch recording
/// off, which is the build's decision alone.
inline unsigned entries_log2()
{
  for (const char* var : {"LATREC_ENTRIES_LOG2_OCUDU_UPPER_PHY", "LATREC_ENTRIES_LOG2"}) {
    if (const char* v = std::getenv(var)) {
      const long n = std::strtol(v, nullptr, 10);
      if (n >= 10 && n <= 30) {
        return static_cast<unsigned>(n);
      }
    }
  }
  return default_entries_log2;
}

} // namespace detail

/// Open the calling thread's ring, once. Idempotent, first call wins.
///
/// This is the only part of the mechanism that is not free, and it must not run
/// on a live slot: latrec_open_in() creates and pre-faults the whole mapping,
/// which is tens of milliseconds - orders of magnitude past a slot deadline.
///
/// It is called on the thread's first complete UL slot, which is at cell
/// bring-up: symbols flow as soon as the RU is up, long before a dApp
/// subscribes and therefore long before a codelet is attached to the hook. That
/// is why the call sits *ahead* of the codelet-attached check at the call site
/// rather than inside it - inside, the first open would land on a slot of a
/// live cell with UEs on it.
///
/// The ring goes to latrec's own compiled-in default directory, the same one
/// libe3 uses, so the gNB's rings and the consumer's land side by side with
/// nothing to configure. Point both elsewhere with -DLATREC_DEFAULT_DIR= at
/// configure time; this repository adds no runtime option for it.
inline void open_ring()
{
  if (detail::ring.enabled) {
    return;
  }
  /* <role>.<tid>, latrec's own naming. The thread id, not the process id: the
   * file is opened O_TRUNC, and a gNB can run several RU receive threads that
   * each capture slots, so a per-process name would have them silently
   * truncating each other's rings. */
  char name[64];
  std::snprintf(name, sizeof(name), "%s.%ld", ring_role, ::syscall(SYS_gettid));
  if (latrec_open_in(&detail::ring, LATREC_DEFAULT_DIR, name, detail::entries_log2()) != 1) {
    std::fprintf(stderr,
                 "[slot_trace] could not open a latrec ring in %s; slot stages will not be recorded\n",
                 LATREC_DEFAULT_DIR);
  }
}

/// A1 entry: the resource grid for this slot is complete and about to be handed
/// to the codelet. Stamped immediately after the hook's own gnb_ts_ns capture,
/// so the two describe the same instant on two clocks.
///
/// \param sfn         system frame number.
/// \param slot_index  slot within the frame - slot_point::slot_index(), the
///                    same value the hook passes as slot_id and the consumer
///                    reads back as its abs_slot.
/// \param gnb_ts_ns   the hook's existing CLOCK_REALTIME anchor, unchanged and
///                    not repurposed. It rides in aux2 raw.
///
/// \c gnb_ts_ns is what joins this ring to the consumer's. It is propagated
/// verbatim - hook to ctx, codelet to output struct, pipeline to the SM - and
/// the consumer stamps its own RECORD_BEGIN with the same value in the same
/// field, so the join is an equality on aux2 and needs no key negotiation. The
/// slot key in aux is the weaker, human-readable cross-check; it repeats every
/// 10.24 s, so it is not sufficient on its own.
inline std::uint64_t record_begin(std::uint16_t sfn, std::uint16_t slot_index, std::uint64_t gnb_ts_ns)
{
  const std::uint64_t s = ++detail::seq;
  latrec_stamp(&detail::ring, s, LATREC_RECORD_BEGIN, (static_cast<std::uint64_t>(sfn) << 16) | slot_index, gnb_ts_ns);
  return s;
}

/// A1 exit / A2 entry: the hook returned, so the slot is in jbpf's output ring
/// and the consumer can see it.
///
/// \param iq_size_bytes the payload the codelet had to move. Carried because it
///                      is most of the explanation for the interval that just
///                      closed - a 4-port 273-PRB slot is ~717 KiB of memcpy.
/// \param gnb_ts_ns     repeated from record_begin(), so a row whose A1 entry
///                      was lost off the front of a wrapped ring is still
///                      attributable to a slot.
inline void process_begin(std::uint64_t seq, std::uint32_t iq_size_bytes, std::uint64_t gnb_ts_ns)
{
  latrec_stamp(&detail::ring, seq, LATREC_PROCESS_BEGIN, iq_size_bytes, gnb_ts_ns);
}

/// Refresh the ring header's record count and closing clock pair, every
/// \c heartbeat_slots slots.
///
/// Rings are not closed on exit - a gNB is killed, not shut down cleanly - so
/// without this the header would still carry the values written at open, and an
/// offline reader could neither tell how many records were written nor fit the
/// monotonic-to-wall mapping that --wall needs.
///
/// The rate limit is here rather than at the call site so the policy lives with
/// the mechanism. 1024 slots is about half a second at 30 kHz SCS, which bounds
/// what a killed gNB loses off the end of its header to that, while leaving the
/// three header stores and their extra clock reads on one slot in 1024.
inline void heartbeat()
{
  constexpr std::uint64_t heartbeat_slots = 1024;
  if ((detail::seq & (heartbeat_slots - 1)) == 0) {
    latrec_heartbeat(&detail::ring);
  }
}

#else /* !LIBE3_ENABLE_LATREC */

inline void          open_ring() {}
inline std::uint64_t record_begin(std::uint16_t, std::uint16_t, std::uint64_t)
{
  return 0;
}
inline void process_begin(std::uint64_t, std::uint32_t, std::uint64_t) {}
inline void heartbeat() {}

#endif /* LIBE3_ENABLE_LATREC */

} // namespace slot_trace
} // namespace janus
} // namespace ocudu

#endif // OCUDU_JANUS_SLOT_TRACE_H
