/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "third_party/snowball/include/libstemmer.h"

namespace valkey_search::indexes::text {

namespace {

// Test words of various sizes
std::vector<std::string> GenerateTestWords() {
  return {
      // --- Group 1: Non-Stemmable & Base Forms (Efficiency test) ---
      "sky", "adroit", "vague", "sync",         // Short, already at root
      "bureaucracy", "alkali", "banana",        // Longer, but no valid English suffix

      // --- Group 2: Common Suffix Variations (Branching logic) ---
      "caress", "ponies", "ties", "cats",       // Step 1a: s-suffixes
      "agreed", "feed", "bled", "plastered",    // Step 1b: e-suffixes
      "happy", "happily", "sky", "crying",      // Step 1c: y-suffixes
      
      // --- Group 3: Morphological Complexity (Snowball Step 2-5) ---
      "relativity", "conflation", "rational",   // Relational suffixes
      "sensitize", "hopefulness", "goodness",   // Compound suffixes
      "probate", "formative", "adjunct",        // Latinate roots
      
      // --- Group 4: Exceptional Forms (Hardcoded rules) ---
      "skis", "skies",                          // Snowball special cases
      "news", "atlas", "cosmos",                // Words that look like plurals but aren't
      "dying", "lying", "tying",                // i/y vowel transitions

      // --- Group 5: Extreme Lengths (Stress testing Karthik's version) ---
      "internationalization",                   // 20 chars
      "incomprehensibilities",                  // 21 chars
      "dichlorodiphenyltrichloroethane",        // 31 chars (DDT)
      "antidisestablishmentarianism",           // 28 chars
      "pneumonoultramicroscopicsilicovolcanoconiosis", // 45 chars (The "final boss")
      
      // --- Group 6: Proper Nouns & Junk (Robustness) ---
      "California", "Snowball", "Google",       // Capitalized (often ignored or unique)
      "xyzzy", "brrr", "asdfghjkl"              // Non-dictionary/consonant-heavy
  };
}


const std::vector<std::string> kTestWords = GenerateTestWords();

// Benchmark snowball stemmer with variable word sizes
static void BM_SnowballStemmer(benchmark::State& state) {
  const std::string& input = kTestWords[state.range(0)];
  
  // Create stemmer once
  struct sb_stemmer* stemmer = sb_stemmer_new("english", "UTF_8");
  if (!stemmer) {
    state.SkipWithError("Failed to create stemmer");
    return;
  }

  for (auto _ : state) {
    const sb_symbol* stemmed = sb_stemmer_stem(
        stemmer, 
        reinterpret_cast<const sb_symbol*>(input.c_str()), 
        input.size());
    benchmark::DoNotOptimize(stemmed);
  }

  sb_stemmer_delete(stemmer);
  state.SetLabel(absl::StrCat("len=", input.size()));
}

// Register benchmark for all word sizes
BENCHMARK(BM_SnowballStemmer)->DenseRange(0, kTestWords.size() - 1);

}  // namespace

}  // namespace valkey_search::indexes::text

BENCHMARK_MAIN();
