/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include <iostream>
#include <set>
#include <string>

#include "gtest/gtest.h"
#include "src/indexes/text/rax_wrapper.h"

namespace valkey_search::indexes::text {

TEST(RaxIteratorTest, BasicIteration) {
  Rax tree;
  
  // Insert some test words with dummy pointers
  std::set<std::string> inserted_words = {"word0", "word1", "word2", "word3", "word4"};
  
  for (const auto& word : inserted_words) {
    tree.MutateTarget(word, [](void* old_val) {
      return reinterpret_cast<void*>(0x12345678);  // dummy pointer
    });
  }
  
  std::cout << "=== Starting iteration ===" << std::endl;
  
  // Iterate and collect words
  std::set<std::string> iterated_words;
  auto iter = tree.GetWordIterator("");
  int iteration = 0;
  
  while (!iter.Done()) {
    std::string word = std::string(iter.GetWord());
    void* target = iter.GetTarget();
    
    std::cout << "Iteration " << iteration << ": word='" << word 
              << "' ptr=" << target << std::endl;
    
    // Check for duplicates
    EXPECT_TRUE(iterated_words.insert(word).second) 
        << "Word '" << word << "' seen twice!";
    
    iteration++;
    iter.Next();
  }
  
  std::cout << "=== Iteration complete ===" << std::endl;
  std::cout << "Total iterations: " << iteration << std::endl;
  std::cout << "Unique words seen: " << iterated_words.size() << std::endl;
  
  // Verify we saw all words exactly once
  EXPECT_EQ(iterated_words.size(), inserted_words.size()) 
      << "Should see all inserted words";
  EXPECT_EQ(iterated_words, inserted_words) 
      << "Should see exactly the words we inserted";
}

TEST(RaxIteratorTest, SingleWordIteration) {
  Rax tree;
  
  tree.MutateTarget("word0", [](void* old_val) {
    return reinterpret_cast<void*>(0x12345678);
  });
  
  std::cout << "=== Single word iteration ===" << std::endl;
  
  auto iter = tree.GetWordIterator("");
  int count = 0;
  
  while (!iter.Done()) {
    std::cout << "Iteration " << count << ": word='" << iter.GetWord() << "'" << std::endl;
    count++;
    iter.Next();
  }
  
  std::cout << "Total iterations: " << count << std::endl;
  
  EXPECT_EQ(count, 1) << "Should iterate exactly once for single word";
}

}  // namespace valkey_search::indexes::text
