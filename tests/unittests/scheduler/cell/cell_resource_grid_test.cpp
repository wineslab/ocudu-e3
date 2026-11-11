// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/scheduler/cell/resource_grid.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/unittests/scheduler/test_utils/config_generators.h"
#include "ocudu/scheduler/config/scheduler_expert_config_factory.h"
#include "ocudu/support/ocudu_test.h"
#include <gtest/gtest.h>

using namespace ocudu;

std::vector<scs_specific_carrier> test_carriers()
{
  std::vector<scs_specific_carrier> ret;
  // 15 kHz.
  ret.emplace_back();
  ret.back().scs               = subcarrier_spacing::kHz15;
  ret.back().offset_to_carrier = 0;
  ret.back().carrier_bandwidth = 52;
  // 120 kHz.
  ret.emplace_back();
  ret.back().scs               = subcarrier_spacing::kHz120;
  ret.back().offset_to_carrier = 0;
  ret.back().carrier_bandwidth = 275;
  return ret;
}

TEST(carrier_subslot_resource_grid_test, test_all)
{
  std::vector<scs_specific_carrier> carrier_cfgs = test_carriers();

  // Wideband Carrier, 15kHz case.
  {
    carrier_subslot_resource_grid carrier_grid(carrier_cfgs[0]);
    TESTASSERT_EQ(fmt::underlying(subcarrier_spacing::kHz15), fmt::underlying(carrier_grid.scs()));
    TESTASSERT_EQ(52, carrier_grid.nof_rbs());
    TESTASSERT_EQ(0, carrier_grid.offset());
    crb_interval lims{0, 52};
    TESTASSERT(carrier_grid.rb_dims() == lims);
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{0, 14}, lims));
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{2, 5}, crb_interval{2, 5}));
    TESTASSERT(not carrier_grid.all_set(ofdm_symbol_range{0, 14}, lims));

    carrier_grid.fill({2, 5}, crb_interval{2, 4});
    TESTASSERT(carrier_grid.collides(ofdm_symbol_range{2, 3}, crb_interval{1, 3}));
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{2, 3}, crb_interval{1, 2}));
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{0, 1}, crb_interval{1, 3}));
    TESTASSERT(carrier_grid.all_set(ofdm_symbol_range{2, 5}, crb_interval{2, 4}));
    TESTASSERT(carrier_grid.all_set(ofdm_symbol_range{2, 4}, crb_interval{2, 3}));
    TESTASSERT(carrier_grid.all_set(ofdm_symbol_range{3, 5}, crb_interval{3, 4}));
    TESTASSERT(not carrier_grid.all_set(ofdm_symbol_range{1, 5}, crb_interval{1, 4}));
    TESTASSERT(not carrier_grid.all_set(ofdm_symbol_range{2, 6}, crb_interval{2, 5}));

    carrier_grid.fill(ofdm_symbol_range{0, 14}, lims);
    TESTASSERT(carrier_grid.all_set(ofdm_symbol_range{0, 14}, lims));
    TESTASSERT(carrier_grid.all_set(ofdm_symbol_range{0, 13}, {0, 51}));
    TESTASSERT(carrier_grid.all_set(ofdm_symbol_range{1, 14}, {1, 52}));

    carrier_grid.clear();
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{2, 3}, crb_interval{1, 3}));
  }

  // Narrowband Carrier, 15kHz case.
  {
    scs_specific_carrier carrier_cfg = carrier_cfgs[0];
    carrier_cfg.offset_to_carrier    = 10;
    carrier_cfg.carrier_bandwidth    = 20;
    carrier_subslot_resource_grid carrier_grid(carrier_cfg);
    TESTASSERT_EQ(fmt::underlying(subcarrier_spacing::kHz15), fmt::underlying(carrier_grid.scs()));
    TESTASSERT_EQ(20, carrier_grid.nof_rbs());
    TESTASSERT_EQ(10, carrier_grid.offset());
    crb_interval lims{10, 30};
    TESTASSERT(carrier_grid.rb_dims() == lims);
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{0, 14}, lims));
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{2, 5}, crb_interval{12, 15}));

    carrier_grid.fill({2, 5}, crb_interval{12, 14});
    TESTASSERT(carrier_grid.collides(ofdm_symbol_range{2, 3}, crb_interval{11, 13}));
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{2, 3}, crb_interval{11, 12}));
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{0, 1}, crb_interval{11, 13}));

    carrier_grid.clear();
    TESTASSERT(not carrier_grid.collides(ofdm_symbol_range{2, 3}, crb_interval{11, 13}));
  }
}

TEST(cell_resource_grid_test, test_all)
{
  std::vector<scs_specific_carrier> carrier_cfgs = test_carriers();

  // Wide BWP, 15 kHz case.
  {
    cell_slot_resource_grid cell_grid{carrier_cfgs};
    bwp_configuration       bwp_cfg{};
    bwp_cfg.scs  = ocudu::subcarrier_spacing::kHz15;
    bwp_cfg.crbs = {0, 52};

    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info(bwp_cfg, {0, 14}, {0, 52})));

    bwp_sch_grant_info grant{bwp_cfg, {2, 14}, {5, 10}};
    cell_grid.fill(grant);
    TESTASSERT(cell_grid.collides(subcarrier_spacing::kHz15, {0, 14}, crb_interval{0, 52}));
    TESTASSERT(not cell_grid.collides(subcarrier_spacing::kHz15, {0, 14}, crb_interval{0, 5}));
    TESTASSERT(cell_grid.collides(subcarrier_spacing::kHz15, {2, 3}, crb_interval{0, 6}));
    TESTASSERT(not cell_grid.collides(subcarrier_spacing::kHz15, {0, 2}, crb_interval{0, 6}));

    cell_grid.clear();
    TESTASSERT(not cell_grid.collides(subcarrier_spacing::kHz15, {2, 3}, crb_interval{0, 6}));
  }

  // Narrow BWP, 15 kHz case.
  {
    cell_slot_resource_grid cell_grid{carrier_cfgs};
    bwp_configuration       bwp_cfg{};
    bwp_cfg.scs  = ocudu::subcarrier_spacing::kHz15;
    bwp_cfg.crbs = {10, 30};

    TESTASSERT(not cell_grid.collides(subcarrier_spacing::kHz15, {0, 14}, crb_interval{0, 52}));

    bwp_sch_grant_info grant{bwp_cfg, {2, 14}, {5, 10}};
    cell_grid.fill(grant);
    TESTASSERT(cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {0, 14}, {0, 20}}));
    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {0, 14}, {0, 5}}));
    TESTASSERT(cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {2, 3}, {0, 6}}));
    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {0, 2}, {0, 6}}));

    cell_grid.clear();
    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {2, 3}, {0, 6}}));
  }

  // Wide BWP, 120 kHz case.
  {
    cell_slot_resource_grid cell_grid{carrier_cfgs};
    bwp_configuration       bwp_cfg{};
    bwp_cfg.scs  = ocudu::subcarrier_spacing::kHz120;
    bwp_cfg.crbs = {10, 275};

    TESTASSERT(not cell_grid.collides(subcarrier_spacing::kHz120, {0, 14}, crb_interval{0, 265}));

    bwp_sch_grant_info grant{bwp_cfg, {2, 14}, {5, 200}};
    cell_grid.fill(grant);
    TESTASSERT(cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {0, 14}, {10, 30}}));
    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {0, 14}, {0, 5}}));
    TESTASSERT(cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {2, 3}, {0, 30}}));
    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {0, 2}, {0, 30}}));

    cell_grid.clear();
    TESTASSERT(not cell_grid.collides(bwp_sch_grant_info{bwp_cfg, {2, 3}, {0, 30}}));
  }
}

/// Test allocation of resources in a cell resource grid
TEST(pusch_resource_allocation_test, test_all)
{
  scheduler_expert_config                 sched_cfg = config_helpers::make_default_scheduler_expert_config();
  test_helpers::test_sched_config_manager cfg_mng{sched_cfg};
  const cell_configuration&               cell_cfg =
      *cfg_mng.add_cell(sched_config_helper::make_default_sched_cell_configuration_request());
  cell_resource_allocator res_grid_alloc{cell_cfg};
  bwp_configuration       bwp_cfg{};
  bwp_cfg.crbs = {0, 52};
  bwp_cfg.scs  = cell_cfg.params.dl_cfg_common.freq_info_dl.scs_carrier_list[0].scs;

  slot_point sl_tx{0, 0};

  // Action 1: New slot
  res_grid_alloc.slot_indication(sl_tx);

  // Test: resource allocator operator[] returns a slot_allocator pointing at the correct slot
  TESTASSERT_EQ(sl_tx, res_grid_alloc.slot_tx());
  TESTASSERT_EQ(sl_tx, res_grid_alloc[0].slot);
  TESTASSERT_EQ(sl_tx + 1, res_grid_alloc[1].slot);

  // Test: No allocations made yet
  TESTASSERT(res_grid_alloc[0].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).none());
  TESTASSERT(res_grid_alloc[0].result.ul.puschs.empty());

  // Action 2: Allocate PUSCH grant in current slot_tx
  bwp_sch_grant_info ul_grant{bwp_cfg, {0, 14}, {1, 5}};
  res_grid_alloc[0].ul_res_grid.fill(ul_grant);
  res_grid_alloc[0].result.ul.puschs.emplace_back();

  // Test: Allocated PUSCH was registered in the cell resource grid for slot_tx
  TESTASSERT(res_grid_alloc[0].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).any());
  TESTASSERT_EQ(res_grid_alloc[0].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).count(), ul_grant.prbs.length());
  TESTASSERT_EQ(1, res_grid_alloc[0].result.ul.puschs.size());

  // Action 3: Allocate PUSCH grant in slot_tx + 1
  bwp_sch_grant_info ul_grant2{bwp_cfg, {0, 14}, {4, 20}};
  res_grid_alloc[1].ul_res_grid.fill(ul_grant2);
  res_grid_alloc[1].result.ul.puschs.emplace_back();

  // Test: Allocated PUSCH was registered in the cell resource grid for slot_tx + 1
  TESTASSERT(res_grid_alloc[1].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).any());
  TESTASSERT_EQ(res_grid_alloc[1].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).count(), ul_grant2.prbs.length());
  TESTASSERT_EQ(1, res_grid_alloc[1].result.ul.puschs.size());

  // Action 4: New slot
  res_grid_alloc.slot_indication(++sl_tx);

  // Test: Current slot_tx allocations match the ones done with "slot_tx + 1" in the previous slot
  TESTASSERT_EQ(res_grid_alloc[0].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).count(), ul_grant2.prbs.length());
}

/// A dApp's PRB block: data allocations, which search through used_crbs() and used_prbs(), see the blocked RBs; the
/// other channels, which check collides(), do not.
TEST(cell_resource_grid_test, blocked_rbs_reach_data_allocations_only)
{
  std::vector<scs_specific_carrier> carrier_cfgs = test_carriers();
  cell_slot_resource_grid           cell_grid{carrier_cfgs};
  bwp_configuration                 bwp_cfg{};
  bwp_cfg.scs  = subcarrier_spacing::kHz15;
  bwp_cfg.crbs = {0, 52};

  crb_bitmap blocked(MAX_NOF_PRBS);
  cell_grid.set_blocked_rbs(&blocked);
  ASSERT_TRUE(cell_grid.used_crbs(bwp_cfg, {0, 14}).none());

  // Every symbol of the slot: a data allocation over any symbol range avoids the blocked RBs.
  blocked.fill(10, 20);
  crb_bitmap used = cell_grid.used_crbs(bwp_cfg, {2, 14});
  ASSERT_EQ(used.count(), 10U);
  ASSERT_TRUE(used.all(10, 20));
  ASSERT_EQ(cell_grid.used_crbs(subcarrier_spacing::kHz15, {0, 52}, {0, 1}), used);

  // BWP-relative bitmap: a BWP starting at CRB 5 sees PRBs 5 to 14 blocked.
  prb_bitmap prbs = cell_grid.used_prbs(subcarrier_spacing::kHz15, {5, 45}, {0, 14});
  ASSERT_EQ(prbs.size(), 40U);
  ASSERT_EQ(prbs.count(), 10U);
  ASSERT_TRUE(prbs.all(5, 15));

  // Allocations add to the block.
  cell_grid.fill(bwp_sch_grant_info(bwp_cfg, {2, 14}, {30, 35}));
  ASSERT_EQ(cell_grid.used_crbs(bwp_cfg, {2, 14}).count(), 15U);

  // PUCCH, PDCCH, SSB, PRACH, SRS: collision checks read the grid only.
  ASSERT_FALSE(cell_grid.collides(subcarrier_spacing::kHz15, {0, 14}, crb_interval{10, 20}));

  // Lifting the block releases the RBs; the allocation stays.
  blocked.reset();
  ASSERT_EQ(cell_grid.used_crbs(bwp_cfg, {2, 14}).count(), 5U);
}

/// The block is set once per cell and applies to every slot of the ring, DL and UL, until it is cleared.
TEST(cell_resource_grid_test, blocked_rbs_apply_to_every_slot_until_cleared)
{
  scheduler_expert_config                 sched_cfg = config_helpers::make_default_scheduler_expert_config();
  test_helpers::test_sched_config_manager cfg_mng{sched_cfg};
  const cell_configuration&               cell_cfg =
      *cfg_mng.add_cell(sched_config_helper::make_default_sched_cell_configuration_request());
  cell_resource_allocator res_grid_alloc{cell_cfg};
  bwp_configuration       bwp_cfg{};
  bwp_cfg.crbs = {0, 52};
  bwp_cfg.scs  = cell_cfg.params.dl_cfg_common.freq_info_dl.scs_carrier_list[0].scs;

  slot_point sl_tx{0, 0};
  res_grid_alloc.slot_indication(sl_tx);

  crb_bitmap blocked(MAX_NOF_PRBS);
  blocked.fill(3, 7);
  res_grid_alloc.set_blocked_rbs(blocked);
  for (unsigned k = 0; k != 8; ++k) {
    ASSERT_TRUE(res_grid_alloc[k].dl_res_grid.used_crbs(bwp_cfg, {0, 14}).all(3, 7)) << "slot +" << k;
    ASSERT_TRUE(res_grid_alloc[k].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).all(3, 7)) << "slot +" << k;
    ASSERT_EQ(res_grid_alloc[k].ul_res_grid.used_crbs(bwp_cfg, {0, 14}).count(), 4U) << "slot +" << k;
  }

  // Slots that enter the ring later read the same block.
  for (unsigned n = 0; n != 30; ++n) {
    res_grid_alloc.slot_indication(++sl_tx);
  }
  ASSERT_EQ(res_grid_alloc[0].dl_res_grid.used_crbs(bwp_cfg, {0, 14}).count(), 4U);

  // A new block replaces the old one whole.
  crb_bitmap other(MAX_NOF_PRBS);
  other.set(40);
  res_grid_alloc.set_blocked_rbs(other);
  crb_bitmap used = res_grid_alloc[1].ul_res_grid.used_crbs(bwp_cfg, {0, 14});
  ASSERT_EQ(used.count(), 1U);
  ASSERT_TRUE(used.test(40));

  res_grid_alloc.clear_blocked_rbs();
  ASSERT_TRUE(res_grid_alloc[0].dl_res_grid.used_crbs(bwp_cfg, {0, 14}).none());
  ASSERT_TRUE(res_grid_alloc.blocked_rbs().none());
}
