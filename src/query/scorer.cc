/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/query/scorer.h"

namespace valkey_search::query {
namespace {

class StubScorer : public Scorer {
 public:
  float ComputeScore(const ScoringContext& ctx) const override {
    if (!ctx.has_text_fields) return 0.0f;
    return 1.0f;  // Placeholder until real algorithms are implemented
  }
};

}  // namespace

std::unique_ptr<Scorer> Scorer::Create(ScorerType type) {
  return std::make_unique<StubScorer>();
}

}  // namespace valkey_search::query
