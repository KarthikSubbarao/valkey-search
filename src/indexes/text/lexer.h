/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef _VALKEY_SEARCH_INDEXES_TEXT_LEXER_H_
#define _VALKEY_SEARCH_INDEXES_TEXT_LEXER_H_

/*

STATELESS LEXER DESIGN

The Lexer is a stateless processor that takes configuration parameters
and produces tokenized output. Configuration is stored in TextIndexSchema
and Text classes, then passed to lexer methods as parameters.

Tokenization Pipeline:
1. Split text on punctuation characters (configurable)
2. Convert to lowercase
3. Stop word removal (filter out common words)
4. Apply stemming based on language and field settings

*/

#include <bitset>
#include <string>
#include <vector>
#include <variant>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/index_schema.pb.h"

struct sb_stemmer;

namespace valkey_search::indexes::text {

struct Lexer {
  // Struct to bundle the string with its original reading-order position
  struct Token {
    std::variant<absl::string_view, std::string> text_storage;
    uint32_t position;
    
    // Constructor for string_view (zero-copy)
    Token(absl::string_view view, uint32_t pos) : text_storage(view), position(pos) {}
    
    // Constructor for owned string (when transformation needed)
    Token(std::string str, uint32_t pos) : text_storage(std::move(str)), position(pos) {}
    
    // Get text as string_view regardless of storage type
    absl::string_view text() const {
      return std::visit([](const auto& t) -> absl::string_view {
        if constexpr (std::is_same_v<std::decay_t<decltype(t)>, std::string>) {
          return absl::string_view(t);
        } else {
          return t;
        }
      }, text_storage);
    }
  };

  struct TokenizationResult {
    std::vector<Token> tokens;
  };

  Lexer(data_model::Language language, const std::string& punctuation,
        const std::vector<std::string>& stop_words);
  ~Lexer() = default;

  absl::StatusOr<TokenizationResult> Tokenize(
      absl::string_view text, bool stemming_enabled, uint32_t min_stem_size,
      absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>>*
          stem_mappings = nullptr) const;

  bool IsPunctuation(char c) const {
    return punct_bitmap_[static_cast<unsigned char>(c)];
  }

  bool IsStopWord(absl::string_view lowercase_word) const {
    return stop_words_set_.contains(lowercase_word);
  }
  sb_stemmer* GetStemmer() const;
  void NormalizeLowerCaseInPlace(std::string& str) const;
  void StemWordInPlace(std::string& word, sb_stemmer* stemmer,
                       uint32_t min_stem_size = 0) const;
  void UpdateStemMap(
      absl::string_view original_word, sb_stemmer* stemmer,
      uint32_t min_stem_size,
      absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>>&
          stem_mappings) const;

 private:
  data_model::Language language_;
  std::bitset<256> punct_bitmap_;
  absl::flat_hash_set<std::string> stop_words_set_;

  // UTF-8 processing helpers
  bool IsValidUtf8(absl::string_view text) const;
  // Common stemming logic
  std::string_view DoStemming(absl::string_view word, sb_stemmer* stemmer,
                              uint32_t min_stem_size) const;
};

}  // namespace valkey_search::indexes::text

#endif
