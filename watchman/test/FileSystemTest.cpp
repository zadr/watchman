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
#include <system_error>
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

  std::error_code statError(const std::string& path) {
    try {
      getFileInformation(path.c_str(), cs_);
      return {};
    } catch (const std::system_error& e) {
      return e.code();
    }
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
  EXPECT_EQ(std::errc::no_such_file_or_directory, statError(root_ + "/nope"));
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
  EXPECT_EQ(
      std::errc::no_such_file_or_directory,
      statError(root_ + "/Dir/file.txt"));
}

TEST_F(GetFileInformationTest, nonCanonicalParentCaseIsRejected) {
  if (cs_ != CaseSensitivity::CaseInSensitive) {
    GTEST_SKIP() << "filesystem is case-sensitive";
  }
  EXPECT_EQ(
      std::errc::no_such_file_or_directory,
      statError(root_ + "/dir/File.txt"));
}

#endif
