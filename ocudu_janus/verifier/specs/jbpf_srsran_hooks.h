#ifndef JBPF_SRSRAN_HOOKS_
#define JBPF_SRSRAN_HOOKS_

/*
 * ocudu-janus hooks.
 *
 * For this project the only hook wired into the RAN data-plane is
 * capture_xran_packet, used by the eCPRI I/Q sampling codelet.
 * Additional hooks (L2/MAC/PDCP/RLC/RRC/NGAP/E1AP) are declared in the
 * upstream srsRAN_Project_jbpf specs and can be ported here on demand.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "jbpf_hook.h"
#include "jbpf_srsran_defs.h"
#include "jbpf_srsran_contexts.h"

DECLARE_JBPF_HOOK(capture_xran_packet,
        struct jbpf_ran_ofh_ctx ctx,
        ctx,
        HOOK_PROTO(
            const void* xran_packet,
            uint32_t packet_len,
            uint16_t ctx_id,
            JbpfDirection_t direction
            ),
        HOOK_ASSIGN(
            ctx.ctx_id = ctx_id;
            ctx.data = (uint64_t)xran_packet;
            ctx.data_end = (uint64_t) ((uint8_t *)xran_packet + packet_len);
            ctx.direction = direction;
            )
        )

/*
 * capture_uplane_iq: post-decode hook.
 *
 * Fires once per (decoded) U-plane section after uplane_decoder->decode()
 * succeeds. The IQ samples are already decompressed (cbf16_t, 4 bytes per
 * complex sample) and the slot/symbol/eAxC metadata is already parsed -
 * the codelet does not need to walk the eCPRI/Open Fronthaul headers.
 */
DECLARE_JBPF_HOOK(capture_uplane_iq,
        struct jbpf_ran_uplane_ctx ctx,
        ctx,
        HOOK_PROTO(
            const void* iq_samples,
            uint32_t iq_size_bytes,
            uint16_t sfn,
            uint16_t subframe_id,
            uint16_t slot_id,
            uint16_t symbol_id,
            uint16_t start_prb,
            uint16_t nof_prbs,
            uint16_t eaxc,
            JbpfDirection_t direction
            ),
        HOOK_ASSIGN(
            ctx.ctx_id = 1;
            ctx.data = (uint64_t)iq_samples;
            ctx.data_end = (uint64_t)((const uint8_t*)iq_samples + iq_size_bytes);
            ctx.sfn = sfn;
            ctx.subframe_id = subframe_id;
            ctx.slot_id = slot_id;
            ctx.symbol_id = symbol_id;
            ctx.start_prb = start_prb;
            ctx.nof_prbs = nof_prbs;
            ctx.eaxc = eaxc;
            ctx.direction = direction;
            )
        )

#ifdef __cplusplus
}
#endif

#endif // JBPF_SRSRAN_HOOKS_
