/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEY_SEARCH_INDEXES_TEXT_HYBRID_ITERATOR_H_
#define VALKEY_SEARCH_INDEXES_TEXT_HYBRID_ITERATOR_H_

#include <memory>
#include <queue>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "src/indexes/index_base.h"
#include "src/indexes/text/text_iterator.h"

namespace valkey_search::indexes::text {

// ComposedANDIterator: Intersects text iterator with numeric/tag fetchers
// Returns keys in sorted order, HasPosition() returns false for numeric/tag keys
class ComposedANDIterator : public TextIterator {
 public:
  ComposedANDIterator(
      std::unique_ptr<TextIterator> text_iter,
      std::queue<std::unique_ptr<EntriesFetcherBase>> non_text_fetchers);

  FieldMaskPredicate QueryFieldMask() const override;
  bool DoneKeys() const override;
  const Key& CurrentKey() const override;
  bool NextKey() override;
  bool SeekForwardKey(const Key& target_key) override;
  bool DonePositions() const override;
  const PositionRange& CurrentPosition() const override;
  bool NextPosition() override;
  bool SeekForwardPosition(Position target_position) override;
  FieldMaskPredicate CurrentFieldMask() const override;
  bool IsIteratorValid() const override;
  bool HasPosition() const;

 private:
  void MaterializeNonTextKeys(
      std::queue<std::unique_ptr<EntriesFetcherBase>>& non_text_fetchers);
  bool AdvanceToNextMatch();

  std::unique_ptr<TextIterator> text_iter_;
  absl::flat_hash_set<const char*> non_text_keys_;
  bool current_key_from_text_;
  bool done_;
};

// ComposedORIterator: Unions text iterator with numeric/tag fetchers
// Returns keys in sorted order, HasPosition() returns false for numeric/tag keys
class ComposedORIterator : public TextIterator {
 public:
  ComposedORIterator(
      std::unique_ptr<TextIterator> text_iter,
      std::queue<std::unique_ptr<EntriesFetcherBase>> non_text_fetchers);

  FieldMaskPredicate QueryFieldMask() const override;
  bool DoneKeys() const override;
  const Key& CurrentKey() const override;
  bool NextKey() override;
  bool SeekForwardKey(const Key& target_key) override;
  bool DonePositions() const override;
  const PositionRange& CurrentPosition() const override;
  bool NextPosition() override;
  bool SeekForwardPosition(Position target_position) override;
  FieldMaskPredicate CurrentFieldMask() const override;
  bool IsIteratorValid() const override;
  bool HasPosition() const;

 private:
  void InitializeNonTextIterators(
      std::queue<std::unique_ptr<EntriesFetcherBase>>& non_text_fetchers);
  bool AdvanceToNextKey();
  const Key* PeekNextNonTextKey();

  std::unique_ptr<TextIterator> text_iter_;
  std::vector<std::unique_ptr<EntriesFetcherIteratorBase>> non_text_iters_;
  const Key* current_key_;
  bool current_key_from_text_;
  bool done_;
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_HYBRID_ITERATOR_H_
