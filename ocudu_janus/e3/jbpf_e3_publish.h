/*
 * jbpf_e3_publish.h — gNB-side E3 publish helpers.
 *
 * Registers two jbpf helper functions that let a verified codelet publish a
 * decompressed uplink slot into the E3Controller-owned /e3_ran_buffers region,
 * in the dApp's fp16 row format, without the codelet ever touching the bytes or
 * holding a pointer into the destination.
 *
 * Why the copy is not in the codelet any more: the eBPF ISA has no SIMD and no
 * floating point, so it cannot perform the cbf16 -> fp16 conversion at all, and
 * its in-VM copy ran at ~8.5 GB/s. The native fused convert measures 66.5 us for
 * a 4-port slot (22.1 GB/s, only 1.26x above memcpy), and replaces BOTH the old
 * eBPF copy (56 us, RX thread) and the controller's publish_row_cbf16 (74 us).
 *
 * The eBPF safety model is preserved, and in one respect improved: the codelet
 * passes a selector and receives back the (buffer, row) it was given, so the
 * destination address is derived entirely from geometry this unit owns and
 * cannot be forged from inside the VM.
 */

#ifndef OCUDU_JANUS_JBPF_E3_PUBLISH_H
#define OCUDU_JANUS_JBPF_E3_PUBLISH_H

#include <cstdint>

namespace ocudu {
namespace janus {

/*
 * Register jbpf_e3_attach and jbpf_e3_publish_slot with the jbpf agent.
 *
 * MUST be called after jbpf_init() and BEFORE any codeletset is loaded:
 * jbpf_register_helper_functions() copies the table into each VM at load time
 * (external/jbpf/src/core/jbpf_helper_impl.c), so a codelet loaded earlier would
 * not see these.
 *
 * Returns 0 on success, negative if jbpf rejected a registration.
 */
int e3_register_publish_helpers();

/*
 * Publish the window of memory the next hook invocation is allowed to read from.
 *
 * Call immediately before firing capture_uplink_slot. This is what makes the
 * SOURCE bound enforced at runtime rather than only proven statically — it does
 * not depend on the codelet having been verified, which matters because the gNB
 * does no verification at load time (ubpf only, and its JIT emits no memory
 * bounds checks).
 *
 * Thread-local: each RU UL execution context has its own window.
 */
void e3_set_src_window(const void* base, std::uint32_t len);

/*
 * True if a vector (AVX2+F16C) convert path was compiled in. Log at startup:
 * the scalar fallback is 47x slower — 2,490 us for a 4-port slot against a
 * 500 us slot period — and is otherwise completely invisible.
 */
bool e3_publish_has_vector_path();

} // namespace janus
} // namespace ocudu

#endif /* OCUDU_JANUS_JBPF_E3_PUBLISH_H */
