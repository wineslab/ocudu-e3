# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# The benchmark needs libe3's headers, which only the jbpf configuration pulls
# in. It links nothing: slot_trace.h uses latrec's static inline explicit-ring
# API, so the whole mechanism is header-only on this side.
#
# CI builds and runs it standalone, with one g++ invocation and no gNB, so this
# target exists to keep it wired into the tree rather than to be the way it is
# usually built.
add_executable(slot_trace_benchmark slot_trace_benchmark.cpp)
