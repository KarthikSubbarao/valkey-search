/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEYSEARCH_SRC_QUERY_SCORER_H_
#define VALKEYSEARCH_SRC_QUERY_SCORER_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/indexes/text/text_iterator.h"

namespace valkey_search::query {

enum class ScorerType {
  kTFIDF,
  kBM25STD,
  kDOCSCORE,
};

// Per-term scoring statistics collected from a TextIterator leaf.
struct TermInfo {
  uint32_t tf{0};           // Term frequency in this document
  uint64_t df{0};           // Document frequency (docs containing this term)
  float field_weight{1.0f}; // Weight from FT.CREATE WEIGHT (future)
  float query_weight{1.0f}; // Weight from query QMA (future)
};

// All data needed by a Scorer to compute a document's relevance score.
// Built from a TextIterator after it has matched a document.
struct ScoringContext {
  float document_score{1.0f};    // From SCORE_FIELD or SCORE default
  uint32_t doc_len{0};           // Total tokens in the document
  uint32_t norm{0};              // Max frequency of any word in the document
  std::vector<TermInfo> terms;   // One entry per leaf query term
  uint64_t total_docs{0};        // N: total documents in the index
  double avg_doc_len{0.0};       // Average document length across the index
  float slop_factor{1.0f};       // Slop penalty (computed from positions)
  bool has_text_fields{false};   // False → score is 0 regardless of other fields

  // Build a ScoringContext from a TextIterator that is positioned on a
  // matching document. The iterator tree is walked via CollectTermInfos() so
  // that every leaf term contributes its own TermInfo — this is the correct
  // approach for nested queries (AND/OR/proximity) because the iterator
  // already holds all the per-term data computed during matching.
  //
  // |total_docs| and |avg_doc_len| are index-level stats from TextIndexSchema.
  // |document_score| is the per-document score from SCORE/SCORE_FIELD.
  static ScoringContext FromIterator(
      const indexes::text::TextIterator& iter,
      uint64_t total_docs,
      double avg_doc_len,
      float document_score = 1.0f) {
    ScoringContext ctx;
    ctx.total_docs = total_docs;
    ctx.avg_doc_len = avg_doc_len;
    ctx.document_score = document_score;
    ctx.doc_len = iter.GetDocumentLength();
    ctx.norm = iter.GetNorm();
    ctx.has_text_fields = (ctx.doc_len > 0 || ctx.norm > 0);

    // Collect per-term TF/DF by walking the iterator tree.
    std::vector<std::pair<uint32_t, uint64_t>> raw;
    iter.CollectTermInfos(raw);
    ctx.terms.reserve(raw.size());
    for (auto& [tf, df] : raw) {
      ctx.terms.push_back(TermInfo{tf, df});
    }
    if (!ctx.terms.empty()) ctx.has_text_fields = true;
    return ctx;
  }
};

class Scorer {
 public:
  virtual ~Scorer() = default;
  virtual float ComputeScore(const ScoringContext& ctx) const = 0;
  static std::unique_ptr<Scorer> Create(ScorerType type);
};

}  // namespace valkey_search::query

#endif  // VALKEYSEARCH_SRC_QUERY_SCORER_H_
