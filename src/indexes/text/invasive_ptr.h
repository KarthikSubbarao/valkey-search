/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEY_SEARCH_INDEXES_TEXT_INVASIVE_PTR_H_
#define VALKEY_SEARCH_INDEXES_TEXT_INVASIVE_PTR_H_

#include <atomic>
#include <cstdint>
#include <utility>
#include <iostream> // <--- Added for std::cout (and assumed vmsdk/src/log.h doesn't hide it)
#include <sanitizer/common_interface_defs.h>

#include "vmsdk/src/log.h"

namespace valkey_search::indexes::text {

/**
 * ... (Doc comments omitted for brevity)
 */
template <typename T>
class InvasivePtr {
 public:
  struct RefCountWrapper {
    template <typename... Args>
    explicit RefCountWrapper(Args &&...args)
        : data_(std::forward<Args>(args)...) {}

    std::atomic<uint32_t> refcount_ = 1;
    T data_;
  };

  InvasivePtr() = default;

  InvasivePtr(std::nullptr_t) noexcept : ptr_(nullptr) {}

  // Factory constructor
  template <typename... Args>
  static InvasivePtr Make(Args &&...args) {
    InvasivePtr result;
    result.ptr_ = new RefCountWrapper(std::forward<Args>(args)...);
    return result;
  }

  // Adopts a raw RefCountWrapper pointer without modifying its reference count.
  // Every Release() should be paired with a corresponding AdoptRaw() later to
  // restore safe memory management.
  static InvasivePtr AdoptRaw(RefCountWrapper *wrapper) {
    return InvasivePtr(wrapper);
  }

  // Creates a new shared reference from a raw pointer, incrementing the
  // reference count. Use this when copying from void* storage (like Rax tree
  // targets) where you need a new managed reference.
  static InvasivePtr CopyRaw(RefCountWrapper *wrapper) {
    if (!wrapper) {
      return InvasivePtr{};
    }
    InvasivePtr result;
    result.ptr_ = wrapper;
    result.AddRef(); // <--- This is fine because it's on 'result', not 'this'
    return result;
  }

  ~InvasivePtr() { this->ReleaseRef(); } // <-- FIX: Added this->

  // Copy semantics
  InvasivePtr(const InvasivePtr &other) : ptr_(other.ptr_) { this->AddRef(); } // <-- FIX: Added this->

  InvasivePtr &operator=(const InvasivePtr &other) {
    if (this != &other) {
      this->ReleaseRef(); // <-- FIX: Added this->
      ptr_ = other.ptr_;
      this->AddRef();     // <-- FIX: Added this->
    }
    return *this;
  }

  InvasivePtr &operator=(std::nullptr_t) noexcept {
    this->Clear(); // <--- Clear() calls ReleaseRef, so this is fine
    return *this;
  }

  // Move semantics
  InvasivePtr(InvasivePtr &&other) noexcept : ptr_(other.ptr_) {
    other.ptr_ = nullptr;
    // NOTE: This constructor DOES NOT call ReleaseRef() on *this* because 'this'
    // is being initialized and does not own an object yet. The original version
    // of your code was correct here. (If you saw an error here earlier, it was 
    // likely a remnant or from another spot).
  }

  InvasivePtr &operator=(InvasivePtr &&other) noexcept {
    if (this != &other) {
      this->ReleaseRef(); // <-- FIX: Added this->
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  // Access operators
  T &operator*() const { return ptr_->data_; }
  T *operator->() const { return &ptr_->data_; }

  // Boolean conversion
  explicit operator bool() const { return ptr_ != nullptr; }

  // Comparison operators
  auto operator<=>(const InvasivePtr &) const = default;

  // Resets to the default nullptr state
  void Clear() {
    this->ReleaseRef(); // <-- FIX: Added this->
    ptr_ = nullptr;
  }

  // Transfers ownership to caller without decrementing refcount.
  // Caller must reconstruct via AdoptRaw() to restore memory management.
  // Freeing the memory directly is very dangerous - you must be certain there
  // are no other references.
  RefCountWrapper *ReleaseRaw() && {
    RefCountWrapper *result = ptr_;
    ptr_ = nullptr;
    return result;
  }

 private:
  explicit InvasivePtr(RefCountWrapper *wrapper) : ptr_(wrapper) {}

void ReleaseRef() {
  // std::cout << "ReleaseRef() ptr_=" << static_cast<void*>(ptr_);
  if (ptr_) {
    int old_count = ptr_->refcount_.load(std::memory_order_relaxed);
    // std::cout << " refcount=" << old_count;
    int new_count = ptr_->refcount_.fetch_sub(1, std::memory_order_acq_rel);
    // std::cout << " -> " << (new_count - 1);
    // if (new_count == 1) {
    //   std::cout << " [DELETING]";
    // }
  }
  // std::cout << std::endl;
  // __sanitizer_print_stack_trace();
  if (ptr_ && ptr_->refcount_.load(std::memory_order_relaxed) == 0) {
    delete ptr_;
  }
}

void AddRef() {
  // std::cout << "AddRef() ptr_=" << static_cast<void*>(ptr_);
  if (ptr_) {
    int old_count = ptr_->refcount_.load(std::memory_order_relaxed);
    // std::cout << " refcount=" << old_count;
    ptr_->refcount_.fetch_add(1, std::memory_order_relaxed);
    // std::cout << " -> " << (old_count + 1);
  }
  // std::cout << std::endl;
  // __sanitizer_print_stack_trace();
}

  RefCountWrapper *ptr_ = nullptr;
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_INVASIVE_PTR_H_
