/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef _VALKEY_SEARCH_INDEXES_TEXT_RADIX_TREE_H
#define _VALKEY_SEARCH_INDEXES_TEXT_RADIX_TREE_H

/*

A radix tree, but with path compression. This data
structure is functionally similar to a BTree but more space and time efficient
when dealing with common prefixes of keys.

While the RadixTree operates on a word basis for the text search case the target
of the RadixTree is a Postings object which itself has multiple Keys and
Positions within it.

In addition to normal insert/delete operations, the RadixTree has a WordIterator
that supports iteration across multiple word entries that share a common prefix.
Iteration is always done in lexical order.

A Path iterator is provided that operates at the path level. It provides
iteration capabilities for interior sub-trees of the RadixTree. Functionally,
the Path iterator is provided a prefix which identifies the sub-tree to be
iterated over. The iteration is over the set of next valid characters present in
the subtree in lexical order. This iterator can be used to visit all words with
a common prefix while intelligently skipping subsets (subtrees) of words --
ideal for fuzzy matching.

Another feature of a RadixTree is the ability to provide a count of the entries
that have a common prefix in O(len(prefix)) time. This is useful in query
planning.

Even though the description of the RadixTree consistently refers to prefixes,
this implementation also supports a suffix RadixTree. A suffix RadixTree is
simply a RadixTree built by reversing the order of the characters in a string.
For suffix RadixTrees, the external interface for the strings is the same, i.e.,
it is the responsibility of the RadixTree object itself to perform any reverse
ordering that might be required, clients of this interface need not reverse
their strings.

Note that unlike most other search objects, this object is explicitly
multi-thread aware. The multi-thread usage of this object is designed to match
the time-sliced mutex, in other words, during write operations, only a small
subset of the methods are allowed. External iterators are not valid across a
write operation. Conversely, during the read cycle, all non-mutation operations
are allowed and don't require any locking.

Ideally, detection of mutation violations, stale iterators, etc. would be built
into the codebase efficiently enough to be deployed in production code.

*/

#include <concepts>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <variant>

#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "rax/rax.h"
#include "src/indexes/text/invasive_ptr.h"

namespace valkey_search::indexes::text {

// Forward declarations
class Postings;

class Rax {

public:
  class WordIterator;
  class PathIterator;

  // Constructor with optional deletion callback for targets.
  // If provided, callback will be invoked for each target during destruction.
  explicit Rax(void (*free_callback)(void*) = nullptr);
  ~Rax();
  
  // Move constructor and assignment
  Rax(Rax&& other) noexcept;
  Rax& operator=(Rax&& other) noexcept;
  
  // Delete copy constructor and assignment (Rax owns its internal state)
  Rax(const Rax&) = delete;
  Rax& operator=(const Rax&) = delete;

  //
  // Adds the target for the given word, replacing the existing target
  // if there is one. Providing an empty target will cause the word to be
  // deleted from the tree. Only use this API when you don't care about any
  // existing target.
  //
  // (TODO) This function is explicitly multi-thread safe and is
  // designed to allow other mutations to be performed on other words and
  // targets simultaneously, with minimal collisions.
  //
  // It's expected that the caller will know whether or not the word
  // exists. Passing in a word that doesn't exist along with a
  // nullopt new_target will cause the word to be added and
  // then immediately deleted from the tree.
  //
  // void SetTarget(absl::string_view word, TargetPtr new_target);

  //
  // Applies the mutation function to the current target of the word to generate
  // a new target. If the word doesn't already exist, a path for it will be
  // first added to the tree with a default-constructed target. The new target
  // is returned to the caller.
  //
  // The input parameter to the mutate function will be nullopt if there is no
  // entry for this word. Otherwise it will contain the value for this word. The
  // return value of the mutate function is the new value for this word. if the
  // return value is nullopt then this word is deleted from the RadixTree.
  //
  // (TODO) This function is explicitly multi-thread safe and is
  // designed to allow other mutations to be performed on other words and
  // targets simultaneously, with minimal collisions.
  //
  // In all cases, the mutate function is invoked once under the locking
  // provided by the RadixTree itself, so if the target objects are disjoint
  // (which is normal) then no locking is required within the mutate function
  // itself.
  //
  void MutateTarget(absl::string_view word,
                    absl::FunctionRef<void*(void*)> mutate);

  // TODO: Replace with GetWordCount("") once it's implemented
  // Get the total number of words in the RadixTree.
  size_t GetTotalWordCount() const;

  // Get the number of words that have the specified prefix in O(len(prefix))
  // time.
  size_t GetWordCount(absl::string_view prefix) const;

  // Get the length of the longest word in the RadixTree, this can be used to
  // pre-size arrays and strings that are used when iterating on this RadixTree.
  size_t GetLongestWord() const;

  // Check if the Rax tree is valid (not moved-from or null)
  bool IsValid() const;

  // Create a word Iterator over the sequence of words that start with the
  // prefix. The iterator will automatically be positioned to the lexically
  // smallest word and will end with the last word that shares the suffix.
  WordIterator GetWordIterator(absl::string_view prefix) const;

  // Create a Path iterator at a specific starting prefix
  PathIterator GetPathIterator(absl::string_view prefix) const;

  // Returns tree structure as vector of strings
  std::vector<std::string> DebugGetTreeStrings() const;

  // Prints tree structure
  void DebugPrintTree(const std::string& label = "") const;

 private:
  /*
   * This is the first iteration of a RadixTree. It will be optimized in the
   * future, likely with multiple different representations.
   *
   * Right now there are three types of nodes:
   *    1) Leaf node that has a target and no children.
   *    2) Branching node that has between 2 and 256 children and may or may not
   *       have a target.
   *    3) Compressed node that has a single child of one or more bytes and may
   *       or may not have a target.
   *
   * Differentiating nodes that have multiple children vs a single child takes
   * inspiration from Rax in the Valkey core. An alternative would be to merge
   * compressed and branching nodes into one:
   *
   * using NodeChildren = std::variant<
   *     std::monostate,                            // Leaf node
   *     std::map<BytePath, std::unique_ptr<Node>>  // Internal node
   * >;
   *
   * For example,
   *
   *                  [compressed]
   *                  "te" |
   *                   [branching]
   *                "s" /     \ "a"
   *          [compressed]   [compressed]
   *          "ting" /           \ "m"
   *   Target <- [leaf]           [leaf] -> Target
   *
   *  would become...
   *
   *                     [node]
   *                  "te" |
   *                     [node]
   *             "sting" /     \ "am"
   *       Target <- [leaf]    [leaf] -> Target
   *
   * There is one less level to the graph, but the complexity at the internal
   * nodes has increased and will be tricky to compress into a performant,
   * compact format given the varying sized outgoing edges. We'll consider the
   * implementations carefully when we return to optimize.
   *
   */

  rax* rax_;  // TODO(BRENNAN): Embed directly at the end after getting it
              // working
  void (*free_callback_)(void*);  // Optional callback for freeing targets

 public:
  //
  // The Word Iterator provides access to sequences of Words and the associated
  // Postings Object in lexical order. Currently the word iterator assumes the
  // radix tree is not mutated for the life of the iterator.
  //
  class WordIterator {
   public:
    // Constructor - seeks to prefix
    explicit WordIterator(rax* rax, absl::string_view prefix);

    // Destructor - cleans up iterator
    ~WordIterator();

    // Disable copy, enable move
    WordIterator(const WordIterator&) = delete;
    WordIterator& operator=(const WordIterator&) = delete;
    WordIterator(WordIterator&& other) noexcept;
    WordIterator& operator=(WordIterator&& other) noexcept;

    // Is iterator valid?
    bool Done() const;

    // Advance to next word in lexical order
    void Next();

    // Seek forward to the next word that's greater or equal to the specified
    // word. If the prefix of this word doesn't match the prefix that created
    // this iterator, then it immediately becomes invalid. The return boolean
    // indicates if the landing spot is equal to the specified word (true) or
    // greater (false).
    bool SeekForward(absl::string_view word);

    // Access the current location, asserts if !Done()
    absl::string_view GetWord() const;
    void* GetTarget() const;

    // Postings-specific accessor. Caller is responsible for tracking the type.
    InvasivePtr<Postings> GetPostingsTarget() const;

   private:
    friend class Rax;

    raxIterator iter_;
    std::string prefix_;
    bool valid_ = false;
  };

  //
  // The Path iterator is initialized with a prefix. It allows
  // iteration over the set of next valid characters for the prefix.
  // For each of those valid characters, the presence of a word or
  // a subtree can be interrogated.
  //
  class PathIterator {
    // Is the iterator itself pointing to a valid node?
    bool Done() const;

    // Is there a word at the current position?
    bool IsWord() const;

    // Advance to the next character at this level of the RadixTree
    void Next();

    // Seek to the char that's greater than or equal
    // returns true if target char is present, false otherwise
    bool SeekForward(char target);

    // Is there a node under the current path?
    bool CanDescend() const;

    // Create a new PathIterator automatically descending from the current
    // position asserts if !CanDescend()
    PathIterator DescendNew() const;

    // get current Path. If IsWord is true, then there's a word here....
    absl::string_view GetPath();

    // Get the target for this word, will assert if !IsWord()
    const void* GetTarget() const;

    // Defrag the current Node and then defrag the Postings if this points to
    // one.
    void Defrag();
  };
};

// // TODO: return the old pointer to the caller for both SetTarget and
// // MutateTarget
// // or maybe for SetTarget we use the same mutate API but the closure
// // has the new pointer to return. We simply destroy the old one

// // template <typename TargetPtr>
// // TargetPtr RadixTree<TargetPtr>::SetTarget(absl::string_view word,
// //                                           TargetPtr new_target) {
// //   CHECK(!word.empty()) << "Can't add the target for an empty word";
// //   void* old_ptr = nullptr;
// //   raxInsert(rax_, const_cast<unsigned char*>(word.data()), word.size(),
// //             reinterpret_cast<void*>(new_target), &old_ptr);
// //   return reinterpret_cast<TargetPtr>(old_ptr);
// // }

// void* Rax::MutateTarget(
//     absl::string_view word, absl::FunctionRef<TargetPtr(TargetPtr)> mutate) {
//   CHECK(!word.empty()) << "Can't mutate the target for an empty word";

//   MutateContext ctx{mutate, TargetPtr{}};

//   raxMutate(rax_,
//             const_cast<unsigned char*>(
//                 reinterpret_cast<const unsigned char*>(word.data())),
//             word.size(),
//             MutateCallback,
//             &ctx);

//   return ctx.result;
// }

// size_t Rax<TargetPtr>::GetTotalWordCount() const {
//   return raxSize(rax_);
// }

// size_t RadixTree<TargetPtr>::GetWordCount(absl::string_view prefix) const {
//   // TODO: Implement word counting
//   return 0;
// }

// size_t RadixTree<TargetPtr>::GetLongestWord() const {
//   // TODO: Implement longest word calculation
//   return 0;
// }

// Rax<TargetPtr>::WordIterator
// Rax<TargetPtr>::GetWordIterator(absl::string_view prefix) const {
//   return WordIterator(rax_, prefix);
// }

// // WordIterator implementation

// Rax<TargetPtr>::WordIterator::WordIterator(rax* rax, absl::string_view
// prefix)
//     : prefix_(prefix) {
//   raxStart(&iter_, rax);

//   // Seek to prefix with ">=" operator (works for empty prefix too)
//   valid_ = raxSeek(&iter_, ">=",
//                    const_cast<unsigned char*>(
//                        reinterpret_cast<const unsigned
//                        char*>(prefix.data())),
//                    prefix.size());

//   // Check if we're still in the prefix range
//   if (valid_ && !raxEOF(&iter_)) {
//     absl::string_view current_key(
//         reinterpret_cast<const char*>(iter_.key), iter_.key_len);
//     if (!current_key.starts_with(prefix)) {
//       valid_ = false;
//     }
//   } else {
//     valid_ = false;
//   }
// }

// Rax<TargetPtr>::WordIterator::~WordIterator() {
//   raxStop(&iter_);
// }

// Rax<TargetPtr>::WordIterator::WordIterator(WordIterator&& other) noexcept
//     : iter_(other.iter_), prefix_(std::move(other.prefix_)),
//     valid_(other.valid_) {
//   // Invalidate the source iterator
//   other.valid_ = false;
// }

// typename Rax<TargetPtr>::WordIterator&
// Rax<TargetPtr>::WordIterator::operator=(WordIterator&& other) noexcept {
//   if (this != &other) {
//     raxStop(&iter_);
//     iter_ = other.iter_;
//     prefix_ = std::move(other.prefix_);
//     valid_ = other.valid_;
//     other.valid_ = false;
//   }
//   return *this;
// }

// bool Rax<TargetPtr>::WordIterator::Done() const {
//   return !valid_ || raxEOF(&iter_);
// }

// void Rax<TargetPtr>::WordIterator::Next() {
//   if (Done()) return;

//   if (!raxNext(&iter_)) {
//     valid_ = false;
//     return;
//   }

//   // Check if still within prefix
//   if (!raxEOF(&iter_)) {
//     absl::string_view current_key(
//         reinterpret_cast<const char*>(iter_.key), iter_.key_len);
//     if (!current_key.starts_with(prefix_)) {
//       valid_ = false;
//     }
//   } else {
//     valid_ = false;
//   }
// }

// bool Rax<TargetPtr>::WordIterator::SeekForward(absl::string_view word) {
//   if (Done()) return false;

//   // Check if word matches prefix
//   if (!word.starts_with(prefix_)) {
//     valid_ = false;
//     return false;
//   }

//   // Seek to the word
//   if (!raxSeek(&iter_, ">=",
//                const_cast<unsigned char*>(
//                    reinterpret_cast<const unsigned char*>(word.data())),
//                word.size())) {
//     valid_ = false;
//     return false;
//   }

//   if (raxEOF(&iter_)) {
//     valid_ = false;
//     return false;
//   }

//   // Check if we're still in prefix range
//   absl::string_view current_key(
//       reinterpret_cast<const char*>(iter_.key), iter_.key_len);
//   if (!current_key.starts_with(prefix_)) {
//     valid_ = false;
//     return false;
//   }

//   // Return true if exact match, false if greater
//   return current_key == word;
// }

// absl::string_view Rax<TargetPtr>::WordIterator::GetWord() const {
//   CHECK(!Done()) << "Cannot get word from invalid iterator";
//   return absl::string_view(reinterpret_cast<const char*>(iter_.key),
//                           iter_.key_len);
// }

// TargetPtr Rax<TargetPtr>::WordIterator::GetTarget() const {
//   CHECK(!Done()) << "Cannot get target from invalid iterator";
//   return reinterpret_cast<TargetPtr>(iter_.data);
// }

// /*** PathIterator ***/

// bool RadixTree<TargetPtr>::PathIterator::Done() const {
//   throw std::logic_error("TODO");
// }

// bool RadixTree<TargetPtr>::PathIterator::IsWord() const {
//   throw std::logic_error("TODO");
// }

// void RadixTree<TargetPtr>::PathIterator::Next() {
//   throw std::logic_error("TODO");
// }

// bool RadixTree<TargetPtr>::PathIterator::SeekForward(char target) {
//   throw std::logic_error("TODO");
// }

// bool RadixTree<TargetPtr>::PathIterator::CanDescend() const {
//   throw std::logic_error("TODO");
// }

// typename RadixTree<TargetPtr>::PathIterator
// RadixTree<TargetPtr>::PathIterator::DescendNew() const {
//   throw std::logic_error("TODO");
// }

// absl::string_view RadixTree<TargetPtr>::PathIterator::GetPath() {
//   throw std::logic_error("TODO");
// }

// const TargetPtr& RadixTree<TargetPtr>::PathIterator::GetTarget() const {
//   throw std::logic_error("TODO");
// }

// void RadixTree<TargetPtr>::PathIterator::Defrag() {
//   throw std::logic_error("TODO");
// }

}  // namespace valkey_search::indexes::text

#endif
