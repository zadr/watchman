/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/fs/FileSystem.h"
#include <folly/testing/TestUtil.h>
#include <folly/portability/GTest.h>
#include <fstream>
#include <optional>
#include <set>
#include <system_error>
#include "watchman/Errors.h"
#include "watchman/fs/DirHandle.h"
#include "watchman/fs/FSDetect.h"

#ifndef _WIN32

using namespace watchman;

namespace {

class GetFileInformationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // realpath: the IO thread only ever passes canonical paths, and macOS
    // temp dirs live under the /var -> /private/var symlink.
    root_ = realPath(tmp_.path().string().c_str()).string();
    mkdirOrThrow(root_ + "/Dir");
    writeFile(root_ + "/Dir/File.txt");
    symlinkOrThrow("Dir", root_ + "/DirLink");
    symlinkOrThrow("File.txt", root_ + "/Dir/FileLink");
    cs_ = getCaseSensitivityForPath(root_.c_str());
  }

  static void mkdirOrThrow(const std::string& path) {
    if (mkdir(path.c_str(), 0755)) {
      throw std::system_error(errno, std::generic_category(), path);
    }
  }

  static void symlinkOrThrow(const char* target, const std::string& path) {
    if (symlink(target, path.c_str())) {
      throw std::system_error(errno, std::generic_category(), path);
    }
  }

  static void writeFile(const std::string& path) {
    std::ofstream(path) << "x";
  }

  // Checks that the throwing and error_code variants agree.
  std::error_code statError(const std::string& path) {
    std::error_code thrown;
    try {
      getFileInformation(path.c_str(), cs_);
    } catch (const std::system_error& e) {
      thrown = e.code();
    }
    auto result = tryGetFileInformation(path.c_str(), cs_);
    std::error_code returned = result.hasError() ? result.error() : std::error_code{};
    EXPECT_EQ(thrown, returned) << path;
    return returned;
  }

  folly::test::TemporaryDirectory tmp_{"wm-fs"};
  std::string root_;
  CaseSensitivity cs_;
};

} // namespace

TEST_F(GetFileInformationTest, file) {
  auto info = getFileInformation((root_ + "/Dir/File.txt").c_str(), cs_);
  EXPECT_TRUE(info.isFile());
  EXPECT_EQ(1, info.size);
}

TEST_F(GetFileInformationTest, directory) {
  auto info = getFileInformation((root_ + "/Dir").c_str(), cs_);
  EXPECT_TRUE(info.isDir());
}

TEST_F(GetFileInformationTest, leafSymlinkIsNotFollowed) {
  auto info = getFileInformation((root_ + "/Dir/FileLink").c_str(), cs_);
  EXPECT_TRUE(info.isSymlink());
}

TEST_F(GetFileInformationTest, missing) {
  auto err = statError(root_ + "/nope");
  EXPECT_EQ(std::errc::no_such_file_or_directory, err);
  EXPECT_EQ(error_code::no_such_file_or_directory, err);
  // The IO thread crawls the parent only for name mismatches.
  EXPECT_NE(&name_mismatch_category(), &err.category());
}

TEST_F(GetFileInformationTest, symlinkedParentIsRejected) {
#ifdef __APPLE__
  // Reported as gone so the IO thread prunes it. The generic path returns
  // ELOOP from open(O_NOFOLLOW), which the IO thread logs and ignores.
  EXPECT_EQ(
      std::errc::no_such_file_or_directory,
      statError(root_ + "/DirLink/File.txt"));
#else
  EXPECT_TRUE(statError(root_ + "/DirLink/File.txt"));
#endif
}

TEST_F(GetFileInformationTest, symlinkedAncestorIsRejected) {
  mkdirOrThrow(root_ + "/Dir/Sub");
  writeFile(root_ + "/Dir/Sub/Leaf");
  EXPECT_EQ(
      std::errc::no_such_file_or_directory,
      statError(root_ + "/DirLink/Sub/Leaf"));
}

TEST_F(GetFileInformationTest, nonCanonicalLeafCaseIsRejected) {
  if (cs_ != CaseSensitivity::CaseInSensitive) {
    GTEST_SKIP() << "filesystem is case-sensitive";
  }
  auto err = statError(root_ + "/Dir/file.txt");
  EXPECT_EQ(std::errc::no_such_file_or_directory, err);
  EXPECT_EQ(error_code::no_such_file_or_directory, err);
  EXPECT_EQ(&name_mismatch_category(), &err.category());
}

TEST_F(GetFileInformationTest, nonCanonicalParentCaseIsRejected) {
  if (cs_ != CaseSensitivity::CaseInSensitive) {
    GTEST_SKIP() << "filesystem is case-sensitive";
  }
  auto err = statError(root_ + "/dir/File.txt");
  EXPECT_EQ(std::errc::no_such_file_or_directory, err);
  EXPECT_EQ(error_code::no_such_file_or_directory, err);
  EXPECT_EQ(&name_mismatch_category(), &err.category());
}

TEST_F(GetFileInformationTest, nonCanonicalAncestorCaseIsRejected) {
  if (cs_ != CaseSensitivity::CaseInSensitive) {
    GTEST_SKIP() << "filesystem is case-sensitive";
  }
  mkdirOrThrow(root_ + "/Dir/Sub");
  writeFile(root_ + "/Dir/Sub/Leaf");
  auto err = statError(root_ + "/dir/Sub/Leaf");
  EXPECT_EQ(std::errc::no_such_file_or_directory, err);
  EXPECT_EQ(&name_mismatch_category(), &err.category());
}

TEST_F(GetFileInformationTest, hardLinkedFileFromEitherName) {
  mkdirOrThrow(root_ + "/Other");
  if (link((root_ + "/Dir/File.txt").c_str(), (root_ + "/Other/Link").c_str())) {
    throw std::system_error(errno, std::generic_category(), "link");
  }
  for (auto name : {"/Dir/File.txt", "/Other/Link", "/Dir/File.txt"}) {
    EXPECT_FALSE(statError(root_ + name)) << name;
  }
}

namespace {
void expectSameAsLstat(const std::string& path, CaseSensitivity cs) {
  struct stat st;
  ASSERT_EQ(0, lstat(path.c_str(), &st)) << path;
  FileInformation want(st);
  auto got = getFileInformation(path.c_str(), cs);
  EXPECT_EQ(want.mode, got.mode) << path;
  EXPECT_EQ(want.size, got.size) << path;
  EXPECT_EQ(want.uid, got.uid) << path;
  EXPECT_EQ(want.gid, got.gid) << path;
  EXPECT_EQ(want.ino, got.ino) << path;
  EXPECT_EQ(want.dev, got.dev) << path;
  EXPECT_EQ(want.nlink, got.nlink) << path;
  EXPECT_EQ(want.atime.tv_sec, got.atime.tv_sec) << path;
  EXPECT_EQ(want.atime.tv_nsec, got.atime.tv_nsec) << path;
  EXPECT_EQ(want.mtime.tv_sec, got.mtime.tv_sec) << path;
  EXPECT_EQ(want.mtime.tv_nsec, got.mtime.tv_nsec) << path;
  EXPECT_EQ(want.ctime.tv_sec, got.ctime.tv_sec) << path;
  EXPECT_EQ(want.ctime.tv_nsec, got.ctime.tv_nsec) << path;
}
} // namespace

TEST_F(GetFileInformationTest, matchesLstat) {
  mkdirOrThrow(root_ + "/Dir/Sub");
  writeFile(root_ + "/Dir/Sub/Leaf");
  writeFile(root_ + "/Dir/Exec");
  chmod((root_ + "/Dir/Exec").c_str(), 04755);
  mkdirOrThrow(root_ + "/Sticky");
  chmod((root_ + "/Sticky").c_str(), 01777);
  link((root_ + "/Dir/File.txt").c_str(), (root_ + "/Dir/Hard").c_str());
  mkfifo((root_ + "/Dir/Fifo").c_str(), 0600);
  for (auto name :
       {"/Dir/File.txt",
        "/Dir/Hard",
        "/Dir/FileLink",
        "/Dir/Exec",
        "/Dir/Fifo",
        "/Dir",
        "/Dir/Sub",
        "/Sticky",
        "/DirLink"}) {
    expectSameAsLstat(root_ + name, cs_);
  }
}

namespace {
// Lists path the way the crawler does and compares against lstat.
void expectListingMatchesLstat(
    const std::string& path,
    std::optional<dev_t> countedDev,
    std::set<std::string> want) {
  auto dir = openDir(path.c_str(), true, CaseSensitivity::Unknown, countedDev);
  std::set<std::string> got;
  while (auto ent = dir->readDir()) {
    got.insert(ent->d_name);
    ASSERT_TRUE(ent->has_stat) << ent->d_name;
    struct stat st;
    ASSERT_EQ(0, lstat((path + "/" + ent->d_name).c_str(), &st));
    if (ent->stat.isDir() && countedDev) {
      EXPECT_EQ(st.st_nlink, ent->stat.nlink) << ent->d_name;
    }
  }
  EXPECT_EQ(want, got) << path;
}
} // namespace

TEST_F(GetFileInformationTest, countedListing) {
  mkdirOrThrow(root_ + "/Empty");
  mkdirOrThrow(root_ + "/Dir/Sub");
  for (int i = 0; i < 300; ++i) {
    writeFile(root_ + "/Dir/Sub/f" + std::to_string(i));
  }
  struct stat st;
  ASSERT_EQ(0, lstat(root_.c_str(), &st));
  auto fsType = w_fstype(root_.c_str());
  std::optional<dev_t> counted;
  if (fsType == "apfs" || fsType == "hfs") {
    counted = st.st_dev;
  }
  std::set<std::string> sub;
  for (int i = 0; i < 300; ++i) {
    sub.insert("f" + std::to_string(i));
  }
  for (auto dev : {std::optional<dev_t>{}, counted}) {
    expectListingMatchesLstat(root_, dev, {"Dir", "DirLink", "Empty"});
    expectListingMatchesLstat(
        root_ + "/Dir", dev, {"File.txt", "FileLink", "Sub"});
    expectListingMatchesLstat(root_ + "/Dir/Sub", dev, sub);
    expectListingMatchesLstat(root_ + "/Empty", dev, {});
  }
}

#endif
