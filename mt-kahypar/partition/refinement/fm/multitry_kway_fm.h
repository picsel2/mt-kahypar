/*******************************************************************************
 * This file is part of MT-KaHyPar.
 *
 * Copyright (C) 2020 Lars Gottesbüren <lars.gottesbueren@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#pragma once

#include <tbb/parallel_for_each.h>

#include <mt-kahypar/partition/context.h>
#include <mt-kahypar/utils/timer.h>

#include <external_tools/kahypar/kahypar/partition/metrics.h>

#include "mt-kahypar/partition/refinement/i_refiner.h"
#include "mt-kahypar/partition/refinement/fm/localized_kway_fm_core.h"
#include "mt-kahypar/partition/refinement/fm/global_rollback.h"
#include "mt-kahypar/utils/memory_tree.h"


namespace mt_kahypar {

class MultiTryKWayFM final : public IRefiner {

  static constexpr bool debug = false;

public:
  MultiTryKWayFM(const Hypergraph& hypergraph,
                 const Context& c,
                 const TaskGroupID taskGroupID) :
    initial_num_nodes(hypergraph.initialNumNodes()),
    original_context(c),
    context(c),
    taskGroupID(taskGroupID),
    sharedData(hypergraph.initialNumNodes(), context),
    globalRollback(hypergraph, context, context.partition.k),
    ets_fm([&] {
      return constructLocalizedKWayFMSearch();
    }) {
    if (context.refinement.fm.obey_minimal_parallelism) {
      sharedData.finishedTasksLimit = std::min(8UL, context.shared_memory.num_threads);
    }
  }

  bool refineImpl(PartitionedHypergraph& phg, kahypar::Metrics& metrics, double time_limit) final ;

  Gain refine(PartitionedHypergraph& phg, const kahypar::Metrics& metrics, double time_limit);

  void initializeImpl(PartitionedHypergraph& phg) final ;

  void roundInitialization(PartitionedHypergraph& phg);


  LocalizedKWayFM constructLocalizedKWayFMSearch() {
    return LocalizedKWayFM(context, initial_num_nodes, sharedData.vertexPQHandles.data());
  }

  static double improvementFraction(Gain gain, HyperedgeWeight old_km1) {
    if (old_km1 == 0)
      return 0;
    else
      return static_cast<double>(gain) / static_cast<double>(old_km1);
  }

  void printMemoryConsumption();

  bool is_initialized = false;
  bool enable_light_fm = false;
  const HypernodeID initial_num_nodes;
  const Context& original_context;
  Context context;
  const TaskGroupID taskGroupID;
  FMSharedData sharedData;
  GlobalRollback globalRollback;
  tbb::enumerable_thread_specific<LocalizedKWayFM> ets_fm;
  size_t peak_reinsertions = 0;
};

}
