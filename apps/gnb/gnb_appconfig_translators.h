// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#ifdef JBPF_ENABLED
#include "jbpf.h"
#endif

namespace ocudu {

struct gnb_appconfig;
struct worker_manager_config;

/// Fills the gNB worker manager parameters of the given worker manager configuration.
void fill_gnb_worker_manager_config(worker_manager_config& config, const gnb_appconfig& unit_cfg);

#ifdef JBPF_ENABLED
/// Translates the gNB-level janus (jbpf) appconfig block into a jbpf_config that jbpf_init() accepts.
void generate_jbpf_config(const gnb_appconfig& config, struct jbpf_config* jcfg);
#endif


} // namespace ocudu
