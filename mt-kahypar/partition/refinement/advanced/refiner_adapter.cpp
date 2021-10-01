/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2021 Tobias Heuer <tobias.heuer@kit.edu>
 * Copyright (C) 2021 Lars Gottesbüren <lars.gottesbueren@kit.edu>
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

#include "mt-kahypar/partition/refinement/advanced/refiner_adapter.h"

#include "mt-kahypar/partition/factories.h"

namespace mt_kahypar {

#define NOW std::chrono::high_resolution_clock::now()
#define RUNNING_TIME(X) std::chrono::duration<double>(NOW - X).count();

bool AdvancedRefinerAdapter::registerNewSearch(const SearchID search_id,
                                               const PartitionedHypergraph& phg) {
  bool success = true;
  size_t refiner_idx = INVALID_REFINER_IDX;
  if ( _unused_refiners.try_pop(refiner_idx) ) {
    // Note, search id are usually consecutive starting from 0.
    // However, this function is not called in increasing search id order.
    _search_lock.lock();
    while ( static_cast<size_t>(search_id) >= _active_searches.size() ) {
      _active_searches.push_back(ActiveSearch { INVALID_REFINER_IDX, NOW, 0.0, false });
    }
    _search_lock.unlock();

    if ( !_refiner[refiner_idx] ) {
      // Lazy initialization of refiner
      _refiner[refiner_idx] = initializeRefiner();
    }

    _active_searches[search_id].refiner_idx = refiner_idx;
    _active_searches[search_id].start = NOW;
    _refiner[refiner_idx]->initialize(phg);
    _refiner[refiner_idx]->updateTimeLimit(timeLimit());
  } else {
    success = false;
  }
  return success;
}

MoveSequence AdvancedRefinerAdapter::refine(const SearchID search_id,
                                            const PartitionedHypergraph& phg,
                                            const vec<HypernodeID>& refinement_nodes) {
  ASSERT(static_cast<size_t>(search_id) < _active_searches.size());
  ASSERT(_active_searches[search_id].refiner_idx != INVALID_REFINER_IDX);

  // Determine number of free threads for current search
  _num_used_threads_lock.lock();
  const size_t num_free_threads = std::min(
    _context.refinement.advanced.num_threads_per_search,
    _context.shared_memory.num_threads - _num_used_threads.load(std::memory_order_relaxed));
  _num_used_threads += num_free_threads;
  _num_used_threads_lock.unlock();

  // Perform refinement
  ASSERT(num_free_threads > 0);
  const size_t refiner_idx = _active_searches[search_id].refiner_idx;
  _refiner[refiner_idx]->setNumThreadsForSearch(num_free_threads);
  MoveSequence moves = _refiner[refiner_idx]->refine(
    phg, refinement_nodes, _active_searches[search_id].start);
  _num_used_threads -= num_free_threads;
  _active_searches[search_id].reaches_time_limit = moves.state == MoveSequenceState::TIME_LIMIT;
  return moves;
}

bool AdvancedRefinerAdapter::isMaximumProblemSizeReached(const SearchID search_id,
                                                         ProblemStats& stats) {
  ASSERT(static_cast<size_t>(search_id) < _active_searches.size());
  ASSERT(_active_searches[search_id].refiner_idx != INVALID_REFINER_IDX);
  const size_t refiner_idx = _active_searches[search_id].refiner_idx;
  return _refiner[refiner_idx]->isMaximumProblemSizeReached(stats);
}

PartitionID AdvancedRefinerAdapter::maxNumberOfBlocks(const SearchID search_id) {
  ASSERT(static_cast<size_t>(search_id) < _active_searches.size());
  ASSERT(_active_searches[search_id].refiner_idx != INVALID_REFINER_IDX);
  const size_t refiner_idx = _active_searches[search_id].refiner_idx;
  return _refiner[refiner_idx]->maxNumberOfBlocksPerSearch();
}

void AdvancedRefinerAdapter::finalizeSearch(const SearchID search_id) {
  ASSERT(static_cast<size_t>(search_id) < _active_searches.size());
  const double running_time = RUNNING_TIME(_active_searches[search_id].start);
  _active_searches[search_id].running_time = running_time;

  //Update average running time
  _search_lock.lock();
  if ( !_active_searches[search_id].reaches_time_limit ) {
    _average_running_time = (running_time + _num_refinements *
      _average_running_time) / static_cast<double>(_num_refinements + 1);
    ++_num_refinements;
  }
  _search_lock.unlock();

  // Search position of refiner associated with the search id
  if ( shouldSetTimeLimit() ) {
    for ( size_t idx = 0; idx < _refiner.size(); ++idx ) {
      if ( _refiner[idx] ) {
        _refiner[idx]->updateTimeLimit(timeLimit());
      }
    }
  }

  ASSERT(_active_searches[search_id].refiner_idx != INVALID_REFINER_IDX);
  _unused_refiners.push(_active_searches[search_id].refiner_idx);
  _active_searches[search_id].refiner_idx = INVALID_REFINER_IDX;
}

void AdvancedRefinerAdapter::reset() {
  _unused_refiners.clear();
  for ( size_t i = 0; i < numAvailableRefiner(); ++i ) {
    _unused_refiners.push(i);
  }
  _active_searches.clear();
  _num_used_threads.store(0, std::memory_order_relaxed);
  _num_refinements = 0;
  _average_running_time = 0.0;
}

std::unique_ptr<IAdvancedRefiner> AdvancedRefinerAdapter::initializeRefiner() {
  return AdvancedRefinementFactory::getInstance().createObject(
    _context.refinement.advanced.algorithm, _hg, _context);
}

}