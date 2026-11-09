// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "upper_phy_rx_symbol_handler_impl.h"
#include "upper_phy_rx_results_notifier_wrapper.h"
#include "ocudu/phy/support/prach_buffer_context.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/channel_processors/pusch/formatters.h"
#include "ocudu/phy/upper/rx_buffer_pool.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/phy/upper/uplink_processor.h"
#include "ocudu/support/error_handling.h"
#include <utility>

#ifdef JBPF_ENABLED
#include "jbpf_e3_publish.h"
#include "jbpf_srsran_hooks.h"
#include "ocudu/ran/cyclic_prefix.h"
#include <algorithm>
#include <time.h>
DEFINE_JBPF_HOOK(capture_uplink_slot);
#endif

using namespace ocudu;

#ifdef JBPF_ENABLED
/// \brief Uplink symbols of a slot as a bitmap: bit s set = symbol s is uplink.
///
/// Uses the same call as the OFH uplink request handler, so the mask is exactly the symbols the RU is asked to send.
/// DL and guard symbols of a mixed slot are 0. FDD (no TDD pattern) has every symbol set.
static uint16_t get_ul_symbol_mask(const std::optional<tdd_ul_dl_config_common>& tdd_pattern,
                                   slot_point                                    slot,
                                   unsigned                                      nof_symbols)
{
  if (!tdd_pattern) {
    return static_cast<uint16_t>((1U << nof_symbols) - 1U);
  }
  const cyclic_prefix cp = (nof_symbols == get_nsymb_per_slot(cyclic_prefix::EXTENDED)) ? cyclic_prefix::EXTENDED
                                                                                         : cyclic_prefix::NORMAL;
  const ofdm_symbol_range ul_symbols = get_active_tdd_ul_symbols(*tdd_pattern, slot.slot_index(), cp);
  uint16_t                mask       = 0;
  for (unsigned symbol = ul_symbols.start(); symbol < ul_symbols.stop(); ++symbol) {
    mask |= static_cast<uint16_t>(1U << symbol);
  }
  return mask;
}

/// Reception count for the slot hook, saturated below \c JBPF_SLOT_COUNT_UNKNOWN.
static uint8_t to_hook_count(unsigned count)
{
  return static_cast<uint8_t>(std::min(count, static_cast<unsigned>(JBPF_SLOT_COUNT_UNKNOWN) - 1U));
}
#endif

upper_phy_rx_symbol_handler_impl::upper_phy_rx_symbol_handler_impl(
    uplink_slot_processor_pool&            ul_processor_pool_,
    std::optional<tdd_ul_dl_config_common> tdd_pattern_) :
  ul_processor_pool(ul_processor_pool_), tdd_pattern(std::move(tdd_pattern_))
{
}

void upper_phy_rx_symbol_handler_impl::handle_rx_symbol(const upper_phy_rx_symbol_context& context,
                                                        const shared_resource_grid&        grid,
                                                        bool                               is_valid)
{
  // Get uplink processor.
  uplink_slot_processor& ul_proc = ul_processor_pool.get_slot_processor(context.slot);

  // Notify Rx symbol.
  ul_proc.handle_rx_symbol(context.symbol, is_valid);

#ifdef JBPF_ENABLED
  // Fire the slot-level capture hook once per slot, on the last symbol.
  // We hand the codelet the resource grid's raw storage pointer - no
  // per-symbol memcpy on the gnb side. The grid is contiguous cbf16_t
  // memory laid out as [port][symbol][subcarrier]. Grid lifetime is the
  // slot-processor window - the demod pipeline above is about to consume
  // the same buffer, so the pointer is live for the hook's duration.
  //
  // The codelet no longer copies the grid itself: it calls the native
  // e3_publish_slot helper, which converts cbf16 -> fp16 and writes the
  // row. eBPF cannot do that job - its ISA has no SIMD and no floating
  // point at all, so it cannot express the conversion.
  //
  // Gated on is_valid: a partial/invalid slot (PRACH-only TDD pattern,
  // dropped last symbol) is skipped rather than published with stale
  // or missing data.
  const resource_grid_reader& reader      = grid.get_reader();
  const unsigned              nof_symbols = reader.get_nof_symbols();
  if (is_valid && context.symbol + 1 == nof_symbols) {
    const unsigned nof_ports = reader.get_nof_ports();
    const unsigned nof_subc  = reader.get_nof_subc();
    const cbf16_t* buf       = reader.get_raw_storage();
    const uint32_t sz        = static_cast<uint32_t>(
        static_cast<size_t>(nof_ports) * nof_symbols * nof_subc * sizeof(cbf16_t));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t gnb_ts_ns =
        static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
    // Publish the window the helper is allowed to read from, so the source
    // bound is enforced at RUNTIME and not only proven offline. This matters
    // because the gNB does no verification at load time (ubpf only, and its
    // JIT emits no memory bounds checks) - the offline verifier is a build-time
    // gate, and nothing guarantees the loaded object passed through it.
    ocudu::janus::e3_set_src_window(buf, sz);

    // What the slot expects to receive from the UEs, so a codelet can tell a slot no UE transmits in.
    const std::optional<uplink_slot_reception_count> rx_count = ul_proc.get_reception_count(context.slot);
    const uint8_t n_ue     = rx_count ? to_hook_count(rx_count->nof_pusch) : JBPF_SLOT_COUNT_UNKNOWN;
    const uint8_t n_pucch  = rx_count ? to_hook_count(rx_count->nof_pucch) : JBPF_SLOT_COUNT_UNKNOWN;
    const uint8_t n_srs_ue = rx_count ? to_hook_count(rx_count->nof_srs) : JBPF_SLOT_COUNT_UNKNOWN;

    hook_capture_uplink_slot(
        buf,
        sz,
        gnb_ts_ns,
        static_cast<uint16_t>(context.slot.sfn()),
        static_cast<uint16_t>(context.slot.subframe_index()),
        static_cast<uint16_t>(context.slot.slot_index()),
        static_cast<uint16_t>(nof_ports),
        static_cast<uint16_t>(nof_symbols),
        static_cast<uint16_t>(nof_subc),
        get_ul_symbol_mask(tdd_pattern, context.slot, nof_symbols),
        n_ue,
        n_pucch,
        n_srs_ue,
        static_cast<uint16_t>(context.sector),
        JBPF_UL);
  }
#endif
}

void upper_phy_rx_symbol_handler_impl::handle_rx_prach_window(const prach_buffer_context& context,
                                                              shared_prach_buffer         buffer)
{
  // Get uplink processor.
  uplink_slot_processor& ul_proc = ul_processor_pool.get_slot_processor(context.slot);

  // Process PRACH.
  ul_proc.process_prach(std::move(buffer), context);
}
