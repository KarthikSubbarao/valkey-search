# Unified Iterator: Fully Resolve Predicates at Iterator Level

**Repo:** `/home/karsubba/perf-valkey-search/valkey-search`  
**Branch:** `perf`  
**Base:** `main` (merge commit 3dd7a9f)

## Summary

Replaces the "pick smallest child + per-key re-evaluation" approach with a unified
iterator tree that fully resolves all predicates (text, tag, numeric, negate, AND, OR)
at the iterator level. Background reader threads no longer re-evaluate predicates
per-key — the iterator emits only keys that satisfy all constraints.

## Why

The old approach (`EvaluateFilterAsPrimary`) picked the smallest predicate child,
iterated its keys, and for each key re-evaluated the remaining predicates via
`PrefilterEvaluator`. This caused:

1. **Lock contention at scale** — per-key predicate evaluation (text lookups, numeric
   range checks, tag set membership) happens under the read-phase lock, serializing
   threads.
2. **Code complexity** — De Morgan's law for negate, `IsUnsolvedQuery`,
   `NeedsDeduplication`, multiple special-case paths, dedup hash sets.
3. **Correctness gap** — De Morgan's for text negate breaks positional relationships
   (slop/inorder constraints disappear when you invert individual terms).

## Design

### Iterator hierarchy

```
TextIterator (pure virtual: DoneKeys, CurrentKey, NextKey, SeekForwardKey, HasPositions, ...)
├── EntriesFetcherIteratorBase (abstract: Done/Next/operator*, delegates to TextIterator methods)
│   ├── Numeric::EntriesFetcherIterator  (sorted + unsorted modes, direct DoneKeys/CurrentKey/NextKey overrides)
│   ├── Tag::EntriesFetcherIterator      (sorted + unsorted modes, direct DoneKeys/CurrentKey/NextKey overrides)
│   └── UniversalSetFetcher::Iterator    (iterates parallel btree_set)
├── TermIterator        (text leaf — has positions from rax postings)
├── ProximityIterator   (AND — intersects children via SeekForwardKey convergence)
├── OrProximityIterator (OR — unions children via min-key merge)
└── ExcludeIterator     (NEGATE — source minus excluded, both sorted)
```

- **`EntriesFetcherIteratorBase` inherits from `TextIterator`** — tag/numeric iterators
  ARE TextIterators directly. No adapter wrapper needed for composition.
- **Direct `DoneKeys`/`CurrentKey`/`NextKey` overrides** on Numeric and Tag — eliminates
  double virtual dispatch through the base class delegation layer.
- **`owned_fetcher_`** member on `EntriesFetcherIteratorBase` keeps the parent
  `EntriesFetcherBase` alive (it owns the data the iterator references).

### Search loop (`SearchNonVectorQuery`)

```cpp
auto [iter, size] = BuildIterator(parameters, predicate, negate, sorted, require_positions);
// Iterate TextIterator directly — no TextFetcher, no IteratorFetcher, no EntriesFetcherBase::Begin()
while (!iterator->DoneKeys()) {
    const auto &key = iterator->CurrentKey();
    neighbors.emplace_back(Neighbor{InternedStringPtr::Borrow(key), 0.0f});
    iterator->NextKey();
}
```

Zero wrapper layers between the search loop and the actual iterator. One virtual call
per method.

### Sorted / unsorted modes (Tag + Numeric)

| Mode | When used | Iteration | Memory |
|------|-----------|-----------|--------|
| `sorted=false` | Standalone queries (single predicate) | Iterates `flat_hash_set` in-place via `++keys_iter_`. Returns `*keys_iter_` directly — zero per-key copies. | Zero allocation |
| `sorted=true` | AND/OR composition (child of ProximityIterator) | Copies keys into vector, sorts once. `CurrentKey()` indexes into vector. `SeekForwardKey` uses `std::lower_bound`. | O(N) vector per predicate side |

The `sorted` flag is passed through `BuildIterator` → `BuildLeafIterator` → `Tag::Search`/`Numeric::Search` → `EntriesFetcher` → `EntriesFetcherIterator`.

### Universal set (for negate)

- **`index_key_info_`** remains `flat_hash_map<Key, IndexKeyInfo>` — O(1) lookups on
  every query result (`PopulateIndexMutationSequenceNumbers`, 10K lookups/query).
- **`sorted_keys_`** (`btree_set<Key>`) added in parallel — sorted iteration for
  `UniversalSetFetcher` (negate queries). ~10-12 bytes/key overhead (~10-12MB for 1M docs).
- Both kept in sync on insert/erase.
- **Why parallel:** changing `index_key_info_` to `btree_map` caused -30% to -80%
  regression on high-match queries due to O(log N) × 10K lookups per query (3-4 cache
  misses per lookup vs 1-2 for flat_hash_map).

### Container types (tag + numeric buckets)

- **`patricia_tree.h`**: `flat_hash_set` (KEPT as original — reverted from btree_set
  which hurt unsorted iteration performance).
- **`numeric.h` buckets**: `flat_hash_set` (KEPT as original — same reason).
- Sorted order for AND composition achieved by copying into a vector and sorting
  per-query, not by changing the storage container.

### require_positions propagation

`BuildIterator` accepts `bool require_positions`:
- Top-level (`EvaluateFilterAsPrimary`): `false`
- AND children: `true` only if parent has slop/inorder
- OR children: inherits from parent

This avoids position tracking overhead (FlatPositionMap reads, PositionIterator
construction) for standalone text queries and non-positional AND compositions.

### `InternedStringPtr::Borrow`

Applied from PR #1043 (borrowed string ptr):
- Sorted vector construction in tag/numeric (`sorted_keys_.push_back(Borrow(key))`)
- `SearchNonVectorQuery` result collection (`neighbors.emplace_back(Borrow(key))`)
- `Materialize()` called once before releasing read lock

Eliminates `DecrementRefCount` from the hot iteration path (was 20-53% of CPU in
flamegraphs).

## Key file changes

| File | Change |
|------|--------|
| `src/indexes/index_base.h` | `EntriesFetcherIteratorBase` inherits `TextIterator`. Adds `owned_fetcher_`, position stubs, delegating `DoneKeys`/`CurrentKey`/`NextKey`. |
| `src/index_schema.h` | `IndexKeyInfoMap` = `flat_hash_map` (unchanged). Added `SortedKeySet` = `btree_set<Key>` + `sorted_keys_` member + `GetSortedKeys()` getter. |
| `src/index_schema.cc` | `sorted_keys_.insert(key)` / `sorted_keys_.erase(key)` alongside `index_key_info_` mutations. |
| `src/indexes/numeric.h/.cc` | `EntriesFetcherIterator`: `sorted` flag, sorted vector path + unsorted `LinearAdvance` path. Direct `DoneKeys`/`CurrentKey`/`NextKey` overrides. `done_` flag replaces `current_key_` for unsorted. `Search()` takes `bool sorted`. `EntriesFetcher` stores+passes `sorted_`. Negate path removed (CHECK). |
| `src/indexes/tag.h/.cc` | Same pattern as numeric. `sorted_keys_` vector with `std::unique` for dedup in sorted mode. Direct overrides. `Search()` takes `bool sorted`. Negate path removed (CHECK). |
| `src/indexes/text/key_only_iterator.h` | `ExcludeIterator` (source minus excluded via `SeekForwardKey`). `KeyOnlyTextIterator` retained only for `UniversalSetFetcher` wrapping. |
| `src/indexes/text/text_iterator.h` | Added `HasPositions()` pure virtual. |
| `src/indexes/text/proximity.cc/.h` | `active_pos_indices_` computed per-key from `HasPositions()`. All children in single `iters_` vector. |
| `src/indexes/text/orproximity.cc/.h` | Dynamic `HasPositions()` (false if non-positional child on current key). Position APIs guarded. |
| `src/indexes/universal_set_fetcher.cc/.h` | Iterates `sorted_keys_` btree_set directly. `SeekForwardKey` via linear scan. |
| `src/query/search.cc` | `BuildIterator` (recursive, 5 params: parameters, predicate, negate, sorted, require_positions). `BuildLeafIterator` returns tag/numeric iterators directly (no `KeyOnlyTextIterator` — they ARE TextIterators). `SearchNonVectorQuery` iterates TextIterator directly. Old code removed: `IsUnsolvedQuery`, `NeedsDeduplication`, `EstimatePredicateSize`, `BuildTextIterator`, per-key `PrefilterEvaluator`, dedup hash sets. |
| `src/utils/patricia_tree.h` | UNCHANGED from main (flat_hash_set). |

## Benchmark results (vs `str` branch — borrowed-ptr baseline)

| Threads | Query | Matches | Delta |
|---------|-------|---------|-------|
| 1 | tag | 10000 | **+154%** |
| 16 | tag | 10000 | **+92%** |
| 1 | tag_numeric | 100 | **+8.5%** |
| 1 | numeric | 10000 | −5% |
| 16 | numeric | 10000 | neutral |
| 1 | text | 10000 | neutral |
| 16 | text | 10000 | −7% |
| 1 | tag_numeric | 10000 | **−17%** |
| 16 | tag_numeric | 10000 | **−19%** |

### Why tag_numeric-10K regresses

Old code: iterates 10K tag keys, for each does O(1) numeric range check on the
document's stored value. Total: O(N). Never touches the numeric index's key set.

New code: copies 10K tag keys into vector + sorts. Copies 10K numeric keys into
vector + sorts. Intersects via SeekForwardKey convergence. Total: O(N log N) + O(N log N).

The sort dominates (38-44% of flamegraph for this query). This is the architectural
cost of unified intersection. In practice, most AND queries have one selective side
(100 matches) and one broad side — the tag_numeric-100 case shows +8-12% improvement.

### Why tag-10K improves dramatically

Old code: tag iterator with per-key negate evaluation, dedup logic, lock held during
evaluation. New code: flat_hash_set linear scan with zero per-key overhead (no
`current_key_` copy, no refcount, reference returned directly). Reduced lock hold time
→ reduced thread contention at 16T.

## Testing

- 41/41 fulltext integration tests pass
- 9/9 non-vector integration tests pass (includes mixed predicates, negate at scale)
- Unit tests updated (search_test.cc mock SeekForwardKey, size checks)
- Pausepoint `search_entries_fetcher` retained for cancel tests

## Known limitations / follow-up work

1. **AND with large equal-sized result sets** — O(N log N) sort per side. Could be
   improved with pre-sorted storage (btree_set per bucket) at the cost of unsorted
   iteration speed, or a hybrid probe approach for non-text AND children.
2. **UniversalSetFetcher still wrapped in `KeyOnlyTextIterator`** — could be eliminated
   by giving UniversalSetFetcher's iterator the same direct TextIterator inheritance.
3. **`require_positions` for OR children** — currently inherits from parent context.
   Could be optimized further for deeply nested queries.
