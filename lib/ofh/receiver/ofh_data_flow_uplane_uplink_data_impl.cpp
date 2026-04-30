// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ofh_data_flow_uplane_uplink_data_impl.h"
#include "../support/uplane_bound_check_helpers.h"
#include "ocudu/instrumentation/traces/ofh_traces.h"
#include "ocudu/ofh/serdes/ofh_message_decoder_properties.h"

#ifdef JBPF_ENABLED
#include "jbpf_srsran_hooks.h"
DEFINE_JBPF_HOOK(capture_uplane_iq);
#endif

using namespace ocudu;
using namespace ofh;

data_flow_uplane_uplink_data_impl::data_flow_uplane_uplink_data_impl(
    const data_flow_uplane_uplink_data_impl_config&  config,
    data_flow_uplane_uplink_data_impl_dependencies&& dependencies) :
  logger(*dependencies.logger),
  ul_cplane_context_repo(std::move(dependencies.ul_cplane_context_repo)),
  uplane_decoder(std::move(dependencies.uplane_decoder)),
  rx_symbol_writer(config.ul_eaxc, config.sector, *dependencies.logger, dependencies.ul_context_repo),
  notification_sender(*dependencies.logger, dependencies.ul_context_repo, dependencies.notifier),
  sector_id(config.sector),
  metrics_collector(config.are_metrics_enabled)
{
  ocudu_assert(ul_cplane_context_repo, "Invalid control plane repository");
  ocudu_assert(uplane_decoder, "Invalid User-Plane decoder");
}

void data_flow_uplane_uplink_data_impl::decode_type1_message(unsigned eaxc, span<const uint8_t> message)
{
  trace_point decode_tp = ofh_tracer.now();

  uplane_message_decoder_results results;
  if (!uplane_decoder->decode(results, message)) {
    metrics_collector.increase_dropped_messages();
    return;
  }
  ofh_tracer << trace_event("ofh_receiver_uplane_decode", decode_tp);

#ifdef JBPF_ENABLED
  // Post-decode UL hook: emits already-decompressed cbf16_t IQ + parsed
  // slot/sym/eAxC metadata. One fire per section (typically 1 per packet).
  for (const auto& section : results.sections) {
    hook_capture_uplane_iq(
        section.iq_samples.data(),
        static_cast<uint32_t>(section.iq_samples.size() * sizeof(cbf16_t)),
        static_cast<uint16_t>(results.params.slot.sfn()),
        static_cast<uint16_t>(results.params.slot.subframe_index()),
        static_cast<uint16_t>(results.params.slot.slot_index()),
        static_cast<uint16_t>(results.params.symbol_id),
        static_cast<uint16_t>(section.start_prb),
        static_cast<uint16_t>(section.nof_prbs),
        static_cast<uint16_t>(eaxc),
        JBPF_UL);
  }
#endif

  if (should_uplane_packet_be_filtered(eaxc, results)) {
    metrics_collector.increase_dropped_messages();

    return;
  }

  if (!rx_symbol_writer.write_to_resource_grid(eaxc, results)) {
    metrics_collector.increase_dropped_messages();

    return;
  }

  notification_sender.notify_received_symbol(results.params.slot, results.params.symbol_id);
}

bool data_flow_uplane_uplink_data_impl::should_uplane_packet_be_filtered(
    unsigned                              eaxc,
    const uplane_message_decoder_results& results) const
{
  if (OCUDU_UNLIKELY(results.params.filter_index == filter_index_type::reserved ||
                     is_a_prach_message(results.params.filter_index))) {
    logger.info("Sector#{}: dropped received Open Fronthaul User-Plane packet for slot '{}' and symbol '{}' as decoded "
                "filter index value '{}' is not valid",
                sector_id,
                results.params.slot,
                results.params.symbol_id,
                to_value(results.params.filter_index));

    return true;
  }

  const uplane_message_params& params  = results.params;
  ul_cplane_context            context = ul_cplane_context_repo->get(params.slot, eaxc);

  // Check if the filter index is valid.
  if (OCUDU_UNLIKELY(params.filter_index != context.filter_index)) {
    logger.info("Sector#{}: dropped received Open Fronthaul User-Plane packet as the expected filter index '{}' does "
                "not match with value '{}' for slot '{}', symbol '{}' and eAxC '{}'",
                sector_id,
                to_value(context.filter_index),
                to_value(params.filter_index),
                params.slot,
                params.symbol_id,
                eaxc);
    return true;
  }

  // Check if the symbol index is valid.
  if (OCUDU_UNLIKELY(!is_symbol_index_in_cplane_valid(context, params.symbol_id))) {
    logger.info("Sector#{}: dropped received Open Fronthaul User-Plane packet as the symbol index '{}' is invalid for "
                "slot '{}', and eAxC '{}'",
                sector_id,
                params.symbol_id,
                params.slot,
                eaxc);
    return true;
  }

  // Check if the PRB ranges are valid.
  return !are_uplane_prb_fields_valid(results, context, sector_id, logger);
}
