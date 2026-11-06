/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/fs/FileSystem.h"
#include <fmt/core.h>
#include <folly/String.h>
#include <system_error>
#include "watchman/Errors.h"
#include "watchman/fs/FSDetect.h"
#include "watchman/portability/WinError.h"
#include "watchman/watchman_stream.h"
#include "watchman/watchman_string.h"

#ifdef __APPLE__
#include <sys/attr.h> // @manual
#include <sys/utsname.h> // @manual
#include <sys/vnode.h> // @manual
#endif

#if defined(_WIN32) || defined(O_PATH)
#define CAN_OPEN_SYMLINKS 1
#else
#define CAN_OPEN_SYMLINKS 0
#endif

#if !CAN_OPEN_SYMLINKS && defined(__APPLE__) && defined(AT_SYMLINK_NOFOLLOW_ANY)
#define WATCHMAN_STAT_NOFOLLOW_ANY 1
#else
#define WATCHMAN_STAT_NOFOLLOW_ANY 0
#endif

namespace watchman {

namespace {

class RealFileSystem final : public FileSystem {
 public:
  std::unique_ptr<DirHandle> openDir(const char* path, bool strict = true)
      override {
    return watchman::openDir(path, strict);
  }

  FileInformation getFileInformation(
      const char* path,
      CaseSensitivity caseSensitive = CaseSensitivity::Unknown) override {
    return watchman::getFileInformation(path, caseSensitive);
  }

  Result<FileInformation, std::error_code> tryGetFileInformation(
      const char* path,
      CaseSensitivity caseSensitive = CaseSensitivity::Unknown) override {
    return watchman::tryGetFileInformation(path, caseSensitive);
  }

  /**
   * Watchman-specific API for creating an empty file on the filesystem.
   * On unix, the created file will have mode 0700.
   * Throws system_error on failure.
   */
  void touch(const char* path) override {
    if (!w_stm_open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0700)) {
      int err = errno;
      throw std::system_error{
          err,
          std::generic_category(),
          fmt::format("failed to touch {}: {}", path, folly::errnoStr(err))};
    }
  }
};

RealFileSystem gRealFileSystem;

} // namespace

FileSystem& realFileSystem = gRealFileSystem;

Result<FileInformation, std::error_code> FileSystem::tryGetFileInformation(
    const char* path,
    CaseSensitivity caseSensitive) {
  try {
    return getFileInformation(path, caseSensitive);
  } catch (const std::system_error& e) {
    return e.code();
  }
}

#if !CAN_OPEN_SYMLINKS

namespace {

#ifdef __APPLE__
/** Non-throwing core of checkCanonicalBaseName. Returns ENOENT in
 * name_mismatch_category if the name differs from the on-disk name. */
std::error_code canonicalBaseNameError(const char* path) {
  struct attrlist attrlist;
  struct {
    uint32_t len;
    attrreference_t ref;
    char canonical_name[WATCHMAN_NAME_MAX];
  } vomit;
  w_string_piece pathPiece(path);
  auto base = pathPiece.baseName();

  memset(&attrlist, 0, sizeof(attrlist));
  attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
  attrlist.commonattr = ATTR_CMN_NAME;

  if (getattrlist(path, &attrlist, &vomit, sizeof(vomit), FSOPT_NOFOLLOW) ==
      -1) {
    return std::error_code(errno, std::generic_category());
  }

  w_string_piece name(((char*)&vomit.ref) + vomit.ref.attr_dataoffset);
  if (name != base) {
    return std::error_code(ENOENT, name_mismatch_category());
  }
  return {};
}
#endif

/** Checks that the basename component of the input path exactly
 * matches the canonical case of the path on disk.
 * It only makes sense to call this function on a case insensitive filesystem.
 * If the case does not match, throws an exception. */
void checkCanonicalBaseName(const char* path) {
#ifdef __APPLE__
  if (auto ec = canonicalBaseNameError(path)) {
    throw std::system_error(
        ec,
        fmt::format(
            "checkCanonicalBaseName({}): {}",
            path,
            ec.category() == name_mismatch_category()
                ? "doesn't match the canonical name on disk"
                : "getattrlist failed"));
  }
#else
  // Older Linux and BSDish systems are in this category.
  // This is the awful portable fallback used in the absence of
  // a system specific way to detect this.
  w_string_piece pathPiece(path);
  auto parent = pathPiece.dirName().asWString();
  auto dir = realFileSystem.openDir(parent.c_str());
  auto base = pathPiece.baseName();

  while (true) {
    auto ent = dir->readDir();
    if (!ent) {
      // We didn't find an entry that exactly matched -> fail
      throw std::system_error(
          ENOENT,
          name_mismatch_category(),
          fmt::format(
              "checkCanonicalBaseName({}): no match found in parent dir",
              path));
    }
    // Note: we don't break out early if we get a case-insensitive match
    // because the dir may contain multiple representations of the same
    // name.  For example, Bash-for-Windows has dirs that contain both
    // "pod" and "Pod" dirs in its perl installation.  We want to make
    // sure that we've observed all of the entries in the dir before
    // giving up.
    if (w_string_piece(ent->d_name) == base) {
      // Exact match; all is good!
      return;
    }
  }
#endif
}
} // namespace
#endif

FileDescriptor openFileHandle(
    const char* path,
    const OpenFileHandleOptions& opts) {
#ifndef _WIN32
  int flags = (!opts.followSymlinks ? O_NOFOLLOW : 0) |
      (opts.closeOnExec ? O_CLOEXEC : 0) |
#ifdef O_PATH
      (opts.metaDataOnly ? O_PATH : 0) |
#endif
      ((opts.readContents && opts.writeContents)
           ? O_RDWR
           : (opts.writeContents      ? O_WRONLY
                  : opts.readContents ? O_RDONLY
                                      : 0)) |
      (opts.create ? O_CREAT : 0) | (opts.exclusiveCreate ? O_EXCL : 0) |
      (opts.truncate ? O_TRUNC : 0) | (opts.directory ? O_DIRECTORY : 0);

  auto fd = open(path, flags);
  if (fd == -1) {
    int err = errno;
    throw std::system_error(
        err, std::generic_category(), fmt::format("open: {}", path));
  }
  FileDescriptor file(fd, FileDescriptor::FDType::Unknown);
#else // _WIN32
  DWORD access = 0, share = 0, create = 0, attrs = 0;
  DWORD err;
  auto sec = SECURITY_ATTRIBUTES();

  if (!strcmp(path, "/dev/null")) {
    path = "NUL:";
  }

  auto wpath = w_string_piece(path).asWideUNC();

  if (opts.metaDataOnly) {
    access = 0;
  } else {
    if (opts.writeContents) {
      access |= GENERIC_WRITE;
    }
    if (opts.readContents) {
      access |= GENERIC_READ;
    }
  }

  // We want more posix-y behavior by default
  share = FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE;

  sec.nLength = sizeof(sec);
  sec.bInheritHandle = TRUE;
  if (opts.closeOnExec) {
    sec.bInheritHandle = FALSE;
  }

  if (opts.create && opts.exclusiveCreate) {
    create = CREATE_NEW;
  } else if (opts.create && opts.truncate) {
    create = CREATE_ALWAYS;
  } else if (opts.create) {
    create = OPEN_ALWAYS;
  } else if (opts.truncate) {
    create = TRUNCATE_EXISTING;
  } else {
    create = OPEN_EXISTING;
  }

  attrs = FILE_FLAG_POSIX_SEMANTICS | FILE_FLAG_BACKUP_SEMANTICS;
  if (!opts.followSymlinks) {
    attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
  }

  FileDescriptor file(
      intptr_t(CreateFileW(
          wpath.c_str(), access, share, &sec, create, attrs, nullptr)),
      FileDescriptor::FDType::Unknown);
  err = GetLastError();
  if (!file) {
    throw std::system_error(
        err,
        std::system_category(),
        std::string("CreateFileW for openFileHandle: ") + path);
  }

#endif

  if (!opts.strictNameChecks) {
    return file;
  }

  checkOpenedPath(file, path);
#if !CAN_OPEN_SYMLINKS
  CaseSensitivity caseSensitive = opts.caseSensitive;
  if (caseSensitive == CaseSensitivity::Unknown) {
    caseSensitive = getCaseSensitivityForPath(path);
  }
  if (caseSensitive == CaseSensitivity::CaseInSensitive) {
    // We need to perform one extra check for case-insensitive
    // paths to make sure that we didn't accidentally open
    // the wrong case name.
    checkCanonicalBaseName(path);
  }
#endif
  return file;
}

void checkOpenedPath(const FileDescriptor& file, const char* path) {
  auto opened = file.getOpenedPath();
  if (!w_string_piece(opened).pathIsEqual(path)) {
    throw std::system_error(
        ENOENT,
        name_mismatch_category(),
        fmt::format(
            "open({}): opened path doesn't match canonical path {}",
            path,
            opened));
  }
}

#if WATCHMAN_STAT_NOFOLLOW_ANY
namespace {

std::error_code lookupError(int err) {
  // NOFOLLOW_ANY fails with ELOOP on a symlink in any intermediate component.
  // A symlink in the middle means the path we were told about no longer
  // exists as a real directory chain.
  return std::error_code(err == ELOOP ? ENOENT : err, std::generic_category());
}

/** fstatat, plus canonical-case checks on the parent and leaf names when
 * checkNames is set. */
Result<FileInformation, std::error_code> statCheckingNames(
    const char* path,
    bool checkNames) {
  struct stat st;
  if (fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW_ANY)) {
    return lookupError(errno);
  }
  if (checkNames) {
    w_string_piece pathPiece(path);
    if (auto ec =
            canonicalBaseNameError(pathPiece.dirName().asWString().c_str())) {
      return ec;
    }
    if (auto ec = canonicalBaseNameError(path)) {
      return ec;
    }
  }
  return FileInformation(st);
}

// Field order follows the ATTR_* bit order, which is the order getattrlist
// packs them in. FSOPT_PACK_INVAL_ATTRS keeps the common fields at fixed
// offsets; the file fields are only present for non-directories.
struct CanonicalStatAttrs {
  uint32_t len;
  attribute_set_t returned; // ATTR_CMN_RETURNED_ATTRS
  dev_t dev; // ATTR_CMN_DEVID
  fsobj_type_t objtype; // ATTR_CMN_OBJTYPE
  struct timespec mtime; // ATTR_CMN_MODTIME
  struct timespec ctime; // ATTR_CMN_CHGTIME
  struct timespec atime; // ATTR_CMN_ACCTIME
  uid_t uid; // ATTR_CMN_OWNERID
  gid_t gid; // ATTR_CMN_GRPID
  uint32_t mode; // ATTR_CMN_ACCESSMASK: permission bits only
  uint64_t ino; // ATTR_CMN_FILEID
  attrreference_t fullpath; // ATTR_CMN_FULLPATH
  uint32_t nlink; // ATTR_FILE_LINKCOUNT
  off_t size; // ATTR_FILE_DATALENGTH: the data fork, as st_size reports
} __attribute__((packed));

constexpr attrgroup_t kCanonicalStatCommonAttrs = ATTR_CMN_RETURNED_ATTRS |
    ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_CHGTIME |
    ATTR_CMN_ACCTIME | ATTR_CMN_OWNERID | ATTR_CMN_GRPID |
    ATTR_CMN_ACCESSMASK | ATTR_CMN_FILEID | ATTR_CMN_FULLPATH;
constexpr attrgroup_t kCanonicalStatFileAttrs =
    ATTR_FILE_LINKCOUNT | ATTR_FILE_DATALENGTH;

mode_t modeTypeBits(fsobj_type_t objtype) {
  switch (objtype) {
    case VREG:
      return S_IFREG;
    case VDIR:
      return S_IFDIR;
    case VLNK:
      return S_IFLNK;
    case VBLK:
      return S_IFBLK;
    case VCHR:
      return S_IFCHR;
    case VFIFO:
      return S_IFIFO;
    case VSOCK:
      return S_IFSOCK;
    default:
      return 0;
  }
}

/** Stat a path on a case-insensitive volume and require that every component
 * matches the on-disk name. The kernel builds ATTR_CMN_FULLPATH from on-disk
 * names regardless of the case used for the lookup, so comparing it with the
 * requested path checks all components in the same call that returns the
 * stat data. This replaces fstatat plus two ATTR_CMN_NAME lookups. */
Result<FileInformation, std::error_code> statCanonical(const char* path) {
  struct attrlist attrlist{};
  attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
  attrlist.commonattr = kCanonicalStatCommonAttrs;
  attrlist.fileattr = kCanonicalStatFileAttrs;

  union {
    CanonicalStatAttrs attrs;
    char bytes[sizeof(CanonicalStatAttrs) + PATH_MAX + 8];
  } buf;
  if (getattrlist(
          path,
          &attrlist,
          &buf,
          sizeof(buf),
          FSOPT_NOFOLLOW | FSOPT_NOFOLLOW_ANY | FSOPT_PACK_INVAL_ATTRS)) {
    return lookupError(errno);
  }
  const auto& a = buf.attrs;
  const bool isDir = a.objtype == VDIR;

  constexpr attrgroup_t kRequiredCommon =
      kCanonicalStatCommonAttrs & ~ATTR_CMN_RETURNED_ATTRS;
  const char* fullpath = nullptr;
  if ((a.returned.commonattr & kRequiredCommon) == kRequiredCommon) {
    fullpath = reinterpret_cast<const char*>(&a.fullpath) +
        a.fullpath.attr_dataoffset;
    if (fullpath + a.fullpath.attr_length > buf.bytes + sizeof(buf)) {
      fullpath = nullptr;
    }
  }
  if (!fullpath ||
      (!isDir &&
       (a.returned.fileattr & kCanonicalStatFileAttrs) !=
           kCanonicalStatFileAttrs)) {
    // The filesystem didn't provide everything; take the long way.
    return statCheckingNames(path, true);
  }

  if (strcmp(fullpath, path) != 0) {
    if (!isDir && a.nlink > 1) {
      // A hard-linked file may report the path of another link.
      return statCheckingNames(path, true);
    }
    return std::error_code(ENOENT, name_mismatch_category());
  }

  if (isDir) {
    // The directory link-count attribute is not st_nlink (APFS reports 1;
    // stat reports 2 + entries), so take stat data from stat.
    return statCheckingNames(path, false);
  }

  FileInformation info;
  info.mode = (a.mode & ~S_IFMT) | modeTypeBits(a.objtype);
  info.size = static_cast<uint64_t>(a.size);
  info.uid = a.uid;
  info.gid = a.gid;
  info.ino = a.ino;
  info.dev = a.dev;
  info.nlink = a.nlink;
  info.atime = a.atime;
  info.mtime = a.mtime;
  info.ctime = a.ctime;
  return info;
}

} // namespace
#endif

Result<FileInformation, std::error_code> tryGetFileInformation(
    const char* path,
    CaseSensitivity caseSensitive) {
#if WATCHMAN_STAT_NOFOLLOW_ANY
  // One lookup instead of open(parent) + F_GETPATH + fstatat + close.
  if (caseSensitive == CaseSensitivity::Unknown) {
    caseSensitive = getCaseSensitivityForPath(path);
  }
  if (caseSensitive == CaseSensitivity::CaseInSensitive) {
    return statCanonical(path);
  }
  return statCheckingNames(path, false);
#else
  try {
    return getFileInformation(path, caseSensitive);
  } catch (const std::system_error& e) {
    return e.code();
  }
#endif
}

FileInformation getFileInformation(
    const char* path,
    CaseSensitivity caseSensitive) {
  auto options = OpenFileHandleOptions::queryFileInfo();
  options.caseSensitive = caseSensitive;
#if defined(_WIN32) || defined(O_PATH)
  // These operating systems allow opening symlink nodes and querying them
  // for stat information
  auto handle = openFileHandle(path, options);
  auto info = handle.getInfo();
  return info;
#elif WATCHMAN_STAT_NOFOLLOW_ANY
  auto result = tryGetFileInformation(path, caseSensitive);
  if (result.hasError()) {
    throw std::system_error(
        result.error(), fmt::format("getFileInformation({})", path));
  }
  return std::move(result).value();
#else
  // Since the leaf of the path may be a symlink, and this system doesn't
  // allow opening symlinks for stat purposes, we have to resort to performing
  // a relative fstatat() from the parent dir.
  w_string_piece pathPiece(path);
  auto parent = pathPiece.dirName().asWString();
  auto handle = openFileHandle(parent.c_str(), options);
  struct stat st;
  if (fstatat(
          handle.fd(), pathPiece.baseName().data(), &st, AT_SYMLINK_NOFOLLOW)) {
    throw std::system_error(errno, std::generic_category(), "fstatat");
  }

  if (caseSensitive == CaseSensitivity::Unknown) {
    caseSensitive = getCaseSensitivityForPath(path);
  }
  if (caseSensitive == CaseSensitivity::CaseInSensitive) {
    // We need to perform one extra check for case-insensitive
    // paths to make sure that we didn't accidentally open
    // the wrong case name.
    checkCanonicalBaseName(path);
  }

  return FileInformation(st);
#endif
}

#ifdef _WIN32
namespace {

w_string getCurrentDirectory() {
  WCHAR wchar[WATCHMAN_NAME_MAX];
  auto len = GetCurrentDirectoryW(std::size(wchar), wchar);
  auto err = GetLastError();
  // Technically, len > std::size(wchar) is sufficient, because the w_string
  // constructor below will add a trailing zero.
  if (len == 0 || len >= std::size(wchar)) {
    throw std::system_error(
        err, std::system_category(), "GetCurrentDirectoryW");
  }
  // Assumption: that the OS maintains the CWD in canonical form
  return w_string(wchar, len);
}

} // namespace
#endif

w_string realPath(const char* path) {
  auto options = OpenFileHandleOptions::queryFileInfo();
  // Follow symlinks, because that's really the point of this function
  options.followSymlinks = 1;
  options.strictNameChecks = 0;

#ifdef _WIN32
  // Special cases for cwd.
  // On Windows, "" is used to refer to the CWD.
  // We also allow using "." for parity with unix, even though that
  // doesn't generally work for that purpose on windows.
  // This allows `watchman watch-project .` to succeeed on windows.
  if (path[0] == 0 || (path[0] == '.' && path[1] == 0)) {
    return getCurrentDirectory();
  }
#endif

  auto handle = openFileHandle(path, options);
  return handle.getOpenedPath();
}

w_string readSymbolicLink(const char* path) {
#ifndef _WIN32
  std::string result;

  // Speculatively assume that this is large enough to read the
  // symlink text.  This helps to avoid an extra lstat call.
  result.resize(256);

  for (int retry = 0; retry < 2; ++retry) {
    auto len = readlink(path, &result[0], result.size());
    if (len < 0) {
      throw std::system_error(
          errno,
          std::system_category(),
          fmt::format("readlink for readSymbolicLink(\"{}\")", path));
    }
    if (size_t(len) < result.size()) {
      return w_string(result.data(), len);
    }

    // Truncated read; we need to figure out the right size to use
    struct stat st;
    if (lstat(path, &st)) {
      throw std::system_error(
          errno,
          std::system_category(),
          fmt::format("lstat for readSymbolicLink(\"{}\")", path));
    }

    result.resize(st.st_size + 1, 0);
  }

  throw std::system_error(
      E2BIG,
      std::system_category(),
      fmt::format(
          "readlink for readSymbolicLink(\"{}\"): symlink changed while reading it",
          path));
#else
  return openFileHandle(path, OpenFileHandleOptions::queryFileInfo())
      .readSymbolicLink();
#endif
}

bool isOnSameMount(
    const FileInformation& root,
    const FileInformation& file,
    const char* file_path) {
#ifdef _WIN32
  (void)root;
  if ((file.fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ==
      FILE_ATTRIBUTE_REPARSE_POINT) {
    auto fd = openFileHandle(file_path, OpenFileHandleOptions::queryFileInfo());
    auto reparseTag = fd.getReparseTag();
    return reparseTag != IO_REPARSE_TAG_PROJFS;
  } else {
    return true;
  }
#else
  (void)file_path;
  return root.dev == file.dev;
#endif
}

} // namespace watchman

#ifdef _WIN32

int mkdir(const char* path, int) {
  auto wpath = w_string_piece(path).asWideUNC();
  DWORD err;
  BOOL res;

  res = CreateDirectoryW(wpath.c_str(), nullptr);
  err = GetLastError();

  if (res) {
    return 0;
  }
  errno = map_win32_err(err);
  return -1;
}

#endif
