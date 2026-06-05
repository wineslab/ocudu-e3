// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/support/resource_grid_base.h"
#include "ocudu/ran/resource_block.h"

namespace ocudu {

/// Describes a resource grid reader interface.
class resource_grid_reader : public resource_grid_base
{
public:
  /// Default destructor.
  virtual ~resource_grid_reader() = default;

  /// \brief Determines whether a given port is empty.
  ///
  /// A port is considered empty if no \c put method have been called since last zero set.
  ///
  /// If zeros are written using any \c put method, the port is considered not empty.
  ///
  /// An invalid port index triggers an assertion.
  ///
  /// \param[in] port Port index.
  /// \return True if the given port is empty. Otherwise, false.
  virtual bool is_empty(unsigned port) const = 0;

  /// \brief Determines whether the resource grid is empty.
  ///
  /// The grid is considered empty if no \c put method have been called since last zero set.
  ///
  /// If zeros are written using any \c put method, the grid is considered not empty.
  ///
  /// \return \c true if the resource grid is empty. Otherwise, \c false.
  virtual bool is_empty() const = 0;

  /// \brief Gets a number of resource elements in the resource grid at the given port and symbol using a bounded bitset
  /// to indicate which subcarriers are allocated and which are not.
  ///
  /// \param[out] symbols Destination symbol buffer.
  /// \param[in]  port    Port index.
  /// \param[in]  l       OFDM symbol index.
  /// \param[in]  k_init  Initial subcarrier index.
  /// \param[in]  mask    Boolean mask denoting the subcarriers to be read (if \c true), starting from \c k_init.
  /// \return A view to the unused entries of \c symbols.
  /// \note The initial subcarrier plus the number of elements of \c mask shall not exceed the number of resource grid
  ///       subcarriers.
  /// \note The number of elements of \c symbols shall be equal to or greater than the number of true elements in
  ///       \c mask.
  virtual span<cf_t> get(span<cf_t>                                 symbols,
                         unsigned                                   port,
                         unsigned                                   l,
                         unsigned                                   k_init,
                         const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask) const = 0;

  /// \brief Gets a number of resource elements in the resource grid at the given port and symbol using a bounded bitset
  /// to indicate which subcarriers are allocated and which are not.
  ///
  /// \param[out] symbols Destination symbol buffer.
  /// \param[in]  port    Port index.
  /// \param[in]  l       OFDM symbol index.
  /// \param[in]  k_init  Initial subcarrier index.
  /// \param[in] mask     Boolean mask denoting the subcarriers to be read (if \c true), starting from \c k_init.
  /// \return A view to the unused entries of \c symbols.
  /// \note The initial subcarrier plus the number of elements of \c mask shall not exceed the number of resource grid
  ///       subcarriers.
  /// \note The number of elements of \c symbols shall be equal to or greater than the number of true elements in
  ///       \c mask.
  virtual span<cbf16_t> get(span<cbf16_t>                              symbols,
                            unsigned                                   port,
                            unsigned                                   l,
                            unsigned                                   k_init,
                            const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask) const = 0;

  /// \brief Gets a number of resource elements for a given port and symbol \c l starting at \c k_init and picks the
  /// first element every \c stride.
  ///
  /// \param[out] symbols Destination symbol buffer.
  /// \param[in]  port    Port index.
  /// \param[in]  l       OFDM symbol index.
  /// \param[in]  k_init  Initial subcarrier index.
  /// \param[in]  stride  Distance between the elements to get.
  /// \note The sum of \c k_init and the number of elements in \c symbols shall not exceed the resource grid number of
  /// subcarriers.
  virtual void get(span<cf_t> symbols, unsigned port, unsigned l, unsigned k_init, unsigned stride = 1) const = 0;

  /// \brief Gets a consecutive number of resource elements for a given port and OFDM symbol \c l starting at
  /// subcarrier \c k_init.
  ///
  /// \param[out] symbols Destination symbol buffer.
  /// \param[in]  port    Port index.
  /// \param[in]  l       OFDM symbol index.
  /// \param[in]  k_init  Initial subcarrier index.
  /// \note The sum of \c k_init and the number of elements in \c symbols shall not exceed the resource grid number of
  /// subcarriers.
  virtual void get(span<cbf16_t> symbols, unsigned port, unsigned l, unsigned k_init) const = 0;

  /// \brief Gets a view of all resource elements for a given port and symbol \c l.
  ///
  /// \param[in]  port    Port index.
  /// \param[in]  l       OFDM symbol index.
  virtual span<const cbf16_t> get_view(unsigned port, unsigned l) const = 0;

#ifdef JBPF_ENABLED
  /// \brief Gets a read-only pointer to the contiguous backing storage of the
  /// resource grid.
  ///
  /// The returned buffer covers `get_nof_ports() * get_nof_symbols() *
  /// get_nof_subc()` elements laid out as `[port][symbol][subcarrier]` in row-
  /// major order. Intended for the capture_uplink_slot jbpf hook (and any
  /// future zero-copy observer) that needs to read or copy out the entire
  /// grid in one shot without going through the per-symbol get_view API.
  ///
  /// Lifetime: the pointer is valid only while the resource grid instance is
  /// alive; observers must not retain it beyond the slot processing window
  /// during which they obtained it.
  ///
  /// Only declared when JBPF_ENABLED is set so this jbpf-specific accessor
  /// doesn't leak into default builds.
  virtual const cbf16_t* get_raw_storage() const = 0;
#endif
};

} // namespace ocudu
