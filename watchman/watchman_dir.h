/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <folly/container/F14Set.h>
#include <memory>
#include "watchman/watchman_string.h"

struct watchman_file;

struct watchman_dir {
  /* the name of this dir, relative to its parent
   * for root (parent == nullptr), name is usually an absolute path */
  w_string name;
  /* the parent dir */
  watchman_dir* parent;

  struct Deleter {
    void operator()(watchman_file*) const;
  };
  using FilePtr = std::unique_ptr<watchman_file, Deleter>;
  using DirPtr = std::unique_ptr<watchman_dir>;

  /**
   * Hashes and compares entries by the name each one stores, and looks them
   * up by name, so the tables don't hold a separate copy of every key.
   */
  struct NameHash {
    using is_transparent = void;
    size_t operator()(w_string_piece name) const noexcept {
      return name.hashValue();
    }
    size_t operator()(const FilePtr& file) const noexcept;
    size_t operator()(const DirPtr& dir) const noexcept;
  };
  struct NameEqual {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const noexcept {
      return nameOf(a) == nameOf(b);
    }

   private:
    static w_string_piece nameOf(w_string_piece name) {
      return name;
    }
    static w_string_piece nameOf(const FilePtr& file);
    static w_string_piece nameOf(const DirPtr& dir);
  };

  /* files contained in this dir, by file->getName() */
  folly::F14ValueSet<FilePtr, NameHash, NameEqual> files;

  /* child dirs contained in this dir, by dir->name */
  folly::F14ValueSet<DirPtr, NameHash, NameEqual> dirs;

  // If we think this dir was deleted, we'll avoid recursing
  // to its children when processing deletes.
  bool last_check_existed{true};

  watchman_dir(w_string name, watchman_dir* parent);

  watchman_dir* getChildDir(w_string_piece name) const;

  /**
   * Returns the direct child file named name, or nullptr if there is no such
   * entry.
   */
  watchman_file* getChildFile(w_string_piece name) const;

  /**
   * Walk up to the chain of dirs via ->parent to and then produce the full path
   * to this dir.
   *
   * Practically, the root watchman_dir's name is often set to an absolute path
   * of the root, so this function returns an absolute path of the dir.
   *
   * If the root watchman_dir has an empty name, then this function returns
   * a path relative to the root of the watch.
   */
  w_string getFullPath() const;

  /**
   * Compute the full path to this dir and concatenate child with it, to produce
   * the path to the child.
   */
  w_string getFullPathToChild(w_string_piece child) const;
};
