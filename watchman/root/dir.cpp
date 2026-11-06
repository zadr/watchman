/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/watchman_dir.h"
#include <limits>
#include "watchman/Logging.h"
#include "watchman/watchman_file.h"

void watchman_dir::Deleter::operator()(watchman_file* file) const {
  free_file_node(file);
}

void watchman_dir::Deleter::operator()(watchman_dir* dir) const {
  dir->~watchman_dir();
  ::operator delete(dir);
}

watchman_dir::watchman_dir(uint32_t nameLength, watchman_dir* parent)
    : parent(parent), nameLength(nameLength) {}

/* The name is embedded after the struct, saving a separate allocation. */
watchman_dir::DirPtr watchman_dir::make(
    w_string_piece name,
    watchman_dir* parent) {
  w_check(
      name.size() <= std::numeric_limits<uint32_t>::max(), "dir name too long");
  void* mem = ::operator new(sizeof(watchman_dir) + name.size() + 1);
  auto data = static_cast<char*>(mem) + sizeof(watchman_dir);
  memcpy(data, name.data(), name.size());
  data[name.size()] = 0;
  return DirPtr(
      new (mem) watchman_dir(static_cast<uint32_t>(name.size()), parent));
}

size_t watchman_dir::NameHash::operator()(const FilePtr& file) const noexcept {
  return file->getName().hashValue();
}

size_t watchman_dir::NameHash::operator()(const DirPtr& dir) const noexcept {
  return dir->getName().hashValue();
}

w_string_piece watchman_dir::NameEqual::nameOf(const FilePtr& file) {
  return file->getName();
}

w_string_piece watchman_dir::NameEqual::nameOf(const DirPtr& dir) {
  return dir->getName();
}

w_string watchman_dir::getFullPath() const {
  return getFullPathToChild(w_string_piece());
}

watchman_file* watchman_dir::getChildFile(w_string_piece name_2) const {
  auto it = files.find(name_2);
  if (it == files.end()) {
    return nullptr;
  }
  return it->get();
}

watchman_dir* watchman_dir::getChildDir(w_string_piece name_2) const {
  auto it = dirs.find(name_2);
  if (it == dirs.end()) {
    return nullptr;
  }
  return it->get();
}

w_string watchman_dir::getFullPathToChild(w_string_piece extra) const {
  uint32_t length = 0;
  if (extra.size()) {
    length = extra.size() + 1 /* separator */;
  }
  for (const watchman_dir* d = this; d; d = d->parent) {
    length += d->nameLength + 1 /* separator OR final NUL terminator */;
  }

  auto* s = watchman::StringHeader::alloc(length - 1, W_STRING_BYTE);

  char* buf = s->buf();
  char* end = buf + s->len;

  *end = 0;
  if (extra.size()) {
    end -= extra.size();
    memcpy(end, extra.data(), extra.size());
  }
  for (const watchman_dir* d = this; d; d = d->parent) {
    if (d != this || (extra.size())) {
      --end;
      *end = '/';
    }
    auto name = d->getName();
    end -= name.size();
    memcpy(end, name.data(), name.size());
  }

  return w_string{s};
}

/* vim:ts=2:sw=2:et:
 */
