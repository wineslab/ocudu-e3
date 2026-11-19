// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief SRS-based DOA estimator interface.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/tensor.h"
#include "ocudu/phy/upper/signal_processors/srs/doa_estimator_result.h"
#include <optional>

namespace ocudu {

/// \brief Direction of Arrival (DOA) estimator for Sounding Reference Signals.
///
/// The configuration of the antenna geometry and the working carrier frequency is implementation defined. See the
/// factory for more details.
class doa_estimator
{
public:
  /// Default destructor.
  virtual ~doa_estimator() = default;

  /// \brief Estimates the direction of arrival of \c srs_sequence from the sampled \c data.
  /// \param[in]  data          The samples from the antenna array (one column per antenna) corresponding to the
  ///                           received SRS sequence.
  /// \return The estimated direction of arrival is the computation was successful, \c nullopt otherwise (data is badly
  /// conditioned and the algorithm does not converge).
  virtual std::optional<doa_estimator_result> estimate(const tensor<2, cf_t>& data) = 0;
};

} // namespace ocudu
