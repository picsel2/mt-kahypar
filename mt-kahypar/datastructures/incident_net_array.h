/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
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

#include <stddef.h>

#include "tbb/enumerable_thread_specific.h"
#include "tbb/parallel_invoke.h"
#include "tbb/parallel_scan.h"

#include "kahypar/datastructure/fast_reset_flag_array.h"

#include "mt-kahypar/macros.h"
#include "mt-kahypar/datastructures/hypergraph_common.h"
#include "mt-kahypar/parallel/stl/scalable_vector.h"
#include "mt-kahypar/parallel/stl/scalable_unique_ptr.h"
#include "mt-kahypar/parallel/atomic_wrapper.h"
#include "mt-kahypar/parallel/parallel_prefix_sum.h"
#include "mt-kahypar/utils/range.h"

namespace mt_kahypar {
namespace ds {

// ! Class allows in-place contraction and uncontraction of the incident net array
class IncidentNetArray {

  using HyperedgeVector = parallel::scalable_vector<parallel::scalable_vector<HypernodeID>>;
  using ThreadLocalCounter = tbb::enumerable_thread_specific<parallel::scalable_vector<size_t>>;
  using AtomicCounter = parallel::scalable_vector<parallel::IntegralAtomicWrapper<size_t>>;

  using AcquireLockFunc = std::function<void (const HypernodeID)>;
  using ReleaseLockFunc = std::function<void (const HypernodeID)>;
  #define NOOP_LOCK_FUNC [] (const HypernodeID) { }

  static_assert(sizeof(char) == 1);

  // Represents one incident net of a vertex.
  // A incident net is associated with a version number. Incident nets
  // with a version number greater or equal than the version number in
  // header (see Header -> current_version) are active.
  struct Entry {
    HyperedgeID e;
    HypernodeID version;
  };

  // Header of the incident net list of a vertex. The incident net lists
  // contracted into one vertex are concatenated in a double linked list.
  struct Header {
    explicit Header(const HypernodeID u) :
      prev(u),
      next(u),
      it_prev(u),
      it_next(u),
      tail(u),
      size(0),
      degree(0),
      current_version(0) { }

    // ! Previous incident net list
    HypernodeID prev;
    // ! Next incident net list
    HypernodeID next;
    // ! Previous non-empty incident net list
    HypernodeID it_prev;
    // ! Next non-empty incident net list
    HypernodeID it_next;
    // ! If we append a vertex v to the incident net list of a vertex u, we store
    // ! the previous tail of vertex v, such that we can restore the list of v
    // ! during uncontraction
    HypernodeID tail;
    // ! All incident nets between [0,size) are active
    HypernodeID size;
    // ! Degree of the vertex
    HypernodeID degree;
    // ! Current version of the incident net list
    HypernodeID current_version;
  };

  // Iterator over the incident nets of a vertex u
  class IncidentNetIterator :
    public std::iterator<std::forward_iterator_tag,    // iterator_category
                         HyperedgeID,   // value_type
                         std::ptrdiff_t,   // difference_type
                         const HyperedgeID*,   // pointer
                         HyperedgeID> {   // reference
   public:
    IncidentNetIterator(const HypernodeID u,
                        const IncidentNetArray* incident_net_array,
                        const bool end) :
      _u(u),
      _current_u(u),
      _last_u(incident_net_array->header(u)->it_prev),
      _current_pos(0),
      _incident_net_array(incident_net_array) {
      if ( end ) {
        _current_u = _last_u;
        _current_pos = incident_net_array->header(_current_u)->size;
      }

      if ( !end && _current_pos == _incident_net_array->header(_current_u)->size ) {
        next_iterator();
      }
    }

    HyperedgeID operator* () const {
      ASSERT(_current_u != _last_u || _current_pos != _incident_net_array->header(_current_u)->size);
      return _incident_net_array->firstEntry(_current_u)[_current_pos].e;
    }

    IncidentNetIterator & operator++ () {
      ASSERT(_current_u != _last_u || _current_pos != _incident_net_array->header(_current_u)->size);
      ++_current_pos;
      if ( _current_pos == _incident_net_array->header(_current_u)->size) {
        next_iterator();
      }
      return *this;
    }

    IncidentNetIterator operator++ (int) {
      IncidentNetIterator copy = *this;
      operator++ ();
      return copy;
    }

    bool operator!= (const IncidentNetIterator& rhs) {
      return _u != rhs._u || _current_u != rhs._current_u ||
             _current_pos != rhs._current_pos;
    }

    bool operator== (const IncidentNetIterator& rhs) {
      return _u == rhs._u && _current_u == rhs._current_u &&
             _current_pos == rhs._current_pos;
    }

   private:
    void next_iterator() {
      while ( _current_pos == _incident_net_array->header(_current_u)->size ) {
        if ( _current_u == _last_u ) {
          break;
        }
        _current_u = _incident_net_array->header(_current_u)->it_next;
        _current_pos = 0;
      }
    }

    HypernodeID _u;
    HypernodeID _current_u;
    HypernodeID _last_u;
    size_t _current_pos;
    const IncidentNetArray* _incident_net_array;
  };

 public:
  IncidentNetArray(AcquireLockFunc acquire_lock = NOOP_LOCK_FUNC,
                   ReleaseLockFunc release_lock = NOOP_LOCK_FUNC) :
    _num_hypernodes(0),
    _index_array(),
    _incident_net_array(nullptr),
    _acquire_lock(acquire_lock),
    _release_lock(release_lock) { }

  IncidentNetArray(const HypernodeID num_hypernodes,
                   const HyperedgeVector& edge_vector,
                   AcquireLockFunc acquire_lock = NOOP_LOCK_FUNC,
                   ReleaseLockFunc release_lock = NOOP_LOCK_FUNC) :
    _num_hypernodes(num_hypernodes),
    _index_array(),
    _incident_net_array(nullptr),
    _acquire_lock(acquire_lock),
    _release_lock(release_lock)  {
    construct(edge_vector);
  }

  // ! Degree of the vertex
  HypernodeID nodeDegree(const HypernodeID u) const {
    ASSERT(u < _num_hypernodes, "Hypernode" << u << "does not exist");
    return header(u)->degree;
  }

  // ! Returns a range to loop over the incident nets of hypernode u.
  IteratorRange<IncidentNetIterator> incidentEdges(const HypernodeID u) const {
    ASSERT(u < _num_hypernodes, "Hypernode" << u << "does not exist");
    return IteratorRange<IncidentNetIterator>(
      IncidentNetIterator(u, this, false),
      IncidentNetIterator(u, this, true));
  }

  // ! Contracts two incident list of u and v, whereby u is the representative and
  // ! v the contraction partner of the contraction. The contraction involves to remove
  // ! all incident nets shared between u and v from the incident net list of v and append
  // ! the list of v to u.
  void contract(const HypernodeID u,
                const HypernodeID v,
                const kahypar::ds::FastResetFlagArray<>& shared_hes_of_u_and_v) {
    HypernodeID current_v = v;
    Header* head_v = header(v);
    do {
      Header* head = header(current_v);
      const HypernodeID new_version = ++head->current_version;
      Entry* last_entry = lastEntry(current_v);
      for ( Entry* current_entry = firstEntry(current_v); current_entry != last_entry; ++current_entry ) {
        if ( shared_hes_of_u_and_v[current_entry->e] ) {
          // Hyperedge is shared between u and v => decrement size of incident net list
          swap(current_entry--, --last_entry);
          ASSERT(head->size > 0);
          --head->size;
          --head_v->degree;
        } else {
          // Hyperedge is non-shared between u and v => adapt version number of current incident net
          current_entry->version = new_version;
        }
      }

      if ( head->size == 0 && current_v != v ) {
        // Current list becomes empty => remove it from the iterator double linked list
        // such that iteration over the incident nets is more efficient
        removeEmptyIncidentNetList(current_v);
      }
      current_v = head->next;
    } while ( current_v != v );

    _acquire_lock(u);
    // Concatenate double-linked list of u and v
    append(u, v);
    header(u)->degree += head_v->degree;
    _release_lock(u);
  }

  // ! Uncontract two previously contracted vertices u and v.
  // ! Uncontraction involves to decrement the version number of all incident lists contained
  // ! in v and restore all incident nets with a version number equal to the new version.
  // ! Note, uncontraction must be done in relative contraction order
  void uncontract(const HypernodeID u,
                  const HypernodeID v) {
    ASSERT(header(v)->prev != v);
    Header* head_v = header(v);
    _acquire_lock(u);
    // Restores the incident list of v to the time before it was appended
    // to the double-linked list of u.
    splice(v);
    header(u)->degree -= head_v->degree;
    _release_lock(u);

    HypernodeID current_v = v;
    HypernodeID last_non_empty_entry = kInvalidHypernode;
    do {
      Header* head = header(current_v);
      ASSERT(head->current_version > 0);
      const HypernodeID new_version = --head->current_version;
      const Entry* last_entry = reinterpret_cast<const Entry*>(header(current_v + 1));
      // Iterate over non-active entries (and activate them) until the version number
      // is not equal to the new version of the list
      for ( Entry* current_entry = lastEntry(current_v); current_entry != last_entry; ++current_entry ) {
        if ( current_entry->version == new_version ) {
          ++head->size;
          ++head_v->degree;
        } else {
          break;
        }
      }

      // Restore iterator double-linked list which only contains
      // non-empty incident net lists
      if ( head->size > 0 || current_v == v ) {
        if ( last_non_empty_entry != kInvalidHypernode &&
            head->it_prev != last_non_empty_entry ) {
          header(last_non_empty_entry)->it_next = current_v;
          head->it_prev = last_non_empty_entry;
        }
        last_non_empty_entry = current_v;
      }
      current_v = head->next;
    } while ( current_v != v );

    ASSERT(last_non_empty_entry != kInvalidHypernode);
    header(v)->it_prev = last_non_empty_entry;
    header(last_non_empty_entry)->it_next = v;
  }

  // ! Removes all incidents nets of u flagged in hes_to_remove.
  void removeIncidentNets(const HypernodeID u,
                          const kahypar::ds::FastResetFlagArray<>& hes_to_remove) {
    HypernodeID current_u = u;
    Header* head_u = header(u);
    do {
      Header* head = header(current_u);
      const HypernodeID new_version = ++head->current_version;
      Entry* last_entry = lastEntry(current_u);
      for ( Entry* current_entry = firstEntry(current_u); current_entry != last_entry; ++current_entry ) {
        if ( hes_to_remove[current_entry->e] ) {
          // Hyperedge should be removed => decrement size of incident net list
          swap(current_entry--, --last_entry);
          ASSERT(head->size > 0);
          --head->size;
          --head_u->degree;
        } else {
          // Vertex is non-shared between u and v => adapt version number of current incident net
          current_entry->version = new_version;
        }
      }

      if ( head->size == 0 && current_u != u ) {
        // Current list becomes empty => remove it from the iterator double linked list
        // such that iteration over the incident nets is more efficient
        removeEmptyIncidentNetList(current_u);
      }
      current_u = head->next;
    } while ( current_u != u );
  }

  // ! Restores all previously removed incident nets
  // ! Note, function must be called in reverse order of calls to
  // ! removeIncidentNets(...) and all uncontraction that happens
  // ! between two consecutive calls to removeIncidentNets(...) must
  // ! be processed.
  void restoreIncidentNets(const HypernodeID u) {
    Header* head_u = header(u);
    HypernodeID current_u = u;
    HypernodeID last_non_empty_entry = u;
    do {
      Header* head = header(current_u);
      ASSERT(head->current_version > 0);
      const HypernodeID new_version = --head->current_version;
      const Entry* last_entry = reinterpret_cast<const Entry*>(header(current_u + 1));
      // Iterate over non-active entries (and activate them) until the version number
      // is not equal to the new version of the list
      for ( Entry* current_entry = lastEntry(current_u); current_entry != last_entry; ++current_entry ) {
        if ( current_entry->version == new_version ) {
          ++head->size;
          ++head_u->degree;
        } else {
          break;
        }
      }

      // Restore iterator double-linked list which only contains
      // non-empty incident net lists
      if ( head->size > 0 && current_u != u ) {
        if ( head->it_prev != last_non_empty_entry ) {
          header(last_non_empty_entry)->it_next = current_u;
          head->it_prev = last_non_empty_entry;
        }
        last_non_empty_entry = current_u;
      }
      current_u = head->next;
    } while ( current_u != u );

    if ( last_non_empty_entry == header(last_non_empty_entry)->it_next ) {
      header(last_non_empty_entry)->it_next = u;
      head_u->it_prev = last_non_empty_entry;
    }
  }

 private:
  friend class IncidentNetIterator;

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE const Header* header(const HypernodeID u) const {
    ASSERT(u <= _num_hypernodes, "Hypernode" << u << "does not exist");
    return reinterpret_cast<const Header*>(_incident_net_array.get() + _index_array[u]);
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE Header* header(const HypernodeID u) {
    return const_cast<Header*>(static_cast<const IncidentNetArray&>(*this).header(u));
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE const Entry* firstEntry(const HypernodeID u) const {
    ASSERT(u <= _num_hypernodes, "Hypernode" << u << "does not exist");
    return reinterpret_cast<const Entry*>(_incident_net_array.get() + _index_array[u] + sizeof(Header));
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE Entry* firstEntry(const HypernodeID u) {
    return const_cast<Entry*>(static_cast<const IncidentNetArray&>(*this).firstEntry(u));
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE const Entry* lastEntry(const HypernodeID u) const {
    ASSERT(u <= _num_hypernodes, "Hypernode" << u << "does not exist");
    return reinterpret_cast<const Entry*>(_incident_net_array.get() +
      _index_array[u] + sizeof(Header) + header(u)->size * sizeof(Entry));
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE Entry* lastEntry(const HypernodeID u) {
    return const_cast<Entry*>(static_cast<const IncidentNetArray&>(*this).lastEntry(u));
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE void swap(Entry* lhs, Entry* rhs) {
    Entry tmp_lhs = *lhs;
    *lhs = *rhs;
    *rhs = tmp_lhs;
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE void append(const HypernodeID u, const HypernodeID v) {
    const HypernodeID tail_u = header(u)->prev;
    const HypernodeID tail_v = header(v)->prev;
    header(tail_u)->next = v;
    header(u)->prev = tail_v;
    header(v)->tail = tail_v;
    header(v)->prev = tail_u;
    header(tail_v)->next = u;

    const HypernodeID it_tail_u = header(u)->it_prev;
    const HypernodeID it_tail_v = header(v)->it_prev;
    header(it_tail_u)->it_next = v;
    header(u)->it_prev = it_tail_v;
    header(v)->it_prev = it_tail_u;
    header(it_tail_v)->it_next = u;

    if ( header(v)->size == 0 ) {
      removeEmptyIncidentNetList(v);
    }
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE void splice(const HypernodeID v) {
    // Restore the iterator double-linked list of u such that it does not contain
    // any incident net list of v
    const HypernodeID tail = header(v)->tail;
    HypernodeID non_empty_entry_prev_v = v;
    HypernodeID non_empty_entry_next_tail = tail;
    while ( non_empty_entry_prev_v == v || header(non_empty_entry_prev_v)->size == 0 ) {
      non_empty_entry_prev_v = header(non_empty_entry_prev_v)->prev;
    }
    while ( non_empty_entry_next_tail == tail || header(non_empty_entry_next_tail)->size == 0 ) {
      non_empty_entry_next_tail = header(non_empty_entry_next_tail)->next;
    }
    header(non_empty_entry_prev_v)->it_next = non_empty_entry_next_tail;
    header(non_empty_entry_next_tail)->it_prev = non_empty_entry_prev_v;

    // Cut out incident list of v
    const HypernodeID prev_v = header(v)->prev;
    const HypernodeID next_tail = header(tail)->next;
    header(v)->prev = tail;
    header(tail)->next = v;
    header(next_tail)->prev = prev_v;
    header(prev_v)->next = next_tail;
  }

  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE void removeEmptyIncidentNetList(const HypernodeID u) {
    ASSERT(header(u)->size == 0, V(u) << V(header(u)->size));
    Header* head = header(u);
    header(head->it_prev)->it_next = head->it_next;
    header(head->it_next)->it_prev = head->it_prev;
    head->it_next = u;
    head->it_prev = u;
  }

  void construct(const HyperedgeVector& edge_vector) {
    // Accumulate degree of each vertex thread local
    const HyperedgeID num_hyperedges = edge_vector.size();
    ThreadLocalCounter local_incident_nets_per_vertex(_num_hypernodes + 1, 0);
    AtomicCounter current_incident_net_pos;
    tbb::parallel_invoke([&] {
      tbb::parallel_for(ID(0), num_hyperedges, [&](const size_t pos) {
        parallel::scalable_vector<size_t>& num_incident_nets_per_vertex =
          local_incident_nets_per_vertex.local();
        for ( const HypernodeID& pin : edge_vector[pos] ) {
          ASSERT(pin < _num_hypernodes, V(pin) << V(_num_hypernodes));
          ++num_incident_nets_per_vertex[pin + 1];
        }
      });
    }, [&] {
      _index_array.assign(_num_hypernodes + 1, 0);
      current_incident_net_pos.assign(
        _num_hypernodes, parallel::IntegralAtomicWrapper<size_t>(0));
    });

    // We sum up the number of incident nets per vertex only thread local.
    // To obtain the global number of incident nets per vertex, we iterate
    // over each thread local counter and sum it up.
    bool first_iteration = true;
    for ( const parallel::scalable_vector<size_t>& c : local_incident_nets_per_vertex ) {
      tbb::parallel_for(ID(0), _num_hypernodes + 1, [&](const size_t pos) {
        _index_array[pos] += c[pos] * sizeof(Entry) + (first_iteration ? sizeof(Header) : 0);
      });
      first_iteration = false;
    }

    // Compute start positon of the incident nets of each vertex via a parallel prefix sum
    parallel::TBBPrefixSum<size_t> incident_net_prefix_sum(_index_array);
    tbb::parallel_scan(tbb::blocked_range<size_t>(
            0UL, UI64(_num_hypernodes + 1)), incident_net_prefix_sum);
    const size_t size_in_bytes = incident_net_prefix_sum.total_sum();
    _incident_net_array = parallel::make_unique<char>(size_in_bytes);

    // Insert incident nets into incidence array
    tbb::parallel_for(ID(0), num_hyperedges, [&](const HyperedgeID he) {
      for ( const HypernodeID& pin : edge_vector[he] ) {
        Entry* entry = firstEntry(pin) + current_incident_net_pos[pin]++;
        entry->e = he;
        entry->version = 0;
      }
    });

    // Setup Header of each vertex
    tbb::parallel_for(ID(0), _num_hypernodes, [&](const HypernodeID u) {
      Header* head = header(u);
      head->prev = u;
      head->next = u;
      head->it_prev = u;
      head->it_next = u;
      head->size = current_incident_net_pos[u].load(std::memory_order_relaxed);
      head->degree = head->size;
      head->current_version = 0;
    });
  }

  const HypernodeID _num_hypernodes;
  parallel::scalable_vector<size_t> _index_array;
  parallel::tbb_unique_ptr<char> _incident_net_array;
  AcquireLockFunc _acquire_lock;
  ReleaseLockFunc _release_lock;
};
}  // namespace ds
}  // namespace mt_kahypar