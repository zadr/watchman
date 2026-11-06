/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GTest.h>

#include "watchman/watchman_file.h"

using namespace watchman;

namespace {

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();

FileTime makeTime(int64_t sec, int32_t nsec) {
  FileTime t;
  t.tv_sec = sec;
  t.tv_nsec = nsec;
  return t;
}

void expectSameTime(FileTime expected, FileTime actual) {
  EXPECT_EQ(expected.tv_sec, actual.tv_sec);
  EXPECT_EQ(expected.tv_nsec, actual.tv_nsec);
}

// Stores t, checks it reads back exactly, and returns whether it spilled.
bool storeAndCheck(int64_t sec, int32_t nsec) {
  auto before = PackedFileTime::spilledCount();
  bool spilled;
  {
    PackedFileTime packed;
    packed = makeTime(sec, nsec);
    expectSameTime(makeTime(sec, nsec), packed);
    struct timespec ts = packed;
    EXPECT_EQ(sec, int64_t(ts.tv_sec));
    EXPECT_EQ(nsec, ts.tv_nsec);
    spilled = packed.isSpilled();
    EXPECT_EQ(before + (spilled ? 1 : 0), PackedFileTime::spilledCount());
  }
  EXPECT_EQ(before, PackedFileTime::spilledCount());
  return spilled;
}

} // namespace

TEST(PackedFileTimeTest, encodes_nanoseconds_since_epoch) {
  EXPECT_EQ(0, PackedFileTime::encode(makeTime(0, 0)));
  EXPECT_EQ(999999999, PackedFileTime::encode(makeTime(0, 999999999)));
  EXPECT_EQ(
      1500000000000000007, PackedFileTime::encode(makeTime(1500000000, 7)));
  EXPECT_EQ(-1000000000, PackedFileTime::encode(makeTime(-1, 0)));
  EXPECT_EQ(-1, PackedFileTime::encode(makeTime(-1, 999999999)));
  EXPECT_EQ(-4999999995, PackedFileTime::encode(makeTime(-5, 5)));
}

TEST(PackedFileTimeTest, encodes_the_range_limits) {
  // INT64_MAX nanoseconds, 2262-04-11.
  EXPECT_EQ(kInt64Max, PackedFileTime::encode(makeTime(9223372036, 854775807)));
  EXPECT_EQ(
      std::nullopt, PackedFileTime::encode(makeTime(9223372036, 854775808)));
  EXPECT_EQ(std::nullopt, PackedFileTime::encode(makeTime(9223372037, 0)));
  // INT64_MIN nanoseconds, 1677-09-21, is the spill marker.
  EXPECT_EQ(
      kInt64Min + 1, PackedFileTime::encode(makeTime(-9223372037, 145224193)));
  EXPECT_EQ(
      std::nullopt, PackedFileTime::encode(makeTime(-9223372037, 145224192)));
  EXPECT_EQ(
      std::nullopt, PackedFileTime::encode(makeTime(-9223372037, 145224191)));
  EXPECT_EQ(
      std::nullopt, PackedFileTime::encode(makeTime(-9223372038, 999999999)));
}

TEST(PackedFileTimeTest, round_trips_inline) {
  EXPECT_FALSE(storeAndCheck(0, 0));
  EXPECT_FALSE(storeAndCheck(0, 999999999));
  EXPECT_FALSE(storeAndCheck(1, 0));
  EXPECT_FALSE(storeAndCheck(1700000000, 123456789));
  EXPECT_FALSE(storeAndCheck(-1, 0));
  EXPECT_FALSE(storeAndCheck(-1, 999999999));
  EXPECT_FALSE(storeAndCheck(-1700000000, 1));
  EXPECT_FALSE(storeAndCheck(9223372036, 854775807));
  EXPECT_FALSE(storeAndCheck(9223372036, 0));
  EXPECT_FALSE(storeAndCheck(-9223372037, 145224193));
  EXPECT_FALSE(storeAndCheck(-9223372037, 999999999));
}

TEST(PackedFileTimeTest, round_trips_through_the_spill_table) {
  EXPECT_TRUE(storeAndCheck(9223372036, 854775808));
  EXPECT_TRUE(storeAndCheck(9223372036, 999999999));
  EXPECT_TRUE(storeAndCheck(-9223372037, 145224192));
  EXPECT_TRUE(storeAndCheck(-9223372037, 0));
  EXPECT_TRUE(storeAndCheck(kInt64Max, 0));
  EXPECT_TRUE(storeAndCheck(kInt64Max, 999999999));
  EXPECT_TRUE(storeAndCheck(kInt64Min, 0));
  EXPECT_TRUE(storeAndCheck(kInt64Min, 999999999));
  EXPECT_TRUE(storeAndCheck(0, -1));
  EXPECT_TRUE(storeAndCheck(0, 1000000000));
  EXPECT_TRUE(storeAndCheck(-1, std::numeric_limits<int32_t>::min()));
  EXPECT_TRUE(storeAndCheck(1, std::numeric_limits<int32_t>::max()));
}

TEST(PackedFileTimeTest, reassignment_moves_between_inline_and_spilled) {
  auto before = PackedFileTime::spilledCount();
  PackedFileTime packed;
  expectSameTime(makeTime(0, 0), packed);

  packed = makeTime(kInt64Max, 1);
  EXPECT_TRUE(packed.isSpilled());
  EXPECT_EQ(before + 1, PackedFileTime::spilledCount());

  packed = makeTime(kInt64Min, 2);
  EXPECT_TRUE(packed.isSpilled());
  EXPECT_EQ(before + 1, PackedFileTime::spilledCount());
  expectSameTime(makeTime(kInt64Min, 2), packed);

  packed = makeTime(42, 3);
  EXPECT_FALSE(packed.isSpilled());
  EXPECT_EQ(before, PackedFileTime::spilledCount());
  expectSameTime(makeTime(42, 3), packed);
}

TEST(WatchmanFileTest, stat_round_trips) {
  FilePool pool;
  auto file = watchman_file::make(w_string("a"), nullptr, pool);
  expectSameTime(makeTime(0, 0), file->stat.mtime);

  FileInformation info;
  info.size = 1234567890123;
  info.uid = 501;
  info.gid = 20;
  info.ino = 0x123456789abcdef0;
  info.dev = 16777234;
  info.mode = S_IFREG | 0644;
  info.nlink = 3;
  info.atime = makeTime(1700000000, 1);
  info.mtime = makeTime(-1, 999999999);
  info.ctime = makeTime(0, 0);
  file->stat = info;

  FileInformation out = file->stat;
  EXPECT_EQ(info.size, out.size);
  EXPECT_EQ(info.uid, out.uid);
  EXPECT_EQ(info.gid, out.gid);
  EXPECT_EQ(info.ino, out.ino);
  EXPECT_EQ(info.dev, out.dev);
  EXPECT_EQ(info.mode, out.mode);
  EXPECT_EQ(info.nlink, out.nlink);
  expectSameTime(info.atime, out.atime);
  expectSameTime(info.mtime, out.mtime);
  expectSameTime(info.ctime, out.ctime);
  EXPECT_TRUE(file->stat.isFile());
  EXPECT_FALSE(file->stat.isDir());
  EXPECT_EQ(DType::Regular, file->stat.dtype());
}

TEST(WatchmanFileTest, spilled_times_are_released_with_the_record) {
  auto before = PackedFileTime::spilledCount();
  FilePool pool;
  auto spilled = watchman_file::make(w_string("spilled"), nullptr, pool);
  auto other = watchman_file::make(w_string("other"), nullptr, pool);

  FileInformation info;
  info.atime = makeTime(kInt64Max, 999999999);
  info.mtime = makeTime(kInt64Min, 0);
  info.ctime = makeTime(1700000000, 5);
  spilled->stat = info;
  other->stat = info;
  EXPECT_TRUE(spilled->stat.atime.isSpilled());
  EXPECT_TRUE(spilled->stat.mtime.isSpilled());
  EXPECT_FALSE(spilled->stat.ctime.isSpilled());
  EXPECT_EQ(before + 4, PackedFileTime::spilledCount());

  other->stat.mtime = makeTime(7, 8);
  EXPECT_EQ(before + 3, PackedFileTime::spilledCount());

  FileInformation out = spilled->stat;
  expectSameTime(info.atime, out.atime);
  expectSameTime(info.mtime, out.mtime);
  expectSameTime(info.ctime, out.ctime);

  spilled.reset();
  EXPECT_EQ(before + 1, PackedFileTime::spilledCount());
  expectSameTime(info.atime, other->stat.atime);
  expectSameTime(makeTime(7, 8), other->stat.mtime);

  // A new record at the freed address starts inline, at the epoch.
  auto reused = watchman_file::make(w_string("spilled"), nullptr, pool);
  EXPECT_FALSE(reused->stat.atime.isSpilled());
  expectSameTime(makeTime(0, 0), reused->stat.atime);

  other.reset();
  EXPECT_EQ(before, PackedFileTime::spilledCount());
}
