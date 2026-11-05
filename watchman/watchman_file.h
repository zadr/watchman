/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "watchman/Clock.h"
#include "watchman/fs/FileInformation.h"
#include "watchman/watchman_dir.h"

#pragma pack(push, 4)
// ClockStamp without padding. The timestamp is unsigned 32-bit seconds,
// which holds the observation time until 2106.
struct CompactClockStamp {
  watchman::ClockTicks ticks{0};
  uint32_t timestamp{0};

  CompactClockStamp() = default;
  /* implicit */ CompactClockStamp(watchman::ClockStamp stamp)
      : ticks(stamp.ticks), timestamp(static_cast<uint32_t>(stamp.timestamp)) {}
  /* implicit */ operator watchman::ClockStamp() const {
    return watchman::ClockStamp{ticks, static_cast<time_t>(timestamp)};
  }
};
#pragma pack(pop)

struct watchman_file;

/**
 * Slab allocator for the watchman_file records of one view.
 *
 * A record is the struct plus its name, so sizes vary. malloc rounds them
 * up to the next size class, which above 128 bytes is 32 bytes wide on
 * recent macOS; the pool rounds to 8. Released records go on a free list
 * for their size. Slabs are only returned when the pool is destroyed.
 *
 * Not thread safe: the owning ViewDatabase serializes all calls under its
 * write lock.
 */
class FilePool {
 public:
  FilePool() = default;
  FilePool(const FilePool&) = delete;
  FilePool& operator=(const FilePool&) = delete;
  ~FilePool();

  void* allocate(size_t size);
  static void release(void* ptr, size_t size);

 private:
  struct Slab {
    FilePool* pool;
    Slab* next;
  };
  struct FreeBlock {
    FreeBlock* next;
  };

  // Power of two: a pooled record's slab is found by masking its address.
  static constexpr size_t kSlabSize = 64 * 1024;
  static constexpr size_t kGranule = 8;
  // Larger records (names over ~900 bytes) go straight to malloc.
  static constexpr size_t kMaxPooled = 1024;

  static size_t roundUp(size_t size) {
    return (size + kGranule - 1) & ~(kGranule - 1);
  }
  void pushFree(void* ptr, size_t size);

  FreeBlock* freeLists_[kMaxPooled / kGranule + 1]{};
  char* cursor_{nullptr};
  char* end_{nullptr};
  Slab* slabs_{nullptr};
};

struct watchman_file {
  /* the parent dir */
  watchman_dir* parent;

  /* linkage to files ordered by changed time.
   * prev points to the address of `next` in the
   * previous file node, or the head of the list. */
  struct watchman_file **prev, *next;

  /* the time we last observed a change to this file */
  CompactClockStamp otime;
  /* the time we first observed this file OR the time
   * that this file switched from !exists to exists.
   * This is thus the "created time" */
  CompactClockStamp ctime;

  /* cache stat results so we can tell if an entry
   * changed */
  watchman::FileInformation stat;

  /* whether we believe that this file still exists */
  bool exists;
  /* whether we think this file might not exist */
  bool maybe_deleted;

  /* the name is stored right after the struct, NUL terminated */
  uint16_t nameLength;

  inline w_string_piece getName() const {
    return w_string_piece(
        reinterpret_cast<const char*>(this + 1), nameLength);
  }

  void removeFromFileList();

  watchman_file() = delete;
  watchman_file(const watchman_file&) = delete;
  watchman_file& operator=(const watchman_file&) = delete;
  ~watchman_file();

  size_t allocationSize() const {
    return sizeof(watchman_file) + nameLength + 1;
  }

  static std::unique_ptr<watchman_file, watchman_dir::Deleter>
  make(const w_string& name, watchman_dir* parent, FilePool& pool);
};

void free_file_node(struct watchman_file* file);
