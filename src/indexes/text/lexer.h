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
#include <cstring>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/index_schema.pb.h"

struct sb_stemmer;

namespace valkey_search::indexes::text {

struct Lexer {
  // Struct to bundle the string with its original reading-order position
  struct Token {
    uintptr_t data;  // Pointer + Tag (LSB = 0: view, LSB = 1: owned)
    uint32_t len;
    uint32_t pos;

    // Constructor for string_view (zero-copy)
    Token(absl::string_view view, uint32_t position)
        : len(view.size()), pos(position) {
      // Ensure pointer is aligned and clear LSB
      uintptr_t ptr_val = reinterpret_cast<uintptr_t>(view.data());
      // Check alignment - pointers should be at least 2-byte aligned
      if (ptr_val & 1) {
        // Unaligned pointer - fall back to owned copy
        if (view.empty()) {
          data = 0;  // nullptr with LSB=0
        } else {
          char* owned_ptr = static_cast<char*>(malloc(view.size()));
          if (owned_ptr) {
            std::memcpy(owned_ptr, view.data(), view.size());
            data = reinterpret_cast<uintptr_t>(owned_ptr) | 1;
          } else {
            data = 1;  // allocation failed
            len = 0;
          }
        }
      } else {
        data = ptr_val;  // LSB already 0
      }
    }

    // Constructor for owned string (when transformation needed)
    Token(std::string&& str, uint32_t position)
        : len(str.size()), pos(position) {
      if (str.empty()) {
        data = 1;  // nullptr | 1
      } else {
        char* owned_ptr = static_cast<char*>(malloc(str.size()));
        if (owned_ptr) {
          std::memcpy(owned_ptr, str.data(), str.size());
          data = reinterpret_cast<uintptr_t>(owned_ptr) | 1;
        } else {
          data = 1;
          len = 0;
        }
      }
    }

    // Move constructor
    Token(Token&& other) noexcept
        : data(other.data), len(other.len), pos(other.pos) {
      other.data = 0;
      other.len = 0;
    }

    // Move assignment
    Token& operator=(Token&& other) noexcept {
      if (this != &other) {
        cleanup();
        data = other.data;
        len = other.len;
        pos = other.pos;
        other.data = 0;
        other.len = 0;
      }
      return *this;
    }

    // Destructor
    ~Token() { cleanup(); }

    // Delete copy operations
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;

    // Helper to get the actual pointer
    const char* ptr() const {
      return reinterpret_cast<const char*>(data & ~uintptr_t(1));
    }

    // Helper to check if we own the memory
    bool is_owned() const { return (data & 1) != 0; }

    // Convert to string_view for sorting/indexing
    absl::string_view text() const {
      const char* p = ptr();
      if (p == nullptr || len == 0) {
        return absl::string_view();
      }
      return absl::string_view(p, len);
    }

    // Position accessor
    uint32_t position() const { return pos; }

    // Comparison operator for sorting
    bool operator<(const Token& other) const { return text() < other.text(); }

   private:
    void cleanup() {
      if (is_owned()) {
        const char* p = ptr();
        if (p != nullptr) {
          free(const_cast<char*>(p));
        }
      }
    }
  };

  struct TokenizationResult {
    std::vector<Token> tokens;

    // Default constructor
    TokenizationResult() = default;

    // Destructor - Token destructors handle cleanup automatically
    ~TokenizationResult() = default;

    // Move constructor
    TokenizationResult(TokenizationResult&& other) noexcept
        : tokens(std::move(other.tokens)) {}

    // Move assignment
    TokenizationResult& operator=(TokenizationResult&& other) noexcept {
      if (this != &other) {
        tokens = std::move(other.tokens);
      }
      return *this;
    }

    // Delete copy operations to prevent accidental copying
    TokenizationResult(const TokenizationResult&) = delete;
    TokenizationResult& operator=(const TokenizationResult&) = delete;
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
