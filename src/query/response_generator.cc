/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/query/response_generator.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/attribute_data_type.h"
#include "src/commands/ft_search_parser.h"
#include "src/indexes/tag.h"
#include "src/indexes/text.h"
#include "src/indexes/text/text_index.h"
#include "src/indexes/vector_base.h"
#include "src/metrics.h"
#include "src/query/predicate.h"
#include "src/query/search.h"
#include "src/valkey_search.h"
#include "vmsdk/src/info.h"
#include "vmsdk/src/log.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/module_config.h"
#include "vmsdk/src/status/status_macros.h"
#include "vmsdk/src/type_conversions.h"
#include "vmsdk/src/utils.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace valkey_search::options {

constexpr absl::string_view kMaxSearchResultRecordSizeConfig{
    "max-search-result-record-size"};
constexpr int kMaxSearchResultRecordSize{10 * 1024 * 1024};  // 10MB
constexpr int kMinSearchResultRecordSize{100};
constexpr absl::string_view kMaxSearchResultFieldsCountConfig{
    "max-search-result-fields-count"};
constexpr int kMaxSearchResultFieldsCount{1000};

/// Register the "--max-search-result-record-size" flag. Controls the max
/// content size for a record in the search response
static auto max_search_result_record_size =
    vmsdk::config::NumberBuilder(
        kMaxSearchResultRecordSizeConfig,  // name
        kMaxSearchResultRecordSize / 2,    // default size
        kMinSearchResultRecordSize,        // min size
        kMaxSearchResultRecordSize)        // max size
        .WithValidationCallback(CHECK_RANGE(kMinSearchResultRecordSize,
                                            kMaxSearchResultRecordSize,
                                            kMaxSearchResultRecordSizeConfig))
        .Build();

/// Register the "--max-search-result-fields-count" flag. Controls the max
/// number of fields in the content of the search response
static auto max_search_result_fields_count =
    vmsdk::config::NumberBuilder(
        kMaxSearchResultFieldsCountConfig,  // name
        kMaxSearchResultFieldsCount / 2,    // default size
        1,                                  // min size
        kMaxSearchResultFieldsCount)        // max size
        .WithValidationCallback(CHECK_RANGE(1, kMaxSearchResultFieldsCount,
                                            kMaxSearchResultFieldsCountConfig))
        .Build();

vmsdk::config::Number &GetMaxSearchResultRecordSize() {
  return dynamic_cast<vmsdk::config::Number &>(*max_search_result_record_size);
}
vmsdk::config::Number &GetMaxSearchResultFieldsCount() {
  return dynamic_cast<vmsdk::config::Number &>(*max_search_result_fields_count);
}

}  // namespace valkey_search::options

namespace valkey_search::query {

class PredicateEvaluator : public query::Evaluator {
 public:
  PredicateEvaluator(const RecordsMap &records,
                     QueryOperations query_operations)
      : Evaluator(query_operations), records_(records), text_index_(nullptr) {}

  PredicateEvaluator(const RecordsMap &records,
                     const valkey_search::indexes::text::TextIndex *text_index,
                     InternedStringPtr target_key,
                     QueryOperations query_operations)
      : Evaluator(query_operations),
        records_(records),
        text_index_(text_index),
        target_key_(target_key) {}

  const InternedStringPtr &GetTargetKey() const override { return target_key_; }

  EvaluationResult EvaluateTags(const query::TagPredicate &predicate) override {
    auto identifier = predicate.GetRetainedIdentifier();
    auto it = records_.find(vmsdk::ToStringView(identifier.get()));
    if (it == records_.end()) {
      return EvaluationResult(false);
      ;
    }
    auto index = predicate.GetIndex();
    // Parsing RECORD DATA: Field value from database key for post-query
    // verification. Uses schema-defined separator since this is record data,
    // not query syntax.
    auto tags = indexes::Tag::ParseSearchTags(
        vmsdk::ToStringView(it->second.value.get()), index->GetSeparator());
    if (!tags.ok()) {
      return EvaluationResult(false);
      ;
    }
    return predicate.Evaluate(&tags.value(), index->IsCaseSensitive());
  }

  EvaluationResult EvaluateNumeric(
      const query::NumericPredicate &predicate) override {
    auto identifier = predicate.GetRetainedIdentifier();
    auto it = records_.find(vmsdk::ToStringView(identifier.get()));
    if (it == records_.end()) {
      return EvaluationResult(false);
    }
    auto out_numeric =
        vmsdk::To<double>(vmsdk::ToStringView(it->second.value.get()));
    if (!out_numeric.ok()) {
      return EvaluationResult(false);
    }
    return predicate.Evaluate(&out_numeric.value());
  }

  EvaluationResult EvaluateText(const query::TextPredicate &predicate,
                                bool require_positions) override {
    CHECK(target_key_);
    if (!text_index_) {
      return EvaluationResult(false);
    }
    return predicate.Evaluate(*text_index_, target_key_, require_positions);
  }

 private:
  const RecordsMap &records_;
  const valkey_search::indexes::text::TextIndex *text_index_ = nullptr;
  InternedStringPtr target_key_;
};

DEV_INTEGER_COUNTER(query, predicate_revalidation);

bool VerifyFilter(const query::SearchParameters &parameters,
                  const RecordsMap &records, const indexes::Neighbor &n) {
  auto predicate = parameters.filter_parse_results.root_predicate.get();
  if (predicate == nullptr) {
    return true;
  }
  auto db_seq =
      parameters.index_schema->GetDbMutationSequenceNumber(n.external_id);
  if (db_seq == n.sequence_number) {
    return true;
  }
  predicate_revalidation.Increment();
  // For text predicates, evaluate using the text index instead of raw data.
  if (parameters.index_schema &&
      parameters.index_schema->GetTextIndexSchema()) {
    const indexes::text::TextIndex *text_index =
        parameters.index_schema->GetTextIndexSchema()->GetPerKeyTextIndex(
            n.external_id, true);

    PredicateEvaluator evaluator(
        records, text_index, n.external_id,
        parameters.filter_parse_results.query_operations);
    EvaluationResult result = predicate->Evaluate(evaluator);
    return result.matches;
  }
  PredicateEvaluator evaluator(
      records, parameters.filter_parse_results.query_operations);
  EvaluationResult result = predicate->Evaluate(evaluator);
  return result.matches;
}

// Check if this node owns the slot for the given key in cluster mode
bool CheckSlotOwnership(ValkeyModuleCtx *ctx, absl::string_view key) {
  // In standalone mode, we own all keys.
  if (!ValkeySearch::Instance().IsCluster()) {
    return true;
  }
  auto cluster_map = ValkeySearch::Instance().GetOrRefreshClusterMap(ctx);
  auto key_str = vmsdk::MakeUniqueValkeyString(key);
  unsigned int slot = ValkeyModule_ClusterKeySlot(key_str.get());
  return cluster_map->IOwnSlot(static_cast<uint16_t>(slot));
}

absl::StatusOr<RecordsMap> GetContentNoReturnJson(
    ValkeyModuleCtx *ctx, const AttributeDataType &attribute_data_type,
    const query::SearchParameters &parameters,
    const indexes::Neighbor &neighbor,
    const std::optional<std::string> &vector_identifier) {
  auto key = neighbor.external_id->Str();
  absl::flat_hash_set<absl::string_view> identifiers;
  identifiers.insert(kJsonRootElementQuery);
  for (const auto &filter_identifier :
       parameters.filter_parse_results.filter_identifiers) {
    identifiers.insert(filter_identifier);
  }
  vmsdk::ValkeySelectDbGuard select_db_guard(ctx, parameters.db_num);
  // Resolve sortby field to actual identifier (e.g., "n1" -> "$.n1" for JSON)
  std::string sortby_identifier;
  if (parameters.sortby_parameter.has_value()) {
    auto schema_identifier = parameters.index_schema->GetIdentifier(
        parameters.sortby_parameter->field);
    sortby_identifier = schema_identifier.ok()
                            ? *schema_identifier
                            : parameters.sortby_parameter->field;
    identifiers.insert(sortby_identifier);
  }
  auto key_str = vmsdk::MakeUniqueValkeyString(key);
  auto key_obj = vmsdk::MakeUniqueValkeyOpenKey(
      ctx, key_str.get(), VALKEYMODULE_OPEN_KEY_NOEFFECTS | VALKEYMODULE_READ);
  if (!key_obj) {
    return absl::NotFoundError("Key not found");
  }
  mstime_t expire = ValkeyModule_GetExpire(key_obj.get());
  if (expire != VALKEYMODULE_NO_EXPIRE && expire <= 0) {
    return absl::NotFoundError("Key expired");
  }
  VMSDK_ASSIGN_OR_RETURN(auto content, attribute_data_type.FetchAllRecords(
                                           ctx, vector_identifier,
                                           key_obj.get(), key, identifiers));
  if (parameters.filter_parse_results.filter_identifiers.empty()) {
    // When returning early, we need to rename the sortby field from the
    // resolved identifier (e.g., "$.n1") back to the alias (e.g., "n1")
    if (parameters.sortby_parameter.has_value() &&
        sortby_identifier != parameters.sortby_parameter->field) {
      auto itr = content.find(sortby_identifier);
      if (itr != content.end()) {
        auto value = std::move(itr->second);
        content.erase(itr);
        content.emplace(parameters.sortby_parameter->field,
                        RecordsMapValue(vmsdk::MakeUniqueValkeyString(
                                            parameters.sortby_parameter->field),
                                        std::move(value.value)));
      }
    }
    return content;
  }
  if (!VerifyFilter(parameters, content, neighbor)) {
    return absl::NotFoundError("Verify filter failed");
  }
  RecordsMap return_content;
  static const vmsdk::UniqueValkeyString kJsonRootElementQueryPtr =
      vmsdk::MakeUniqueValkeyString(kJsonRootElementQuery);
  return_content.emplace(
      kJsonRootElementQuery,
      RecordsMapValue(
          kJsonRootElementQueryPtr.get(),
          std::move(content.find(kJsonRootElementQuery)->second.value)));

  if (parameters.sortby_parameter.has_value()) {
    auto itr = content.find(sortby_identifier);
    if (itr != content.end()) {
      // Use the alias (sortby_parameter->field) as the key in the response,
      // not the resolved identifier
      return_content.emplace(
          parameters.sortby_parameter->field,
          RecordsMapValue(
              vmsdk::MakeUniqueValkeyString(parameters.sortby_parameter->field),
              std::move(itr->second.value)));
    }
  }
  return return_content;
}

absl::StatusOr<RecordsMap> GetContent(
    ValkeyModuleCtx *ctx, const AttributeDataType &attribute_data_type,
    const query::SearchParameters &parameters,
    const indexes::Neighbor &neighbor,
    const std::optional<std::string> &vector_identifier) {
  auto key = neighbor.external_id->Str();
  if (attribute_data_type.ToProto() ==
          data_model::AttributeDataType::ATTRIBUTE_DATA_TYPE_JSON &&
      parameters.return_attributes.empty()) {
    return GetContentNoReturnJson(ctx, attribute_data_type, parameters,
                                  neighbor, vector_identifier);
  }
  absl::flat_hash_set<absl::string_view> identifiers;
  for (const auto &return_attribute : parameters.return_attributes) {
    identifiers.insert(vmsdk::ToStringView(return_attribute.identifier.get()));
  }
  if (!parameters.return_attributes.empty()) {
    for (const auto &filter_identifier :
         parameters.filter_parse_results.filter_identifiers) {
      identifiers.insert(filter_identifier);
    }
  }
  vmsdk::ValkeySelectDbGuard select_db_guard(ctx, parameters.db_num);
  // Resolve sortby field to actual identifier. Only add to identifiers set
  // when return_attributes is specified, because when return_attributes is
  // empty, all fields are fetched anyway.
  std::string sortby_identifier;
  if (parameters.sortby_parameter.has_value()) {
    auto schema_identifier = parameters.index_schema->GetIdentifier(
        parameters.sortby_parameter->field);
    sortby_identifier = schema_identifier.ok()
                            ? *schema_identifier
                            : parameters.sortby_parameter->field;
    // Only add sortby to identifiers when return_attributes is not empty
    if (!parameters.return_attributes.empty()) {
      identifiers.insert(sortby_identifier);
    }
  }
  auto key_str = vmsdk::MakeUniqueValkeyString(key);
  auto key_obj = vmsdk::MakeUniqueValkeyOpenKey(
      ctx, key_str.get(), VALKEYMODULE_OPEN_KEY_NOEFFECTS | VALKEYMODULE_READ);
  if (!key_obj) {
    return absl::NotFoundError("Key not found");
  }
  mstime_t expire = ValkeyModule_GetExpire(key_obj.get());
  if (expire != VALKEYMODULE_NO_EXPIRE && expire <= 0) {
    return absl::NotFoundError("Key expired");
  }
  VMSDK_ASSIGN_OR_RETURN(auto content,
                         attribute_data_type.FetchAllRecords(
                             ctx, vector_identifier, key_obj.get(),
                             neighbor.external_id->Str(), identifiers));
  if (parameters.filter_parse_results.filter_identifiers.empty()) {
    return content;
  }
  if (!VerifyFilter(parameters, content, neighbor)) {
    return absl::NotFoundError("Verify filter failed");
  }
  if (parameters.return_attributes.empty()) {
    return content;
  }
  RecordsMap return_content;
  for (auto &return_attribute : parameters.return_attributes) {
    auto itr =
        content.find(vmsdk::ToStringView(return_attribute.identifier.get()));
    if (itr == content.end()) {
      continue;
    }
    return_content.emplace(
        vmsdk::ToStringView(return_attribute.identifier.get()),
        RecordsMapValue(
            return_attribute.identifier.get(),
            vmsdk::RetainUniqueValkeyString(itr->second.value.get())));
  }

  // Add sortby field to return_content for sorting, even when return_attributes
  // is not empty. Use the alias (sortby_parameter->field) as the key.
  if (parameters.sortby_parameter.has_value()) {
    auto itr = content.find(sortby_identifier);
    if (itr != content.end()) {
      return_content.emplace(
          parameters.sortby_parameter->field,
          RecordsMapValue(
              vmsdk::MakeUniqueValkeyString(parameters.sortby_parameter->field),
              vmsdk::RetainUniqueValkeyString(itr->second.value.get())));
    }
  }

  return return_content;
}

// Adds all local content for neighbors to the list of neighbors.
//
// Any neighbors already contained in the attribute content map will be skipped.
// Any data not found locally will be skipped.
void ProcessNeighborsForReply(
    ValkeyModuleCtx *ctx, const AttributeDataType &attribute_data_type,
    std::vector<indexes::Neighbor> &neighbors,
    const query::SearchParameters &parameters,
    const std::optional<std::string> &vector_identifier,
    bool allow_raw_reply) {
  const auto max_content_size =
      options::GetMaxSearchResultRecordSize().GetValue();
  const auto max_content_fields =
      options::GetMaxSearchResultFieldsCount().GetValue();
  // Hoisted once for the whole batch.
  vmsdk::ValkeySelectDbGuard select_db_guard(ctx, parameters.db_num);
  // Fast path eligibility: hash content via ScanKeyRawPinned -> pinned shells
  // -> ReplyWithString -> BULK_STR_REF (zero-copy all the way). Applies to
  // plain, vector (score prefix added at reply time), and RETURN-subset
  // queries. Now also SORTBY: we pin the sort field and order by it, then emit
  // the (zero-copy) content -- only the sort key is inspected, never copied.
  // Excludes JSON (values aren't borrowable hash sds) and any computed/
  // transformed reply value (FT.AGGREGATE REDUCE/APPLY) -- those have no stored
  // buffer to borrow. filter_identifiers being
  // non-empty is OK -- the index already filtered.
  // NOTE (staleness): like the original plain fast path, this skips VerifyFilter
  // (the seq-id re-check). A doc mutated out of the match set between the worker
  // scan and the reply is still returned. This gap pre-existed for plain
  // content; it now also covers vector/RETURN. Acceptable only while the
  // point-in-time snapshot semantics are acceptable for FT.SEARCH content.
  // VS_NO_ZEROCOPY (env) forces the standard FetchAllRecords copy path -- the
  // exact path a build without zero-copy takes -- for controlled A/B baselines.
  static const bool kZeroCopyDisabled = (std::getenv("VS_NO_ZEROCOPY") != nullptr);
  const bool raw_eligible =
      !kZeroCopyDisabled &&
      allow_raw_reply &&
      ValkeyModule_ScanKeyRawPinned != nullptr &&
      attribute_data_type.ToProto() ==
          data_model::AttributeDataType::ATTRIBUTE_DATA_TYPE_HASH;
  // For RETURN <fields>, pin only the requested identifiers (not the whole
  // doc). When SORTBY is present we ALSO pin the sort field so ApplySorting can
  // order by it -- even if it is not in RETURN it is read for sorting, not
  // emitted. SORTBY with no RETURN pins all fields, so the sort field is
  // already among them. The sort key is a pinned reference, not a copy: content
  // stays zero-copy; only the small value we must *inspect* to order is read.
  // Built once; string_views reference storage owned by `parameters` /
  // `raw_sortby_identifier`.
  const bool raw_has_return = !parameters.return_attributes.empty();
  std::string raw_sortby_identifier;  // must outlive raw_wanted's string_views
  absl::flat_hash_set<absl::string_view> raw_wanted;
  if (raw_eligible && raw_has_return) {
    for (const auto &ra : parameters.return_attributes) {
      raw_wanted.insert(vmsdk::ToStringView(ra.identifier.get()));
    }
    if (parameters.sortby_parameter.has_value()) {
      auto id =
          parameters.index_schema->GetIdentifier(parameters.sortby_parameter->field);
      raw_sortby_identifier = id.ok() ? *id : parameters.sortby_parameter->field;
      raw_wanted.insert(raw_sortby_identifier);
    }
  }
  for (auto &neighbor : neighbors) {
    // Remote neighbors (from fanout) always have attribute_contents populated,
    // so they skip this entire block. Only local neighbors without content
    // reach the slot ownership check below.
    if (neighbor.attribute_contents.has_value()) {
      continue;
    }
    // Check slot ownership for local neighbors before fetching content.
    // Remote neighbors are never checked since they always have content.
    if (!CheckSlotOwnership(ctx, neighbor.external_id->Str())) {
      // Skip this neighbor - we don't own its slot.
      continue;
    }
    if (raw_eligible) {
      auto key_str =
          vmsdk::MakeUniqueValkeyString(neighbor.external_id->Str());
      auto key_obj = vmsdk::MakeUniqueValkeyOpenKey(
          ctx, key_str.get(),
          VALKEYMODULE_OPEN_KEY_NOEXPIRE | VALKEYMODULE_READ);
      if (!key_obj) {
        continue;
      }
      mstime_t expire = ValkeyModule_GetExpire(key_obj.get());
      if (expire != VALKEYMODULE_NO_EXPIRE && expire <= 0) {
        continue;
      }
      // Vector parity with FetchAllRecords: drop docs missing the vector field
      // so the KNN reply set matches the copy path exactly.
      if (vector_identifier.has_value() &&
          !HashHasRecord(key_obj.get(), vector_identifier.value())) {
        continue;
      }
      auto raw = FetchAllHashFieldsRawPinned(
          key_obj.get(), raw_has_return ? &raw_wanted : nullptr);
      if (raw.size() > max_content_fields) {
        ++Metrics::GetStats().query_result_record_dropped_cnt;
        continue;
      }
      size_t total_size = 0;
      bool size_exceeded = false;
      for (const auto &item : raw) {
        total_size += item.first.size();
        size_t vlen;
        const char *vptr = ValkeyModule_StringPtrLen(item.second.get(), &vlen);
        total_size += vlen;
        if (total_size > max_content_size) {
          ++Metrics::GetStats().query_result_record_dropped_cnt;
          size_exceeded = true;
          break;
        }
      }
      if (!size_exceeded) {
        neighbor.raw_contents = std::move(raw);
      }
      continue;
    }
    auto content = GetContent(ctx, attribute_data_type, parameters, neighbor,
                              vector_identifier);
    if (!content.ok()) {
      continue;
    }

    // Check content size before assigning

    size_t total_size = 0;
    bool size_exceeded = false;
    if (content.value().size() > max_content_fields) {
      ++Metrics::GetStats().query_result_record_dropped_cnt;
      VMSDK_LOG_EVERY_N_SEC(WARNING, ctx, 1)
          << "Content field number exceeds configured limit of "
          << max_content_fields << " for neighbor with ID: "
          << vmsdk::config::RedactIfNeeded((*neighbor.external_id).Str());
      continue;
    }

    for (const auto &item : content.value()) {
      total_size += item.first.size();
      total_size += vmsdk::ToStringView(item.second.value.get()).size();
      if (total_size > max_content_size) {
        ++Metrics::GetStats().query_result_record_dropped_cnt;
        VMSDK_LOG_EVERY_N_SEC(WARNING, ctx, 1)
            << "Content size exceeds configured limit of " << max_content_size
            << " bytes for neighbor with ID: "
            << vmsdk::config::RedactIfNeeded((*neighbor.external_id).Str());
        size_exceeded = true;
        break;
      }
    }

    // Only assign content if it's within size limit
    if (!size_exceeded) {
      neighbor.attribute_contents = std::move(content.value());
    }
  }
  // Remove all entries that don't have content now.
  // TODO: incorporate a retry in case of removal.
  neighbors.erase(
      std::remove_if(neighbors.begin(), neighbors.end(),
                     [](const indexes::Neighbor &neighbor) {
                       return !neighbor.attribute_contents.has_value() &&
                              !neighbor.raw_contents.has_value();
                     }),
      neighbors.end());
}

}  // namespace valkey_search::query
