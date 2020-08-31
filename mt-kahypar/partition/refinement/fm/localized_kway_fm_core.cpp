#include "mt-kahypar/partition/refinement/fm/localized_kway_fm_core.h"

namespace mt_kahypar {

  bool LocalizedKWayFM::findMovesLocalized(PartitionedHypergraph& phg, size_t taskID) {
    localMoves.clear();
    thisSearch = ++sharedData.nodeTracker.highestActiveSearchID;

    const size_t nSeeds = context.refinement.fm.num_seed_nodes;
    HypernodeID seedNode;
    while (runStats.pushes < nSeeds && sharedData.refinementNodes.try_pop(seedNode, taskID)) {
      if (sharedData.nodeTracker.tryAcquireNode(seedNode, thisSearch)) {
        fm_details.insertIntoPQ(phg, seedNode);
      }
    }
    fm_details.updatePQs();

    if (runStats.pushes > 0) {
      if (!context.refinement.fm.perform_moves_global
          && deltaPhg.combinedMemoryConsumption() > sharedData.deltaMemoryLimitPerThread) {
        sharedData.deltaExceededMemoryConstraints = true;
      }

      if (sharedData.deltaExceededMemoryConstraints) {
        deltaPhg.dropMemory();
      }


      deltaPhg.clear();
      deltaPhg.setPartitionedHypergraph(&phg);
      if (context.refinement.fm.perform_moves_global || sharedData.deltaExceededMemoryConstraints) {
        internalFindMoves<false>(phg);
      } else {
        internalFindMoves<true>(phg);
      }
      return true;
    } else {
      return false;
    }

  }

  bool LocalizedKWayFM::findMovesUsingFullBoundary(PartitionedHypergraph& phg) {
    localMoves.clear();
    thisSearch = ++sharedData.nodeTracker.highestActiveSearchID;

    for (HypernodeID u : sharedData.refinementNodes.safely_inserted_range()) {
      if (sharedData.nodeTracker.tryAcquireNode(u, thisSearch)) {
        fm_details.insertIntoPQ(phg, u);
      }
    }
    fm_details.updatePQs();

    internalFindMoves<true>(phg);
    return true;
  }


  template<typename Partition>
  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE std::pair<PartitionID, HypernodeWeight> heaviestPartAndWeight(const Partition& partition) {
    PartitionID p = kInvalidPartition;
    HypernodeWeight w = std::numeric_limits<HypernodeWeight>::min();
    for (PartitionID i = 0; i < partition.k(); ++i) {
      if (partition.partWeight(i) > w) {
        w = partition.partWeight(i);
        p = i;
      }
    }
    return std::make_pair(p, w);
  }

/*
  void LocalizedKWayFM:: internalFindMovesOnDeltaHypergraph(PartitionedHypergraph& phg,
                                                            FMSharedData& sharedData) {
    StopRule stopRule(phg.initialNumNodes());
    Move move;

    auto hes_to_update_func = [&](const HyperedgeID he,
                                  const HyperedgeWeight,
                                  const HypernodeID,
                                  const HypernodeID pin_count_in_from_part_after,
                                  const HypernodeID pin_count_in_to_part_after) {
      // Gains of the pins of a hyperedge can only change in the following situation.
      // In such cases, we mark the hyperedge as invalid and update the gain of all
      // pins afterwards.
      if ( pin_count_in_from_part_after == 0 || pin_count_in_from_part_after == 1 ||
           pin_count_in_to_part_after == 1 || pin_count_in_to_part_after == 2 ) {
        edgesWithGainChanges.push_back(he);
      }
    };

    size_t bestImprovementIndex = 0;
    Gain estimatedImprovement = 0;
    Gain bestImprovement = 0;

    HypernodeWeight heaviestPartWeight = 0;
    HypernodeWeight fromWeight = 0, toWeight = 0;

    while (!stopRule.searchShouldStop()
           && sharedData.finishedTasks.load(std::memory_order_relaxed) < sharedData.finishedTasksLimit
           && findNextMove(deltaPhg, move)) {
      sharedData.nodeTracker.deactivateNode(move.node, thisSearch);

      bool moved = false;
      if (move.to != kInvalidPartition) {
        heaviestPartWeight = heaviestPartAndWeight(deltaPhg).second;
        fromWeight = deltaPhg.partWeight(move.from);
        toWeight = deltaPhg.partWeight(move.to);
        moved = deltaPhg.changeNodePart(move.node, move.from, move.to,
                                        context.partition.max_part_weights[move.to], hes_to_update_func);
      }

      if (moved) {
        runStats.moves++;
        estimatedImprovement += move.gain;
        localData.localMoves.push_back(move);
        stopRule.update(move.gain);
        const bool improved_km1 = estimatedImprovement > bestImprovement;
        const bool improved_balance_less_equal_km1 = estimatedImprovement >= bestImprovement
                                                     && fromWeight == heaviestPartWeight
                                                     && toWeight + phg.nodeWeight(move.node) < heaviestPartWeight;

        if (improved_km1 || improved_balance_less_equal_km1) {
          stopRule.reset();
          bestImprovement = estimatedImprovement;
          bestImprovementIndex = localData.localMoves.size();
        }

        acquireOrUpdateNeighbors(deltaPhg, sharedData, move);
      }

      fm_details.updatePQs();
    }

    std::tie(bestImprovement, bestImprovementIndex) =
            applyMovesOnGlobalHypergraph(phg, sharedData, bestImprovementIndex, bestImprovement);
    runStats.estimated_improvement = bestImprovement;
    fm_details.clearPQs(sharedData, bestImprovementIndex);
    runStats.merge(stats);
  }
*/


  //template<typename FMDetails>
  template<bool use_delta>
  void LocalizedKWayFM//<FMDetails>
    ::internalFindMoves(PartitionedHypergraph& phg) {
    StopRule stopRule(phg.initialNumNodes());
    Move move;

    auto delta_func = [&](const HyperedgeID he,
                          const HyperedgeWeight edge_weight,
                          const HypernodeID ,
                          const HypernodeID pin_count_in_from_part_after,
                          const HypernodeID pin_count_in_to_part_after) {
      // Gains of the pins of a hyperedge can only change in the following situations.
      if ( pin_count_in_from_part_after == 0 || pin_count_in_from_part_after == 1 ||
           pin_count_in_to_part_after == 1 || pin_count_in_to_part_after == 2 ) {
        edgesWithGainChanges.push_back(he);
      }

      // TODO we can almost make this function take a generic partitioned hypergraph
      // we would have to add the success func to the interface of DeltaPhg (and then ignore it there...)
      // and do the local rollback outside this function

      if constexpr (use_delta) {
        fm_details.deltaGainUpdates(deltaPhg, he, edge_weight, move.from, pin_count_in_from_part_after,
                                    move.to, pin_count_in_to_part_after);
      } else {
        fm_details.deltaGainUpdates(phg, he, edge_weight, move.from, pin_count_in_from_part_after,
                                    move.to, pin_count_in_to_part_after);
      }

    };

    size_t bestImprovementIndex = 0;
    Gain estimatedImprovement = 0;
    Gain bestImprovement = 0;

    HypernodeWeight heaviestPartWeight = 0;
    HypernodeWeight fromWeight = 0, toWeight = 0;

    while (!stopRule.searchShouldStop()
           && sharedData.finishedTasks.load(std::memory_order_relaxed) < sharedData.finishedTasksLimit) {

      if constexpr (use_delta) {
        if (!fm_details.findNextMove(deltaPhg, move)) break;
      } else {
        if (!fm_details.findNextMove(phg, move)) break;
      }

      sharedData.nodeTracker.deactivateNode(move.node, thisSearch);
      MoveID move_id = std::numeric_limits<MoveID>::max();
      auto report_success = [&] { move_id = sharedData.moveTracker.insertMove(move); };


      /**
       * exchangable parts:
       *        -- findNextMove
       *        -- acquireOrUpdateNode
       *        -- changeNodePart: with gain updates and without
       *
       */

      bool moved = false;
      if (move.to != kInvalidPartition) {

        if constexpr (use_delta) {
          heaviestPartWeight = heaviestPartAndWeight(deltaPhg).second;
          fromWeight = deltaPhg.partWeight(move.from);
          toWeight = deltaPhg.partWeight(move.to);

          moved = deltaPhg.changeNodePart(move.node, move.from, move.to,
                                          context.partition.max_part_weights[move.to], delta_func);
        } else {
          heaviestPartWeight = heaviestPartAndWeight(phg).second;
          fromWeight = phg.partWeight(move.from);
          toWeight = phg.partWeight(move.to);

          moved = phg.changeNodePart(move.node, move.from, move.to,
                                     context.partition.max_part_weights[move.to],
                                     report_success, delta_func);
        }

      }

      if (moved) {
        runStats.moves++;
        estimatedImprovement += move.gain;
        localMoves.emplace_back(move, move_id);
        stopRule.update(move.gain);
        const bool improved_km1 = estimatedImprovement > bestImprovement;
        const bool improved_balance_less_equal_km1 = estimatedImprovement >= bestImprovement
                                                     && fromWeight == heaviestPartWeight
                                                     && toWeight + phg.nodeWeight(move.node) < heaviestPartWeight;

        if (improved_km1 || improved_balance_less_equal_km1) {
          stopRule.reset();
          bestImprovement = estimatedImprovement;
          bestImprovementIndex = localMoves.size();
        }

        if constexpr (use_delta) {
          acquireOrUpdateNeighbors(deltaPhg, move);
        } else {
          acquireOrUpdateNeighbors(phg, move);
        }
      }

      fm_details.updatePQs();
    }

    if constexpr (use_delta) {
      std::tie(bestImprovement, bestImprovementIndex) =
              applyMovesOnGlobalHypergraph(phg, bestImprovementIndex, bestImprovement);
    } else {
      revertToBestLocalPrefix(phg, bestImprovementIndex);
    }

    runStats.estimated_improvement = bestImprovement;
    fm_details.clearPQs(bestImprovementIndex);
    runStats.merge(stats);
  }


  std::pair<Gain, size_t> LocalizedKWayFM::applyMovesOnGlobalHypergraph(
                                                        PartitionedHypergraph& phg,
                                                        const size_t bestGainIndex,
                                                        const Gain bestEstimatedImprovement) {
    // TODO find better variable names!

    Gain estimatedImprovement = 0;
    Gain lastGain = 0;

    auto delta_gain_func = [&](const HyperedgeID he,
                               const HyperedgeWeight edge_weight,
                               const HypernodeID edge_size,
                               const HypernodeID pin_count_in_from_part_after,
                               const HypernodeID pin_count_in_to_part_after) {
      lastGain += km1Delta(he, edge_weight, edge_size,
                           pin_count_in_from_part_after, pin_count_in_to_part_after);
    };

    // Apply move sequence to original hypergraph and update gain values
    Gain bestImprovement = 0;
    size_t bestIndex = 0;
    for (size_t i = 0; i < bestGainIndex; ++i) {
      Move& local_move = localMoves[i].first;
      MoveID& move_id = localMoves[i].second;
      lastGain = 0;

      if constexpr (FMDetails::uses_gain_cache) {
        phg.changeNodePartWithGainCacheUpdate(local_move.node, local_move.from, local_move.to,
                                              std::numeric_limits<HypernodeWeight>::max(),
                                              [&] { move_id = sharedData.moveTracker.insertMove(local_move); },
                                              delta_gain_func);
      } else {
        phg.changeNodePart(local_move.node, local_move.from, local_move.to,
                           std::numeric_limits<HypernodeWeight>::max(),
                           [&]{ move_id = sharedData.moveTracker.insertMove(local_move); },
                           delta_gain_func);
      }

      lastGain = -lastGain; // delta func yields negative sum of improvements, i.e. negative values mean improvements
      estimatedImprovement += lastGain;
      ASSERT(move_id != std::numeric_limits<MoveID>::max());
      Move& global_move = sharedData.moveTracker.getMove(move_id);
      global_move.gain = lastGain; // Update gain value based on hypergraph delta
      if (estimatedImprovement >= bestImprovement) {  // TODO also incorporate balance into this?
        bestImprovement = estimatedImprovement;
        bestIndex = i;
      }
    }

    runStats.local_reverts += localMoves.size() - bestGainIndex;
    if (bestIndex != bestGainIndex) {
      runStats.best_prefix_mismatch++;
    }

    // Kind of double rollback, if gain values are not correct
    // TODO be more aggressive about it, i.e., really just go back to bestIndex every time?
    if ( estimatedImprovement < 0 ) {
      runStats.local_reverts += bestGainIndex - bestIndex + 1;
      for ( size_t i = bestIndex + 1; i < bestGainIndex; ++i ) {
        Move& m = sharedData.moveTracker.getMove(localMoves[i].second);

        if constexpr (FMDetails::uses_gain_cache) {
          phg.changeNodePartWithGainCacheUpdate(m.node, m.to, m.from);
        } else {
          phg.changeNodePart(m.node, m.to, m.from);
        }

        sharedData.moveTracker.invalidateMove(m);
      }
      return std::make_pair(bestImprovement, bestIndex);
    } else {
      return std::make_pair(bestEstimatedImprovement, bestGainIndex);
    }
  }

  void LocalizedKWayFM::revertToBestLocalPrefix(PartitionedHypergraph& phg, size_t bestGainIndex) {
    runStats.local_reverts += localMoves.size() - bestGainIndex;
    while (localMoves.size() > bestGainIndex) {
      Move& m = sharedData.moveTracker.getMove(localMoves.back().second);
      if constexpr (FMDetails::uses_gain_cache) {
        phg.changeNodePartWithGainCacheUpdate(m.node, m.to, m.from);
      } else {
        phg.changeNodePart(m.node, m.to, m.from);
      }
      sharedData.moveTracker.invalidateMove(m);
      localMoves.pop_back();
    }
  }

  void LocalizedKWayFM::memoryConsumption(utils::MemoryTreeNode* parent) const {
    ASSERT(parent);

    utils::MemoryTreeNode* localized_fm_node = parent->addChild("Localized k-Way FM");

    utils::MemoryTreeNode* deduplicator_node = localized_fm_node->addChild("Deduplicator");
    deduplicator_node->updateSize(updateDeduplicator.size_in_bytes());
    utils::MemoryTreeNode* edges_to_activate_node = localized_fm_node->addChild("edgesWithGainChanges");
    edges_to_activate_node->updateSize(edgesWithGainChanges.capacity() * sizeof(HyperedgeID));

    utils::MemoryTreeNode* local_moves_node = parent->addChild("Local FM Moves");
    local_moves_node->updateSize(localMoves.capacity() * sizeof(std::pair<Move, MoveID>));

    // TODO fm_details.memoryConsumptiom(..)
    /*
    utils::MemoryTreeNode* block_pq_node = localized_fm_node->addChild("Block PQ");
    block_pq_node->updateSize(blockPQ.size_in_bytes());
    utils::MemoryTreeNode* vertex_pq_node = localized_fm_node->addChild("Vertex PQ");
    for ( const VertexPriorityQueue& pq : vertexPQs ) {
      vertex_pq_node->updateSize(pq.size_in_bytes());
    }
     */

    deltaPhg.memoryConsumption(localized_fm_node);
  }

}