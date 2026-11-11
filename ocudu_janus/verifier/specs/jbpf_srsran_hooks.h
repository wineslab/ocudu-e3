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

/*
 * capture_uplink_slot: per-slot snapshot hook.
 *
 * Fires once per UL slot, on the last symbol, after the upper-PHY
 * symbol handler has populated the resource grid with the full slot.
 * The codelet receives a pointer to a contiguous cbf16_t buffer of
 * shape [nof_ports][nof_symbols][nof_subcarriers] - already
 * decompressed (RU did the BFP decompression) and laid out in the
 * resource grid's natural order. No eCPRI/Open Fronthaul parsing
 * required on the codelet side; metadata (sfn, subframe, slot, sector,
 * grid dimensions, which symbols are uplink, how many UEs the slot expects
 * on PUSCH, PUCCH and SRS) comes through the ctx struct.
 *
 * Buffer lifetime: the snapshot is owned by the upper_phy_rx_symbol_
 * handler instance and reused across slots. The codelet must finish
 * reading or copy out before returning - the next slot will overwrite
 * the same memory.
 */
DECLARE_JBPF_HOOK(capture_uplink_slot,
        struct jbpf_ran_slot_ctx ctx,
        ctx,
        HOOK_PROTO(
            const void* iq_grid,
            uint32_t iq_size_bytes,
            uint64_t gnb_ts_ns,
            uint16_t sfn,
            uint16_t subframe_id,
            uint16_t slot_id,
            uint16_t nof_ports,
            uint16_t nof_symbols,
            uint16_t nof_subcarriers,
            uint16_t valid_symbol_mask,
            uint8_t n_ue,
            uint8_t n_pucch,
            uint8_t n_srs_ue,
            uint16_t sector_id,
            JbpfDirection_t direction
            ),
        HOOK_ASSIGN(
            ctx.ctx_id = sector_id;
            ctx.data = (uint64_t)iq_grid;
            ctx.data_end = (uint64_t)((const uint8_t*)iq_grid + iq_size_bytes);
            ctx.gnb_ts_ns = gnb_ts_ns;
            ctx.sfn = sfn;
            ctx.subframe_id = subframe_id;
            ctx.slot_id = slot_id;
            ctx.nof_ports = nof_ports;
            ctx.nof_symbols = nof_symbols;
            ctx.nof_subcarriers = nof_subcarriers;
            ctx.valid_symbol_mask = valid_symbol_mask;
            ctx.n_ue = n_ue;
            ctx.n_pucch = n_pucch;
            ctx.n_srs_ue = n_srs_ue;
            ctx.direction = direction;
            )
        )

/*
 * sched_prb_block: control hook, fired by the scheduler at the start of every
 * slot of every cell, before any allocation (cell_scheduler::run_slot).
 *
 * The codelet answers which PRBs data allocations must avoid in this slot: it
 * writes them into the buffer, a struct e3_prb_block_slot owned by the
 * scheduler (ocudu_janus/e3/jbpf_e3_prb_block_api.h), and returns
 * E3_PRB_BLOCK_APPLY, or returns E3_PRB_BLOCK_NONE. With no codelet loaded the
 * hook returns jbpf's default, 0, which is E3_PRB_BLOCK_NONE.
 *
 * The ctx is the MAC scheduler one; only data, data_end and cell_id are set.
 */
DECLARE_JBPF_CTRL_HOOK(sched_prb_block,
        struct jbpf_mac_sched_ctx ctx,
        ctx,
        HOOK_PROTO(
            void* slot_io,
            uint32_t slot_io_len,
            uint16_t cell_index
            ),
        HOOK_ASSIGN(
            ctx.data = (uint64_t)slot_io;
            ctx.data_end = (uint64_t)((uint8_t*)slot_io + slot_io_len);
            ctx.meta_data = 0;
            ctx.srs_meta_data1 = 0;
            ctx.ctx_id = 0;
            ctx.du_ue_index = 0;
            ctx.cell_id = cell_index;
            ctx.rnti = 0;
            )
        )

#ifdef __cplusplus
}
#endif

#endif // JBPF_SRSRAN_HOOKS_
