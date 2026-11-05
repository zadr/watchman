/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/watchman_file.h"
#include <folly/Memory.h>
#include <folly/memory/SanitizeAddress.h>
#include <limits>
#include "watchman/Logging.h"
#ifdef __APPLE__
#include <sys/attr.h> // @manual
#endif

void watchman_file::removeFromFileList() {
  if (next) {
    next->prev = prev;
  }
  // file->prev points to the address of either
  // `previous_file->next` OR `root->inner.latest_file`.
  // This next assignment is therefore fixing up either
  // the linkage from the prior file node or from the
  // head of the list.
  if (prev) {
    *prev = next;
  }
}

FilePool::~FilePool() {
  while (slabs_) {
    auto next = slabs_->next;
    folly::aligned_free(slabs_);
    slabs_ = next;
  }
}

// Free and unallocated space is ASan-poisoned so use-after-free of a record
// is still reported.
void FilePool::pushFree(void* ptr, size_t size) {
  folly::asan_unpoison_memory_region(ptr, sizeof(FreeBlock));
  auto block = static_cast<FreeBlock*>(ptr);
  block->next = freeLists_[size / kGranule];
  freeLists_[size / kGranule] = block;
  folly::asan_poison_memory_region(ptr, size);
}

void* FilePool::allocate(size_t size) {
  size = roundUp(size);
  if (size > kMaxPooled) {
    return malloc(size);
  }
  if (auto block = freeLists_[size / kGranule]) {
    folly::asan_unpoison_memory_region(block, size);
    freeLists_[size / kGranule] = block->next;
    return block;
  }
  if (static_cast<size_t>(end_ - cursor_) < size) {
    // Keep the tail of the current slab if a record could still fit there.
    auto tail = static_cast<size_t>(end_ - cursor_);
    if (tail >= roundUp(sizeof(watchman_file) + 1)) {
      pushFree(cursor_, tail);
    }
    auto slab = static_cast<Slab*>(folly::aligned_malloc(kSlabSize, kSlabSize));
    if (!slab) {
      throw std::bad_alloc();
    }
    slab->pool = this;
    slab->next = slabs_;
    slabs_ = slab;
    cursor_ = reinterpret_cast<char*>(slab) + roundUp(sizeof(Slab));
    end_ = reinterpret_cast<char*>(slab) + kSlabSize;
    folly::asan_poison_memory_region(cursor_, end_ - cursor_);
  }
  auto ptr = cursor_;
  cursor_ += size;
  folly::asan_unpoison_memory_region(ptr, size);
  return ptr;
}

void FilePool::release(void* ptr, size_t size) {
  size = roundUp(size);
  if (size > kMaxPooled) {
    free(ptr);
    return;
  }
  auto slab = reinterpret_cast<Slab*>(
      reinterpret_cast<uintptr_t>(ptr) & ~(uintptr_t(kSlabSize) - 1));
  slab->pool->pushFree(ptr, size);
}

/* The name is embedded after the struct, saving a separate allocation. */
std::unique_ptr<watchman_file, watchman_dir::Deleter>
watchman_file::make(const w_string& name, watchman_dir* parent, FilePool& pool) {
  w_check(
      name.size() <= std::numeric_limits<uint16_t>::max(),
      "file name too long");
  auto size = sizeof(watchman_file) + name.size() + 1;
  auto file = static_cast<watchman_file*>(pool.allocate(size));
  memset(static_cast<void*>(file), 0, size);
  std::unique_ptr<watchman_file, watchman_dir::Deleter> filePtr(
      file, watchman_dir::Deleter());

  file->nameLength = static_cast<uint16_t>(name.size());

  auto data = (char*)(file + 1);
  memcpy(data, name.data(), name.size());
  data[name.size()] = 0;

  file->parent = parent;
  file->exists = true;

  return filePtr;
}

watchman_file::~watchman_file() {
  removeFromFileList();
}

void free_file_node(struct watchman_file* file) {
  auto size = file->allocationSize();
  file->~watchman_file();
  FilePool::release(file, size);
}

/* vim:ts=2:sw=2:et:
 */
