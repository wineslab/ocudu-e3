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
#include "jbpf_srsran_hooks.h"
#include <time.h>
DEFINE_JBPF_HOOK(capture_uplink_slot);
#endif

using namespace ocudu;

upper_phy_rx_symbol_handler_impl::upper_phy_rx_symbol_handler_impl(uplink_slot_processor_pool& ul_processor_pool_) :
  ul_processor_pool(ul_processor_pool_)
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
  // memory laid out as [port][symbol][subcarrier]; the codelet copies
  // it out within the hook body. Grid lifetime is the slot-processor
  // window - the demod pipeline above is about to consume the same
  // buffer, so the pointer is live for the hook's duration.
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
    // RAN-side anchor for end-to-end latency. CLOCK_REALTIME matches
    // jbpf_time_get_ns() (CLOCK_REALTIME ns) so codelet-side codelet_ts_ns
    // and this gnb_ts_ns subtract cleanly. Captured immediately before
    // the hook fires so it represents the moment the gNB hands the slot
    // off to the codelet.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t gnb_ts_ns =
        static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
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
