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
#ifndef _WIN32
#include <sys/mman.h>
#endif
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
#ifdef _WIN32
  for (auto& sc : classes_) {
    for (auto list : {sc.available, sc.full}) {
      while (list) {
        auto next = list->next;
        folly::aligned_free(list);
        list = next;
      }
    }
  }
#else
  for (auto region : regions_) {
    munmap(region, kRegionSize + kSlabSize);
  }
#endif
}

void* FilePool::newSlabMemory() {
#ifdef _WIN32
  auto slab = folly::aligned_malloc(kSlabSize, kSlabSize);
  if (!slab) {
    throw std::bad_alloc();
  }
  return slab;
#else
  if (!emptySlabs_.empty()) {
    auto slab = emptySlabs_.back();
    emptySlabs_.pop_back();
#ifdef MADV_FREE_REUSE
    madvise(slab, kSlabSize, MADV_FREE_REUSE);
#endif
    return slab;
  }
  if (regionCursor_ == regionEnd_) {
    // Over-reserve by a slab so the start can be aligned; the unused part is
    // never touched.
    auto region = mmap(
        nullptr,
        kRegionSize + kSlabSize,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON,
        -1,
        0);
    if (region == MAP_FAILED) {
      throw std::bad_alloc();
    }
    regions_.push_back(region);
    regionCursor_ = reinterpret_cast<char*>(
        (reinterpret_cast<uintptr_t>(region) + kSlabSize - 1) &
        ~(uintptr_t(kSlabSize) - 1));
    regionEnd_ = regionCursor_ + kRegionSize;
  }
  auto slab = regionCursor_;
  regionCursor_ += kSlabSize;
  return slab;
#endif
}

void FilePool::freeSlabMemory(void* slab) {
#ifdef _WIN32
  folly::aligned_free(slab);
#else
#ifdef MADV_FREE_REUSABLE
  madvise(slab, kSlabSize, MADV_FREE_REUSABLE);
#else
  madvise(slab, kSlabSize, MADV_DONTNEED);
#endif
  emptySlabs_.push_back(slab);
#endif
}

void FilePool::unlink(Slab*& head, Slab* slab) {
  if (slab->prev) {
    slab->prev->next = slab->next;
  } else {
    head = slab->next;
  }
  if (slab->next) {
    slab->next->prev = slab->prev;
  }
  slab->prev = slab->next = nullptr;
}

void FilePool::push(Slab*& head, Slab* slab) {
  slab->prev = nullptr;
  slab->next = head;
  if (head) {
    head->prev = slab;
  }
  head = slab;
}

// Free and unallocated space is ASan-poisoned so use-after-free of a record
// is still reported.
void* FilePool::allocate(size_t size) {
  size = roundUp(size);
  if (size > kMaxPooled) {
    return malloc(size);
  }
  auto& sc = classes_[size / kGranule];
  auto slab = sc.available;
  if (!slab) {
    slab = static_cast<Slab*>(newSlabMemory());
    slab->pool = this;
    slab->free = nullptr;
    slab->cursor = reinterpret_cast<char*>(slab) + roundUp(sizeof(Slab));
    slab->live = 0;
    slab->sizeClass = static_cast<uint32_t>(size / kGranule);
    folly::asan_poison_memory_region(
        slab->cursor, reinterpret_cast<char*>(slab) + kSlabSize - slab->cursor);
    push(sc.available, slab);
  }

  void* ptr;
  if (slab->free) {
    ptr = slab->free;
    folly::asan_unpoison_memory_region(ptr, size);
    slab->free = slab->free->next;
  } else {
    ptr = slab->cursor;
    slab->cursor += size;
    folly::asan_unpoison_memory_region(ptr, size);
  }
  slab->live++;

  auto end = reinterpret_cast<char*>(slab) + kSlabSize;
  if (!slab->free && static_cast<size_t>(end - slab->cursor) < size) {
    unlink(sc.available, slab);
    push(sc.full, slab);
  }
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
  slab->pool->releaseToSlab(slab, ptr, size);
}

void FilePool::releaseToSlab(Slab* slab, void* ptr, size_t size) {
  auto& sc = classes_[slab->sizeClass];
  auto end = reinterpret_cast<char*>(slab) + kSlabSize;
  bool wasFull = !slab->free && static_cast<size_t>(end - slab->cursor) < size;

  auto block = static_cast<FreeBlock*>(ptr);
  block->next = slab->free;
  slab->free = block;
  folly::asan_poison_memory_region(ptr, size);

  if (--slab->live == 0) {
    unlink(wasFull ? sc.full : sc.available, slab);
    folly::asan_unpoison_memory_region(slab, kSlabSize);
    freeSlabMemory(slab);
  } else if (wasFull) {
    unlink(sc.full, slab);
    push(sc.available, slab);
  }
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
