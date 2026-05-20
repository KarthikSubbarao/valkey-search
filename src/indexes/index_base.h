/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEYSEARCH_SRC_INDEXES_INDEX_BASE_H
#define VALKEYSEARCH_SRC_INDEXES_INDEX_BASE_H

#include <cstddef>
#include <memory>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/index_schema.pb.h"
#include "src/indexes/text/text_iterator.h"
#include "src/rdb_serialization.h"
#include "src/utils/string_interning.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace valkey_search::indexes {
enum class IndexerType { kHNSW, kFlat, kNumeric, kTag, kVector, kNone, kText };

enum class DeletionType {
  kRecord,      // The record was deleted from the index.
  kIdentifier,  // One or more fields of the record were deleted.
  kNone         // No deletion occurred.
};

const absl::NoDestructor<absl::flat_hash_map<absl::string_view, IndexerType>>
    kIndexerTypeByStr({{"VECTOR", IndexerType::kVector},
                       {"TAG", IndexerType::kTag},
                       {"NUMERIC", IndexerType::kNumeric},
                       {"TEXT", IndexerType::kText}});

class IndexBase {
 public:
  explicit IndexBase(IndexerType indexer_type) : indexer_type_(indexer_type) {}
  virtual ~IndexBase() = default;

  // Add/Remove/Modify will return true if the operation was successful, false
  // if it was skipped.  Returns an error status if there is an unexpected
  // failure.
  virtual absl::StatusOr<bool> AddRecord(const InternedStringPtr& key,
                                         absl::string_view data) = 0;
  virtual absl::StatusOr<bool> RemoveRecord(const InternedStringPtr& key,
                                            DeletionType deletion_type) = 0;
  virtual absl::StatusOr<bool> ModifyRecord(const InternedStringPtr& key,
                                            absl::string_view data) = 0;
  virtual int RespondWithInfo(ValkeyModuleCtx* ctx) const = 0;
  IndexerType GetIndexerType() const { return indexer_type_; }
  virtual absl::Status SaveIndex(RDBChunkOutputStream chunked_out) const = 0;

  virtual std::unique_ptr<data_model::Index> ToProto() const = 0;

  virtual size_t GetTrackedKeyCount() const = 0;
  virtual size_t GetUnTrackedKeyCount() const = 0;
  virtual bool IsTracked(const InternedStringPtr& key) const = 0;
  virtual bool IsUnTracked(const InternedStringPtr& key) const = 0;
  virtual void UnTrack(const InternedStringPtr& key) = 0;
  virtual absl::Status ForEachTrackedKey(
      absl::AnyInvocable<absl::Status(const InternedStringPtr&)> fn) const = 0;
  virtual absl::Status ForEachUnTrackedKey(
      absl::AnyInvocable<absl::Status(const InternedStringPtr&)> fn) const = 0;

  virtual vmsdk::UniqueValkeyString NormalizeStringRecord(
      vmsdk::UniqueValkeyString input) const {
    return input;
  }

  /// Returns the mutation weight for this index type
  virtual uint32_t GetMutationWeight() const = 0;

 private:
  IndexerType indexer_type_{IndexerType::kNone};
};

class EntriesFetcherBase;  // forward declaration

class EntriesFetcherIteratorBase : public text::TextIterator {
 public:
  virtual bool Done() const = 0;
  virtual void Next() = 0;
  virtual const InternedStringPtr& operator*() const = 0;

  // TextIterator interface — delegates to Done/Next/operator*.
  bool DoneKeys() const override { return Done(); }
  const InternedStringPtr& CurrentKey() const override { return **this; }
  bool NextKey() override { Next(); return !Done(); }
  bool SeekForwardKey(const InternedStringPtr& target) override = 0;

  // No positions — key-only iteration.
  text::FieldMaskPredicate QueryFieldMask() const override { return ~0ULL; }
  bool DonePositions() const override { return true; }
  const text::PositionRange& CurrentPosition() const override {
    CHECK(false) << "CurrentPosition() called on key-only iterator";
    __builtin_unreachable();
  }
  bool NextPosition() override { return false; }
  bool SeekForwardPosition(text::Position) override { return false; }
  text::FieldMaskPredicate CurrentFieldMask() const override { return ~0ULL; }
  bool HasPositions() const override { return false; }
  bool IsIteratorValid() const override { return !Done(); }

  // Optional: keeps the fetcher alive when iterator outlives its creator.
  std::unique_ptr<EntriesFetcherBase> owned_fetcher_;

  ~EntriesFetcherIteratorBase() override = default;
};

class EntriesFetcherBase {
 public:
  virtual size_t Size() const = 0;
  virtual ~EntriesFetcherBase() = default;
  virtual std::unique_ptr<EntriesFetcherIteratorBase> Begin() = 0;
};

}  // namespace valkey_search::indexes

#endif  // VALKEYSEARCH_SRC_INDEXES_INDEX_BASE_H
