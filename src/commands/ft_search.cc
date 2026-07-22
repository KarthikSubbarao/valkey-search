/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include <strings.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "src/commands/commands.h"
#include "src/commands/ft_search_parser.h"
#include "src/indexes/index_base.h"
#include "src/indexes/vector_base.h"
#include "src/metrics.h"
#include "src/query/response_generator.h"
#include "src/query/search.h"
#include "value.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/type_conversions.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace valkey_search {

namespace {
// FT.SEARCH idx "*=>[KNN 10 @vec $BLOB AS score]" PARAMS 2 BLOB
// "\x12\xa9\xf5\x6c" DIALECT 2

void ReplyAvailNeighbors(ValkeyModuleCtx *ctx,
                         const query::SearchResult &search_result,
                         const query::SearchParameters &parameters) {
  if (parameters.IsNonVectorQuery()) {
    ValkeyModule_ReplyWithLongLong(ctx, search_result.total_count);
  } else {
    ValkeyModule_ReplyWithLongLong(
        ctx,
        std::min(search_result.total_count, static_cast<size_t>(parameters.k)));
  }
}

void SendReplyNoContent(ValkeyModuleCtx *ctx,
                        const query::SearchResult &search_result,
                        const query::SearchParameters &parameters) {
  const auto &neighbors = search_result.neighbors;
  auto range = search_result.GetSerializationRange(parameters);

  ValkeyModule_ReplyWithArray(ctx, range.count() + 1);
  ReplyAvailNeighbors(ctx, search_result, parameters);
  for (auto i = range.start_index; i < range.end_index; ++i) {
    ValkeyModule_ReplyWithString(
        ctx, vmsdk::MakeUniqueValkeyString(*neighbors[i].external_id).get());
  }
}

void ReplyScore(ValkeyModuleCtx *ctx, ValkeyModuleString &score_as,
                const indexes::Neighbor &neighbor) {
  ValkeyModule_ReplyWithString(ctx, &score_as);
  auto score_value = absl::StrFormat("%.12g", neighbor.distance);
  ValkeyModule_ReplyWithString(
      ctx, vmsdk::MakeUniqueValkeyString(score_value).get());
}

void SerializeNeighbors(ValkeyModuleCtx *ctx,
                        const query::SearchResult &search_result,
                        const query::SearchParameters &parameters) {
  const auto &neighbors = search_result.neighbors;
  CHECK_GT(static_cast<size_t>(parameters.k), parameters.limit.first_index);
  auto range = search_result.GetSerializationRange(parameters);

  ValkeyModule_ReplyWithArray(ctx, 2 * range.count() + 1);
  ReplyAvailNeighbors(ctx, search_result, parameters);

  for (auto i = range.start_index; i < range.end_index; ++i) {
    ValkeyModule_ReplyWithString(
        ctx, vmsdk::MakeUniqueValkeyString(*neighbors[i].external_id).get());
    const bool use_raw = neighbors[i].raw_contents.has_value();
    if (parameters.return_attributes.empty()) {
      if (use_raw) {
        // Zero-copy borrowed content + score prefix.
        const auto &raw = neighbors[i].raw_contents.value();
        ValkeyModule_ReplyWithArray(ctx, 2 * raw.size() + 2);
        ReplyScore(ctx, *parameters.score_as, neighbors[i]);
        for (const auto &kv : raw) {
          ValkeyModule_ReplyWithStringBuffer(ctx, kv.first.data(),
                                             kv.first.size());
          ValkeyModule_ReplyWithStringBuffer(ctx, kv.second.first,
                                             kv.second.second);
        }
      } else {
        ValkeyModule_ReplyWithArray(
            ctx, 2 * neighbors[i].attribute_contents.value().size() + 2);
        ReplyScore(ctx, *parameters.score_as, neighbors[i]);
        for (auto &attribute_content :
             neighbors[i].attribute_contents.value()) {
          ValkeyModule_ReplyWithString(
              ctx, attribute_content.second.GetIdentifier());
          ValkeyModule_ReplyWithString(ctx,
                                       attribute_content.second.value.get());
        }
      }
    } else {
      ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
      size_t cnt = 0;
      for (const auto &return_attribute : parameters.return_attributes) {
        if (vmsdk::ToStringView(parameters.score_as.get()) ==
            vmsdk::ToStringView(return_attribute.identifier.get())) {
          ReplyScore(ctx, *parameters.score_as, neighbors[i]);
          ++cnt;
          continue;
        }
        if (use_raw) {
          // RETURN subset over borrowed content; score handled above.
          auto ident = vmsdk::ToStringView(return_attribute.identifier.get());
          for (const auto &kv : neighbors[i].raw_contents.value()) {
            if (kv.first == ident) {
              ValkeyModule_ReplyWithString(ctx, return_attribute.alias.get());
              ValkeyModule_ReplyWithStringBuffer(ctx, kv.second.first,
                                                 kv.second.second);
              ++cnt;
              break;
            }
          }
        } else {
          auto it = neighbors[i].attribute_contents.value().find(
              vmsdk::ToStringView(return_attribute.identifier.get()));
          if (it != neighbors[i].attribute_contents.value().end()) {
            ValkeyModule_ReplyWithString(ctx, return_attribute.alias.get());
            ValkeyModule_ReplyWithString(ctx, it->second.value.get());
            ++cnt;
          }
        }
      }
      ValkeyModule_ReplySetArrayLength(ctx, 2 * cnt);
    }
  }
}

// Reads the sort field value for a neighbor, preferring the zero-copy borrowed
// raw_contents (keyed by the hash field identifier) and falling back to
// the copy-path attribute_contents (keyed by the sort field/alias). Returns
// false when the field is absent so callers can preserve "missing sorts last".
bool TryGetSortFieldValue(const indexes::Neighbor &neighbor,
                          const SearchCommand &command, std::string *out) {
  if (!command.sortby_parameter.has_value()) {
    return false;
  }
  absl::string_view field = command.sortby_parameter->field;
  if (neighbor.raw_contents.has_value()) {
    auto id = command.index_schema->GetIdentifier(field);
    const std::string ident = id.ok() ? *id : std::string(field);
    for (const auto &kv : neighbor.raw_contents.value()) {
      if (kv.first == ident) {
        out->assign(kv.second.first, kv.second.second);
        return true;
      }
    }
    return false;
  }
  if (neighbor.attribute_contents.has_value()) {
    auto it = neighbor.attribute_contents->find(field);
    if (it != neighbor.attribute_contents->end()) {
      *out = std::string(vmsdk::ToStringView(it->second.value.get()));
      return true;
    }
  }
  return false;
}

// Helper function to get the sort key value for a neighbor
std::string GetSortKeyValue(const indexes::Neighbor &neighbor,
                            const SearchCommand &command) {
  std::string v;
  if (TryGetSortFieldValue(neighbor, command, &v)) {
    return v;
  }
  return "";
}

// Handle non-vector queries by processing the neighbors and replying with the
// attribute contents.
void SerializeNonVectorNeighbors(ValkeyModuleCtx *ctx,
                                 const query::SearchResult &search_result,
                                 const SearchCommand &command) {
  const auto &neighbors = search_result.neighbors;
  auto range = search_result.GetSerializationRange(command);

  // When with_sort_keys is true, we add an extra element per result (the sort
  // key)
  size_t elements_per_result = command.with_sort_keys ? 3 : 2;
  ValkeyModule_ReplyWithArray(ctx, elements_per_result * range.count() + 1);
  ReplyAvailNeighbors(ctx, search_result, command);
  for (size_t i = range.start_index; i < range.end_index; ++i) {
    // Document ID
    ValkeyModule_ReplyWithString(
        ctx, vmsdk::MakeUniqueValkeyString(*neighbors[i].external_id).get());

    // Sort key value (prefixed with #) when WITHSORTKEYS is specified
    if (command.with_sort_keys) {
      std::string sort_key_value = GetSortKeyValue(neighbors[i], command);
      std::string prefixed_value = "#" + sort_key_value;
      ValkeyModule_ReplyWithString(
          ctx, vmsdk::MakeUniqueValkeyString(prefixed_value).get());
    }

    // Zero-copy borrowed path: reply directly with borrowed ptrs.
    if (neighbors[i].raw_contents.has_value()) {
      const auto &raw = neighbors[i].raw_contents.value();
      if (command.return_attributes.empty()) {
        ValkeyModule_ReplyWithArray(ctx, 2 * raw.size());
        for (const auto &kv : raw) {
          ValkeyModule_ReplyWithStringBuffer(ctx, kv.first.data(),
                                             kv.first.size());
          ValkeyModule_ReplyWithStringBuffer(ctx, kv.second.first,
                                             kv.second.second);
        }
      } else {
        // RETURN subset: emit alias + borrowed value, preserving RETURN order.
        ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
        size_t cnt = 0;
        for (const auto &return_attribute : command.return_attributes) {
          auto ident = vmsdk::ToStringView(return_attribute.identifier.get());
          for (const auto &kv : raw) {
            if (kv.first == ident) {
              ValkeyModule_ReplyWithString(ctx, return_attribute.alias.get());
              ValkeyModule_ReplyWithStringBuffer(ctx, kv.second.first,
                                                 kv.second.second);
              ++cnt;
              break;
            }
          }
        }
        ValkeyModule_ReplySetArrayLength(ctx, 2 * cnt);
      }
      continue;
    }

    const auto &contents = neighbors[i].attribute_contents.value();

    if (command.return_attributes.empty()) {
      ValkeyModule_ReplyWithArray(ctx, 2 * contents.size());
      for (const auto &attribute_content : contents) {
        ValkeyModule_ReplyWithString(ctx,
                                     attribute_content.second.GetIdentifier());
        ValkeyModule_ReplyWithString(ctx, attribute_content.second.value.get());
      }
    } else {
      ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
      size_t cnt = 0;
      for (const auto &return_attribute : command.return_attributes) {
        auto it = contents.find(
            vmsdk::ToStringView(return_attribute.identifier.get()));
        if (it != contents.end()) {
          ValkeyModule_ReplyWithString(ctx, return_attribute.alias.get());
          ValkeyModule_ReplyWithString(ctx, it->second.value.get());
          ++cnt;
        }
      }
      ValkeyModule_ReplySetArrayLength(ctx, 2 * cnt);
    }
  }
}

}  // namespace
// Apply sorting to neighbors based on attribute values in attribute_contents
void ApplySorting(std::vector<indexes::Neighbor> &neighbors,
                  const SearchCommand &parameters) {
  if (!parameters.sortby_parameter.has_value() || neighbors.empty()) {
    return;
  }

  auto sortby = parameters.sortby_parameter.value();

  // Check if field is a declared numeric attribute
  auto index_result = parameters.index_schema->GetIndex(sortby.field);
  bool is_numeric =
      index_result.ok() &&
      index_result.value()->GetIndexerType() == indexes::IndexerType::kNumeric;
  auto compare = [&](const indexes::Neighbor &a,
                     const indexes::Neighbor &b) -> bool {
    std::string va, vb;
    bool fa = TryGetSortFieldValue(a, parameters, &va);
    bool fb = TryGetSortFieldValue(b, parameters, &vb);
    if (!fa) {
      return false;  // a's sort key missing -> orders after b
    }
    if (!fb) {
      return true;  // b's sort key missing -> a orders before b
    }

    expr::Value val_a, val_b;
    if (is_numeric) {
      auto num_a = vmsdk::To<double>(va).value_or(0.0);
      auto num_b = vmsdk::To<double>(vb).value_or(0.0);
      val_a = expr::Value(num_a);
      val_b = expr::Value(num_b);
    } else {
      val_a = expr::Value(absl::string_view(va));
      val_b = expr::Value(absl::string_view(vb));
    }

    auto cmp = expr::Compare(val_a, val_b);
    if (cmp == expr::Ordering::kLESS) {
      return sortby.order == query::SortOrder::kAscending;
    }
    if (cmp == expr::Ordering::kGREATER) {
      return sortby.order == query::SortOrder::kDescending;
    }
    return false;
  };

  auto amountToKeep = parameters.limit.first_index + parameters.limit.number;
  if (amountToKeep >= neighbors.size()) {
    std::stable_sort(neighbors.begin(), neighbors.end(), compare);
  } else {
    std::partial_sort(neighbors.begin(), neighbors.begin() + amountToKeep,
                      neighbors.end(), compare);
  }
}

// Check for scenarios that require sending an early reply.
// Returns true if an early reply was sent and processing should stop.
bool HandleEarlyReplyScenarios(ValkeyModuleCtx *ctx,
                               query::SearchResult &search_result,
                               const SearchCommand &command) {
  // Check if no results should be returned based on query parameters.
  if (query::ShouldReturnNoResults(command)) {
    ValkeyModule_ReplyWithArray(ctx, 1);
    ValkeyModule_ReplyWithLongLong(ctx, search_result.total_count);
    return true;  // Early reply sent, stop processing
  }

  if (command.no_content) {
    SendReplyNoContent(ctx, search_result, command);
    return true;  // Early reply sent, stop processing
  }

  return false;  // No early reply needed, continue processing
}

// Process neighbors for both vector and non-vector queries
absl::Status ProcessNeighborsForQuery(ValkeyModuleCtx *ctx,
                                      query::SearchResult &search_result,
                                      SearchCommand &command) {
  size_t original_size = search_result.neighbors.size();

  std::optional<std::string> vector_identifier = std::nullopt;

  if (command.IsVectorQuery()) {
    VMSDK_ASSIGN_OR_RETURN(
        vector_identifier,
        command.index_schema->GetIdentifier(command.attribute_alias));
  }
  // Handle vector queries

  query::ProcessNeighborsForReply(
      ctx, command.index_schema->GetAttributeDataType(),
      search_result.neighbors, command, vector_identifier,
      /*allow_raw_reply=*/true);
  // Adjust total count based on neighbors removed during processing
  // due to filtering or missing attributes.
  search_result.total_count -= (original_size - search_result.neighbors.size());

  return absl::OkStatus();
}

// The reply structure is an array which consists of:
// 1. The amount of response elements
// 2. Per response entry:
//   1. The cache entry Hash key
//   2. An array with the following entries:
//      1. Key value: [$score_as] score_value
//      2. Distance value
//      3. Attribute name
//      4. The vector value
// SendReply respects the Limit, see https://valkey.io/commands/ft.search/
void SearchCommand::SendReply(ValkeyModuleCtx *ctx,
                              query::SearchResult &search_result) {
  // Increment success counter.
  ++Metrics::GetStats().query_successful_requests_cnt;

  // 1. Handle early reply scenarios
  if (HandleEarlyReplyScenarios(ctx, search_result, *this)) {
    return;
  }

  // 2. Process neighbors for the query
  auto status = ProcessNeighborsForQuery(ctx, search_result, *this);
  if (!status.ok()) {
    ++Metrics::GetStats().query_failed_requests_cnt;
    ValkeyModule_ReplyWithError(ctx, status.message().data());
    return;
  }

  ApplySorting(search_result.neighbors, *this);

  // 3. Serialize neighbors based on query type
  if (IsNonVectorQuery()) {
    SerializeNonVectorNeighbors(ctx, search_result, *this);
  } else {
    SerializeNeighbors(ctx, search_result, *this);
  }
}

absl::Status FTSearchCmd(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                         int argc) {
  return QueryCommand::Execute(ctx, argv, argc,
                               std::unique_ptr<QueryCommand>(new SearchCommand(
                                   ValkeyModule_GetSelectedDb(ctx))));
}

}  // namespace valkey_search
