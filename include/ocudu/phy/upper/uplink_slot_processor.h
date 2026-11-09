// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Uplink processor interface.

#pragma once

#include "ocudu/phy/support/shared_prach_buffer.h"
#include <optional>

namespace ocudu {

struct prach_buffer_context;
class slot_point;
class upper_phy_rx_results_notifier;
struct srs_estimator_configuration;
struct resource_grid_context;
class shared_resource_grid;
class uplink_pdu_slot_repository;

/// Number of uplink reception requests of a slot, by channel.
struct uplink_slot_reception_count {
  /// PUSCH receptions, one for each scheduled UE.
  unsigned nof_pusch = 0;
  /// PUCCH receptions. PUCCH Format 1 receptions that share a configuration count once for each UE.
  unsigned nof_pucch = 0;
  /// SRS receptions.
  unsigned nof_srs = 0;
};

/// \brief Uplink slot processor.
///
/// Processes received symbols and PRACH.
class uplink_slot_processor
{
public:
  /// Default destructor.
  virtual ~uplink_slot_processor() = default;

  /// \brief Notify the reception of an OFDM symbol.
  ///
  /// \param[in] end_symbol_index Last received symbol in the slot.
  /// \param[in] is_valid         Indicates if the given OFDM symbol index has been received successfully.
  ///
  /// \remark An OFDM symbol might be invalid if a processing error occurred in the radio unit (e.g., losing an IQ
  ///         packet for a certain OFDM symbol).
  ///
  /// \remark The uplink slot processor discards all the receive requests that are allocated on the given slot and
  ///         afterward.
  virtual void handle_rx_symbol(unsigned end_symbol_index, bool is_valid) = 0;

  /// \brief Processes the PRACH using the given configuration and context.
  ///
  /// The PRACH detection results will be notified by the upper_phy_rx_results_notifier with event \ref
  /// upper_phy_rx_results_notifier::on_new_prach_results "on_new_prach_results".
  ///
  /// \param[in] buffer   Channel symbols the PRACH detection is performed on.
  /// \param[in] context  Context used by the underlying PRACH detector.
  virtual void process_prach(shared_prach_buffer buffer, const prach_buffer_context& context) = 0;

  /// Discards the slot processing due to an error.
  virtual void discard_slot() = 0;

  /// \brief Gets the number of reception requests of a slot, by channel.
  ///
  /// The count covers every request of the slot, also the ones already processed or discarded, so it is complete on
  /// the last symbol of the slot.
  ///
  /// \param[in] slot Slot the count is for.
  /// \return The count, or \c std::nullopt if the processor does not hold the requests of the slot.
  virtual std::optional<uplink_slot_reception_count> get_reception_count(slot_point slot) const = 0;
};

/// \brief Pool of uplink processors.
///
/// This interface manages the access to the available uplink processors.
class uplink_slot_processor_pool
{
public:
  /// Default destructor.
  virtual ~uplink_slot_processor_pool() = default;

  /// \brief Returns an uplink slot processor for the given slot.
  ///
  /// Slots must be requested in order to detects gaps, intermediate slots that are not requested will be considered
  /// lost and the uplink processors will be released accordingly.
  ///
  /// \param slot[in]      Slot point.
  /// \return An uplink processor.
  virtual uplink_slot_processor& get_slot_processor(slot_point slot) = 0;
};
} // namespace ocudu
