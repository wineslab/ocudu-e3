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

#ifdef __cplusplus
}
#endif

#endif // JBPF_SRSRAN_HOOKS_
