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
#include <limits>
#include <optional>
#include <vector>

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

namespace watchman {

#pragma pack(push, 4)

/**
 * A FileTime stored as nanoseconds since the epoch, which covers
 * 1677-09-21 to 2262-04-11 exactly. Any other value, or a tv_nsec outside
 * [0, 1e9), is kept exactly in a side table keyed by the field's address
 * and the field holds kSpilled; records never move, so the key is stable.
 *
 * A field and its side table entry change only under the owning view's
 * write lock, so a reader holding the read lock sees kSpilled exactly when
 * the entry exists. The table has its own lock because views are
 * independent.
 */
class PackedFileTime {
 public:
  PackedFileTime() = default;
  PackedFileTime(const PackedFileTime&) = delete;
  PackedFileTime& operator=(const PackedFileTime&) = delete;
  ~PackedFileTime() {
    if (ns_ == kSpilled) {
      unspill();
    }
  }

  PackedFileTime& operator=(FileTime t) {
    if (auto ns = encode(t)) {
      if (ns_ == kSpilled) {
        unspill();
      }
      ns_ = *ns;
    } else {
      spill(t);
    }
    return *this;
  }

  /* implicit */ operator FileTime() const {
    if (ns_ == kSpilled) {
      return spilled();
    }
    // Floor division, without forming sec * kNsPerSec, which can overflow.
    int64_t sec = ns_ / kNsPerSec;
    int64_t nsec = ns_ % kNsPerSec;
    if (nsec < 0) {
      sec -= 1;
      nsec += kNsPerSec;
    }
    FileTime t;
    t.tv_sec = sec;
    t.tv_nsec = static_cast<int32_t>(nsec);
    return t;
  }

  /* implicit */ operator struct timespec() const {
    return FileTime(*this);
  }

  bool isSpilled() const {
    return ns_ == kSpilled;
  }

  static std::optional<int64_t> encode(FileTime t) {
    if (t.tv_nsec < 0 || t.tv_nsec >= kNsPerSec) {
      return std::nullopt;
    }
    if (t.tv_sec > kMaxSec ||
        (t.tv_sec == kMaxSec && t.tv_nsec > kMaxSecNsec)) {
      return std::nullopt;
    }
    // kSpilled is INT64_MIN, so the lowest stored value is INT64_MIN + 1.
    if (t.tv_sec < kMinSec ||
        (t.tv_sec == kMinSec && t.tv_nsec <= kMinSecNsec)) {
      return std::nullopt;
    }
    if (t.tv_sec >= 0) {
      return t.tv_sec * kNsPerSec + t.tv_nsec;
    }
    return (t.tv_sec + 1) * kNsPerSec + (t.tv_nsec - kNsPerSec);
  }

  static size_t spilledCount();

 private:
  static constexpr int64_t kNsPerSec = 1000000000;
  static constexpr int64_t kSpilled = std::numeric_limits<int64_t>::min();
  // INT64_MAX is kMaxSec seconds and kMaxSecNsec nanoseconds.
  static constexpr int64_t kMaxSec =
      std::numeric_limits<int64_t>::max() / kNsPerSec;
  static constexpr int64_t kMaxSecNsec =
      std::numeric_limits<int64_t>::max() % kNsPerSec;
  // INT64_MIN is kMinSec seconds and kMinSecNsec nanoseconds.
  static constexpr int64_t kMinSec =
      std::numeric_limits<int64_t>::min() / kNsPerSec - 1;
  static constexpr int64_t kMinSecNsec =
      kNsPerSec + std::numeric_limits<int64_t>::min() % kNsPerSec;

  void spill(FileTime t);
  void unspill();
  FileTime spilled() const;

  int64_t ns_{0};
};
static_assert(sizeof(PackedFileTime) == 8);

/**
 * FileInformation as stored in a watchman_file, with each time in 8 bytes
 * rather than 12.
 */
struct StoredFileInformation {
  uint64_t size{0};
  uid_t uid{0};
  gid_t gid{0};
  ino_t ino{0};
  dev_t dev{0};
  mode_t mode{0};
  nlink_t nlink{0};
#ifdef _WIN32
  uint32_t fileAttributes{0};
#endif
  PackedFileTime atime;
  PackedFileTime mtime;
  PackedFileTime ctime;

  StoredFileInformation() = default;
  StoredFileInformation(const StoredFileInformation&) = delete;
  StoredFileInformation& operator=(const StoredFileInformation&) = delete;

  StoredFileInformation& operator=(const FileInformation& info);
  /* implicit */ operator FileInformation() const;

  DType dtype() const {
    return typeOnly().dtype();
  }
  bool isSymlink() const {
    return typeOnly().isSymlink();
  }
  bool isDir() const {
    return typeOnly().isDir();
  }
  bool isFile() const {
    return typeOnly().isFile();
  }

 private:
  FileInformation typeOnly() const {
    FileInformation info;
    info.mode = mode;
#ifdef _WIN32
    info.fileAttributes = fileAttributes;
#endif
    return info;
  }
};

#pragma pack(pop)

} // namespace watchman

struct watchman_file;

/**
 * Slab allocator for the watchman_file records of one view.
 *
 * A record is the struct plus its name, so sizes vary. malloc rounds them
 * up to the next size class, which above 128 bytes is 32 bytes wide on
 * recent macOS; the pool rounds to 8. Each slab serves a single size and
 * has its own free list. An empty slab's pages go straight back to the
 * kernel, so deleted trees give their memory back. (malloc keeps freed
 * 16KB blocks dirty, so slabs come from mmap rather than malloc.)
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
  struct FreeBlock {
    FreeBlock* next;
  };
  struct Slab {
    FilePool* pool;
    Slab* prev;
    Slab* next;
    FreeBlock* free;
    char* cursor;
    uint32_t live;
    uint32_t sizeClass;
  };
  // Slabs of one size class. Every slab is on exactly one list.
  struct SizeClass {
    Slab* available{nullptr};
    Slab* full{nullptr};
  };

  // Power of two: a pooled record's slab is found by masking its address.
  static constexpr size_t kSlabSize = 16 * 1024;
  static constexpr size_t kRegionSize = 1024 * 1024;
  static constexpr size_t kGranule = 8;
  // Larger records (names over ~900 bytes) go straight to malloc.
  static constexpr size_t kMaxPooled = 1024;

  static size_t roundUp(size_t size) {
    return (size + kGranule - 1) & ~(kGranule - 1);
  }
  static void unlink(Slab*& head, Slab* slab);
  static void push(Slab*& head, Slab* slab);
  void releaseToSlab(Slab* slab, void* ptr, size_t size);
  void* newSlabMemory();
  void freeSlabMemory(void* slab);

  SizeClass classes_[kMaxPooled / kGranule + 1];
#ifndef _WIN32
  std::vector<void*> regions_;
  std::vector<void*> emptySlabs_;
  char* regionCursor_{nullptr};
  char* regionEnd_{nullptr};
#endif
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
  watchman::StoredFileInformation stat;

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
