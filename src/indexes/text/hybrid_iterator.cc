/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/text/hybrid_iterator.h"

#include "absl/log/check.h"

namespace valkey_search::indexes::text {

// ComposedANDIterator implementation
ComposedANDIterator::ComposedANDIterator(
    std::unique_ptr<TextIterator> text_iter,
    std::queue<std::unique_ptr<EntriesFetcherBase>> non_text_fetchers)
    : text_iter_(std::move(text_iter)),
      current_key_from_text_(false),
      done_(false) {
  MaterializeNonTextKeys(non_text_fetchers);
  if (!text_iter_->DoneKeys()) {
    AdvanceToNextMatch();
  } else {
    done_ = true;
  }
}

void ComposedANDIterator::MaterializeNonTextKeys(
    std::queue<std::unique_ptr<EntriesFetcherBase>>& non_text_fetchers) {
  while (!non_text_fetchers.empty()) {
    auto fetcher = std::move(non_text_fetchers.front());
    non_text_fetchers.pop();
    auto iter = fetcher->Begin();
    while (!iter->Done()) {
      non_text_keys_.insert((**iter)->Str().data());
      iter->Next();
    }
  }
}

bool ComposedANDIterator::AdvanceToNextMatch() {
  while (!text_iter_->DoneKeys()) {
    const auto& key = text_iter_->CurrentKey();
    if (non_text_keys_.contains(key->Str().data())) {
      current_key_from_text_ = true;
      return true;
    }
    if (!text_iter_->NextKey()) {
      done_ = true;
      return false;
    }
  }
  done_ = true;
  return false;
}

FieldMaskPredicate ComposedANDIterator::QueryFieldMask() const {
  return text_iter_->QueryFieldMask();
}

bool ComposedANDIterator::DoneKeys() const { return done_; }

const Key& ComposedANDIterator::CurrentKey() const {
  CHECK(!done_);
  return text_iter_->CurrentKey();
}

bool ComposedANDIterator::NextKey() {
  CHECK(!done_);
  if (!text_iter_->NextKey()) {
    done_ = true;
    return false;
  }
  return AdvanceToNextMatch();
}

bool ComposedANDIterator::SeekForwardKey(const Key& target_key) {
  CHECK(!done_);
  if (!text_iter_->SeekForwardKey(target_key)) {
    done_ = true;
    return false;
  }
  return AdvanceToNextMatch();
}

bool ComposedANDIterator::DonePositions() const {
  return done_ || text_iter_->DonePositions();
}

const PositionRange& ComposedANDIterator::CurrentPosition() const {
  CHECK(!done_);
  return text_iter_->CurrentPosition();
}

bool ComposedANDIterator::NextPosition() {
  CHECK(!done_);
  return text_iter_->NextPosition();
}

bool ComposedANDIterator::SeekForwardPosition(Position target_position) {
  CHECK(!done_);
  return text_iter_->SeekForwardPosition(target_position);
}

FieldMaskPredicate ComposedANDIterator::CurrentFieldMask() const {
  CHECK(!done_);
  return text_iter_->CurrentFieldMask();
}

bool ComposedANDIterator::IsIteratorValid() const {
  return !done_ && text_iter_->IsIteratorValid();
}

bool ComposedANDIterator::HasPosition() const {
  return current_key_from_text_;
}

// ComposedORIterator implementation
ComposedORIterator::ComposedORIterator(
    std::unique_ptr<TextIterator> text_iter,
    std::queue<std::unique_ptr<EntriesFetcherBase>> non_text_fetchers)
    : text_iter_(std::move(text_iter)),
      current_key_(nullptr),
      current_key_from_text_(false),
      done_(false) {
  InitializeNonTextIterators(non_text_fetchers);
  AdvanceToNextKey();
}

void ComposedORIterator::InitializeNonTextIterators(
    std::queue<std::unique_ptr<EntriesFetcherBase>>& non_text_fetchers) {
  while (!non_text_fetchers.empty()) {
    auto fetcher = std::move(non_text_fetchers.front());
    non_text_fetchers.pop();
    auto iter = fetcher->Begin();
    if (!iter->Done()) {
      non_text_iters_.push_back(std::move(iter));
    }
  }
}

const Key* ComposedORIterator::PeekNextNonTextKey() {
  const Key* min_key = nullptr;
  for (auto& iter : non_text_iters_) {
    if (!iter->Done()) {
      const auto& key = **iter;
      if (!min_key || key < *min_key) {
        min_key = &key;
      }
    }
  }
  return min_key;
}

bool ComposedORIterator::AdvanceToNextKey() {
  const Key* text_key = text_iter_->DoneKeys() ? nullptr : &text_iter_->CurrentKey();
  const Key* non_text_key = PeekNextNonTextKey();

  if (!text_key && !non_text_key) {
    done_ = true;
    return false;
  }

  if (!non_text_key || (text_key && *text_key < *non_text_key)) {
    current_key_ = text_key;
    current_key_from_text_ = true;
  } else {
    current_key_ = non_text_key;
    current_key_from_text_ = false;
    // Advance all non-text iterators pointing to this key
    for (auto& iter : non_text_iters_) {
      if (!iter->Done() && **iter == *current_key_) {
        iter->Next();
      }
    }
  }
  return true;
}

FieldMaskPredicate ComposedORIterator::QueryFieldMask() const {
  return text_iter_->QueryFieldMask();
}

bool ComposedORIterator::DoneKeys() const { return done_; }

const Key& ComposedORIterator::CurrentKey() const {
  CHECK(!done_);
  return *current_key_;
}

bool ComposedORIterator::NextKey() {
  CHECK(!done_);
  if (current_key_from_text_) {
    text_iter_->NextKey();
  }
  return AdvanceToNextKey();
}

bool ComposedORIterator::SeekForwardKey(const Key& target_key) {
  CHECK(!done_);
  // Seek text iterator
  if (!text_iter_->DoneKeys()) {
    text_iter_->SeekForwardKey(target_key);
  }
  // Seek non-text iterators
  for (auto& iter : non_text_iters_) {
    while (!iter->Done() && **iter < target_key) {
      iter->Next();
    }
  }
  return AdvanceToNextKey();
}

bool ComposedORIterator::DonePositions() const {
  return done_ || !current_key_from_text_ || text_iter_->DonePositions();
}

const PositionRange& ComposedORIterator::CurrentPosition() const {
  CHECK(!done_ && current_key_from_text_);
  return text_iter_->CurrentPosition();
}

bool ComposedORIterator::NextPosition() {
  CHECK(!done_ && current_key_from_text_);
  return text_iter_->NextPosition();
}

bool ComposedORIterator::SeekForwardPosition(Position target_position) {
  CHECK(!done_ && current_key_from_text_);
  return text_iter_->SeekForwardPosition(target_position);
}

FieldMaskPredicate ComposedORIterator::CurrentFieldMask() const {
  CHECK(!done_ && current_key_from_text_);
  return text_iter_->CurrentFieldMask();
}

bool ComposedORIterator::IsIteratorValid() const {
  return !done_ && (!current_key_from_text_ || text_iter_->IsIteratorValid());
}

bool ComposedORIterator::HasPosition() const {
  return current_key_from_text_;
}

}  // namespace valkey_search::indexes::text
