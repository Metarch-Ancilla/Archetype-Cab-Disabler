#include "core.hpp"
#include <cerrno>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include "../../frontends/shared/windows_files.h"
#include <shlobj.h>
#ifdef CAB_WINDOWS_STATIC_AUTH
#include "../../frontends/windows/authorization.h"
#endif
#else
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#else
#include <openssl/sha.h>
#include <sys/random.h>
#endif
#endif
namespace cab {
std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \r\n\t");
    if (a == s.npos) return "";
    return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
std::string json(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c < 32) {
            char b[7];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else
            out += c;
    }
    return out + '"';
}
std::string encode16(const std::string &s, bool big) {
    std::string out;
    auto emit = [&](uint16_t u) {
        out += char(big ? u >> 8 : u & 255);
        out += char(big ? u & 255 : u >> 8);
    };
    out.reserve(s.size() * 2);
    bool valid = nd::visitUtf8(s, [&](uint32_t c) {
        if (c < 0x10000)
            emit((uint16_t)c);
        else {
            c -= 0x10000;
            emit(0xd800 | (c >> 10));
            emit(0xdc00 | (c & 1023));
        }
    });
    require(valid, "Invalid UTF-8");
    return out;
}
#ifdef _WIN32
static std::wstring wide(const std::string &s) {
    require(utf8(s), "Invalid path UTF-8");
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    require(n > 0 || w.empty(), "Invalid UTF-16 path");
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring windowsPath(const Path &p) {
    auto w = wide(p);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}
static std::wstring native(const Path &p) {
    auto w = windowsPath(p);
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}
static HANDLE handle(Handle h) { return (HANDLE)h; }
#endif
static IOError failure(const std::string &s) {
#ifdef _WIN32
    auto code = GetLastError();
    return IOError(s + " (Windows error " + std::to_string(code) + ")", (int)code);
#else
    int code = errno;
    return IOError(s + ": " + std::strerror(code), code);
#endif
}
Path join(const Path &a, const Path &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + (a.back() == '/' ? "" : "/") + b;
}
Path filename(const Path &p) {
    auto i = p.find_last_of('/');
    return i == p.npos ? p : p.substr(i + 1);
}
Path parent(const Path &p) {
    auto i = p.find_last_of('/');
    if (i == p.npos) return p;
    if (i == 0) return "/";
    if (i == 2 && p[1] == ':') return p.substr(0, 3);
    return p.substr(0, i);
}
Path extension(const Path &p) {
    auto f = filename(p);
    auto i = f.find_last_of('.');
    return i == f.npos || i == 0 || i + 1 == f.size() ? "" : f.substr(i);
}
Path stem(const Path &p) {
    auto f = filename(p), e = extension(p);
    return f.substr(0, f.size() - e.size());
}
std::vector<std::string> parts(const Path &p) {
    std::vector<std::string> out;
    size_t b = 0;
    while (b < p.size()) {
        auto e = p.find('/', b);
        if (e == p.npos) e = p.size();
        if (e > b) out.push_back(p.substr(b, e - b));
        b = e + 1;
    }
    return out;
}
Path absolute(const Path &input) {
    require(!input.empty() && input.find('\0') == input.npos, "Invalid empty/NUL path");
    Path p = input;
#ifdef _WIN32
    std::replace(p.begin(), p.end(), '\\', '/');
    auto w = wide(p);
    DWORD size = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    require(size > 0, "Cannot resolve path");
    std::wstring resolved(size, 0);
    DWORD n = GetFullPathNameW(w.c_str(), size, resolved.data(), nullptr);
    require(n && n < size, "Cannot resolve path");
    resolved.resize(n);
    p = narrow(resolved);
    std::replace(p.begin(), p.end(), '\\', '/');
#else
    if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) p = home() + p.substr(1);
    if (p[0] != '/') {
        char cwd[8192];
        if (!getcwd(cwd, sizeof(cwd))) throw failure("Cannot get directory");
        p = join(cwd, p);
    }
#endif
    Path prefix = p.rfind("//", 0) == 0 ? "//" : p[0] == '/' ? "/" : "";
    std::vector<std::string> clean;
    for (auto &part : parts(p)) {
        if (part == ".") continue;
        if (part == "..") {
            require(!clean.empty(), "Path escapes root");
            clean.pop_back();
        } else
            clean.push_back(part);
    }
    Path out = prefix;
    for (auto &part : clean) out = join(out, part);
    if (out.size() == 2 && out[1] == ':') out += '/';
    return out;
}
Path relative(const Path &p, const Path &root) {
    auto prefix = root + (root.back() == '/' ? "" : "/");
#ifdef _WIN32
    require(lower(p).rfind(lower(prefix), 0) == 0, "Preset outside its library");
#else
    require(p.rfind(prefix, 0) == 0, "Preset outside its library");
#endif
    return p.substr(prefix.size());
}
Path home() {
#ifdef _WIN32
    wchar_t *p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) {
        auto result = narrow(p);
        CoTaskMemFree(p);
        std::replace(result.begin(), result.end(), '\\', '/');
        return result;
    }
    throw Error("Cannot locate user profile");
#else
    const char *p = getenv("HOME");
    require(p && *p, "Cannot locate home directory");
    return p;
#endif
}
Path desktop() {
#ifdef _WIN32
    wchar_t *p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) {
        auto result = absolute(narrow(p));
        CoTaskMemFree(p);
        return result;
    }
#endif
    return join(home(), "Desktop");
}
Path executable() {
#ifdef _WIN32
    std::wstring w(32768, 0);
    DWORD n = GetModuleFileNameW(nullptr, w.data(), (DWORD)w.size());
    require(n && n < w.size(), "Cannot locate backend");
    w.resize(n);
    return absolute(narrow(w));
#elif defined(__APPLE__)
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::vector<char> b(n);
    require(!_NSGetExecutablePath(b.data(), &n), "Cannot locate backend");
    return absolute(b.data());
#else
    char b[8192];
    ssize_t n = readlink("/proc/self/exe", b, sizeof(b));
    require(n > 0 && n < (ssize_t)sizeof(b), "Cannot locate backend");
    return std::string(b, (size_t)n);
#endif
}
bool exists(const Path &p) {
#ifdef _WIN32
    return GetFileAttributesW(native(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat s;
    return lstat(p.c_str(), &s) == 0;
#endif
}
bool writable(const Path &p) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(native(p).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           ((attrs & FILE_ATTRIBUTE_DIRECTORY) || !(attrs & FILE_ATTRIBUTE_READONLY));
#else
    return access(p.c_str(), W_OK) == 0;
#endif
}
bool permissionError(int code) {
#ifdef _WIN32
    return code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD;
#else
    return code == EACCES || code == EPERM;
#endif
}
bool directory(const Path &p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(native(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat s;
    return lstat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
#endif
}
// Anchors are operation-scoped; a later operation resolves the path afresh.
struct DirectoryAnchor {
    File file;
    explicit DirectoryAnchor(const Path &raw, bool create = false) {
        auto path = absolute(raw);
#ifdef _WIN32
        auto h = cab_open_directory(windowsPath(path).c_str(), create);
        if (h == INVALID_HANDLE_VALUE) throw failure("Cannot open anchored directory");
        file = File((Handle)h);
#else
        file = File(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (file.handle == Invalid) throw failure("Cannot open root directory");
        for (const auto &name : parts(path)) {
            int next = openat((int)file.handle, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0 && create && errno == ENOENT) {
                if (mkdirat((int)file.handle, name.c_str(), 0777) && errno != EEXIST)
                    throw failure("Cannot create anchored directory");
                next = openat((int)file.handle, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            }
            if (next < 0) throw failure("Cannot open anchored directory");
            file = File(next);
        }
#endif
    }
    explicit DirectoryAnchor(File opened) : file(std::move(opened)) {}
    File open(const Path &name, bool writing, bool create, bool exclusive = false) const {
        require(!name.empty() && name != "." && name != ".." && name.find('/') == name.npos && name.find('\0') == name.npos,
                "Invalid anchored filename");
#ifdef _WIN32
        auto h = cab_open_child(handle(file.handle), wide(name).c_str(),
            GENERIC_READ | (writing ? GENERIC_WRITE : 0) | (exclusive ? DELETE : 0),
            create ? FILE_CREATE : FILE_OPEN, FILE_NON_DIRECTORY_FILE,
            exclusive ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (h == INVALID_HANDLE_VALUE) throw failure("Cannot open anchored file");
        File out((Handle)h);
#else
        (void)exclusive;
        int h = openat((int)file.handle, name.c_str(), (writing ? O_RDWR : O_RDONLY) |
            O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0), 0666);
        if (h < 0) throw failure("Cannot open anchored file");
        File out(h);
#endif
        signature(out.handle);
        return out;
    }
};
// Normalization is immutable; optional name checks never authorize an open.
struct PreparedPath {
    Path value;
    explicit PreparedPath(const Path &raw) : value(absolute(raw)) {}
    void check() const {
        Path current=value;
#ifdef _WIN32
        bool unc=value.rfind("//",0)==0;
        size_t depth=parts(value).size();
#endif
        for(;;) {
#ifdef _WIN32
            if(!unc||depth>=2) {
                DWORD attributes=GetFileAttributesW(native(current).c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES) {
                    DWORD error=GetLastError();
                    if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)throw failure("Cannot validate preset path");
                }else require(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Symbolic-link/junction paths are not writable");
            }
            --depth;
#else
            struct stat info;
            if(!lstat(current.c_str(),&info))require(!S_ISLNK(info.st_mode),"Symbolic-link paths are not writable");
            else require(errno==ENOENT,"Cannot validate preset path");
#endif
            auto up=parent(current);if(up==current)break;current=std::move(up);
        }
    }
};
void safePath(const Path &raw) { PreparedPath(raw).check(); }
void mkdirs(const Path &p) { DirectoryAnchor directory(p, true); }
void removeFile(const Path &p) {
#ifdef _WIN32
    if (!DeleteFileW(native(p).c_str())) throw failure("Cannot remove temporary file");
#else
    if (unlink(p.c_str())) throw failure("Cannot remove temporary file");
#endif
}
std::vector<Entry> entries(const Path &p) {
    safePath(p); // Validate again when traversing, not only when a directory was scheduled.
    std::vector<Entry> out;
    size_t pathBytes=0;
#ifdef _WIN32
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(native(join(p, "*")).c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) throw failure("Cannot scan folder");
    std::unique_ptr<void, decltype(&FindClose)> enumeration(h, FindClose);
    do {
        auto name = narrow(d.cFileName);
        if (name == "." || name == ".." || !utf8(name)) continue;
        auto path=join(p,name);
        require(path.size()<=MaxPlanBytes-pathBytes,"Folder scan memory limit exceeded");pathBytes+=path.size();
        out.push_back({std::move(path), (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
                       (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
        if (out.size() >= 50001) {
            return out;
        }
    } while (FindNextFileW(h, &d));
    DWORD error = GetLastError();
    require(error == ERROR_NO_MORE_FILES, "Folder scan failed");
#else
    DirectoryAnchor anchored(p);
    DIR *d = fdopendir((int)anchored.file.handle);
    if(d)anchored.file.release();
    if (!d) throw failure("Cannot scan folder");
    auto closeDirectory = [](DIR *directory) { closedir(directory); };
    std::unique_ptr<DIR, decltype(closeDirectory)> enumeration(d, closeDirectory);
    for (;;) {
        errno = 0;
        auto e = readdir(d);
        if (!e) {
            require(!errno, "Folder scan failed");
            break;
        }
        std::string name = e->d_name;
        if (name == "." || name == ".." || !utf8(name)) continue;
        if (out.size() >= 50001) break;
        auto path = join(p, name);
        struct stat st;
        if (fstatat(dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) continue;
        require(path.size()<=MaxPlanBytes-pathBytes,"Folder scan memory limit exceeded");pathBytes+=path.size();
        out.push_back({std::move(path), S_ISDIR(st.st_mode), S_ISREG(st.st_mode), S_ISLNK(st.st_mode),
                       (uint64_t)st.st_dev, (uint64_t)st.st_ino});
        errno = 0;
    }

#endif
    return out;
}
File &File::operator=(File &&other) noexcept {
    if (this != &other) {
        File previous(release());
        handle = other.release();
    }
    return *this;
}
File::~File() {
    if (handle == Invalid) return;
#ifdef _WIN32
    CloseHandle(cab::handle(handle));
#else
    close((int)handle);
#endif
}
static File openFile(const PreparedPath &p, bool writable, bool create, Signature *stamp=nullptr) {
    DirectoryAnchor directory(parent(p.value));
    auto f = directory.open(filename(p.value), writable, create);
    auto observed=signature(f.handle);
    if(stamp)*stamp=observed;
    return f;
}
File openRead(const Path &p) { return openFile(PreparedPath(p), false, false); }
File openWrite(const Path &p) { return openFile(PreparedPath(p), true, false); }
File createExclusive(const Path &p) { return openFile(PreparedPath(p), true, true); }
Signature signature(Handle h) {
    Signature out;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION i;
    require(GetFileType(handle(h)) == FILE_TYPE_DISK && GetFileInformationByHandle(handle(h), &i),
            "Not a regular disk file");
    require(!(i.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
            "Not a regular file");
    out.values = {
        i.dwVolumeSerialNumber,
        ((uint64_t)i.nFileIndexHigh << 32) | i.nFileIndexLow,
        ((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow,
        ((uint64_t)i.ftLastWriteTime.dwHighDateTime << 32) | i.ftLastWriteTime.dwLowDateTime,
        ((uint64_t)i.ftCreationTime.dwHighDateTime << 32) | i.ftCreationTime.dwLowDateTime,
        i.dwFileAttributes,
        0,
        0};
#else
    struct stat st;
    if (fstat((int)h, &st)) throw failure("Cannot inspect file");
    require(S_ISREG(st.st_mode), "Not a regular file");
#ifdef __APPLE__
    auto m = st.st_mtimespec, c = st.st_ctimespec;
#else
    auto m = st.st_mtim, c = st.st_ctim;
#endif
    out.values = {(uint64_t)st.st_dev,
                  (uint64_t)st.st_ino,
                  (uint64_t)st.st_size,
                  (uint64_t)m.tv_sec * 1000000000 + m.tv_nsec,
                  (uint64_t)c.tv_sec * 1000000000 + c.tv_nsec,
                  (uint64_t)st.st_mode,
                  st.st_uid,
                  st.st_gid};
#endif
    return out;
}
static Signature pathSignature(const PreparedPath &p) {
    DirectoryAnchor directory(parent(p.value));
#ifdef _WIN32
    auto h = cab_open_child(handle(directory.file.handle), wide(filename(p.value)).c_str(),
        FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (h == INVALID_HANDLE_VALUE) throw failure("Cannot inspect file identity");
    File file((Handle)h);
#else
    auto file = directory.open(filename(p.value), false, false);
#endif
    return signature(file.handle);
}
Signature signature(const Path &p) { return pathSignature(PreparedPath(p)); }
std::string identity(const Path &p) { return identity(p, signature(p)); }
std::string identity(const Entry &entry) {
    if (!entry.fileIndex) return identity(entry.path);
    // Enumeration metadata deduplicates scan results; safety is still checked afresh.
    safePath(entry.path);
    Signature observed;
    observed.values[0] = entry.volumeId;
    observed.values[1] = entry.fileIndex;
    return identity(entry.path, observed);
}
std::string identity(const Path &p, const Signature &s) {
    if (s.values[1]) return std::to_string(s.values[0]) + ":" + std::to_string(s.values[1]);
#ifdef _WIN32
    return lower(absolute(p));
#else
    return absolute(p);
#endif
}
uint64_t position(Handle h) {
#ifdef _WIN32
    LARGE_INTEGER zero = {}, out;
    require(SetFilePointerEx(handle(h), zero, &out, FILE_CURRENT), "Cannot tell file position");
    return out.QuadPart;
#else
    auto p = lseek((int)h, 0, SEEK_CUR);
    require(p >= 0, "Cannot tell file position");
    return (uint64_t)p;
#endif
}
void seek(Handle h, uint64_t p) {
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)p;
    require(SetFilePointerEx(handle(h), at, nullptr, FILE_BEGIN), "Cannot seek file");
#else
    require(lseek((int)h, (off_t)p, SEEK_SET) >= 0, "Cannot seek file");
#endif
}
Bytes readHandle(Handle h, size_t limit) {
    Bytes out;
    char block[65536];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        require(ReadFile(handle(h), block, sizeof(block), &n, nullptr), "File read failed");
#else
        ssize_t n = read((int)h, block, sizeof(block));
        if (n < 0 && errno == EINTR) continue;
        require(n >= 0, "File read failed");
#endif
        if (!n) break;
        require(out.size() + (size_t)n <= limit, "Preset exceeds 4 MiB safety limit");
        out.append(block, (size_t)n);
    }
    return out;
}
Bytes readCount(Handle h, size_t count) {
    Bytes out(count, 0);
    size_t offset = 0;
    while (offset < count) {
#ifdef _WIN32
        DWORD n = 0;
        require(ReadFile(handle(h), out.data() + offset,
                         (DWORD)std::min<size_t>(65536, count - offset), &n, nullptr) &&
                    n,
                "Truncated file");
#else
        ssize_t n = read((int)h, out.data() + offset, std::min<size_t>(65536, count - offset));
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "Truncated file");
#endif
        offset += (size_t)n;
    }
    return out;
}
void writeAll(Handle h, const Bytes &data) {
    size_t offset = 0;
    while (offset < data.size()) {
#ifdef _WIN32
        DWORD n = 0;
        require(WriteFile(handle(h), data.data() + offset,
                          (DWORD)std::min<size_t>(65536, data.size() - offset), &n, nullptr) &&
                    n,
                "File write failed");
#else
        ssize_t n =
            write((int)h, data.data() + offset, std::min<size_t>(65536, data.size() - offset));
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "File write failed");
#endif
        offset += (size_t)n;
    }
}
void sync(Handle h) {
#ifdef _WIN32
    require(FlushFileBuffers(handle(h)), "File flush failed");
#else
    require(!fsync((int)h), "File flush failed");
#endif
}
void truncate(Handle h, uint64_t size) {
#ifdef _WIN32
    seek(h, size);
    require(SetEndOfFile(handle(h)), "Cannot truncate file");
#else
    require(!ftruncate((int)h, (off_t)size), "Cannot truncate file");
#endif
}
// Detect changes to both the opened file and its path while its contents are read.
Snapshot readSnapshot(const Path &raw) {
    PreparedPath p(raw);
    Signature before;
    auto f = openFile(p,false,false,&before);
    require(before.values[2] <= MaxBytes, "Preset exceeds 4 MiB safety limit");
    auto data = readHandle(f.handle);
    require(signature(f.handle) == before && pathSignature(p) == before,
            "Source changed while reading; review again");
    return {std::move(data), before};
}
std::string sha256(const Bytes &data) {
    unsigned char digest[32];
#ifdef _WIN32
    struct Provider {
        BCRYPT_ALG_HANDLE value = nullptr;
        Provider() {
            require(!BCryptOpenAlgorithmProvider(&value, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
                    "Cannot open SHA-256 provider");
        }
        ~Provider() { BCryptCloseAlgorithmProvider(value, 0); }
    };
    static Provider provider;
    require(BCryptHash(provider.value, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), digest,
                       sizeof(digest)) == 0,
            "SHA-256 failed");
#elif defined(__APPLE__)
    CC_SHA256(data.data(), (CC_LONG)data.size(), digest);
#else
    SHA256((const unsigned char *)data.data(), data.size(), digest);
#endif
    return hex(std::string((char *)digest, sizeof(digest)));
}
uint64_t entropy() {
    uint64_t value = 0;
#ifdef _WIN32
    require(
        !BCryptGenRandom(nullptr, (PUCHAR)&value, sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
        "Secure entropy failed");
#elif defined(__APPLE__)
    arc4random_buf(&value, sizeof(value));
#else
    require(getrandom(&value, sizeof(value), 0) == sizeof(value), "Secure entropy failed");
#endif
    return value;
}
void writeOriginal(const Path &raw, const Bytes &output, const Signature &expected,
                   const std::string &hash, Authorization *authorization) {
    PreparedPath p(raw);
    File file;
    Signature opened;
    try {
        file = openFile(p,true,false,&opened);
    } catch (const IOError &e) {
        if (!authorization || !permissionError(e.code)) throw;
        file = File(authorization->open(p.value));
        opened=signature(file.handle);
    }
    // After the first write, failures must warn that the original may be partially modified.
    bool started = false;
    try {
        require(opened == expected,
                "Source changed before direct write; review again");
        auto before = readHandle(file.handle);
        require(sha256(before) == hash && signature(file.handle) == expected &&
                    pathSignature(p) == expected,
                "Source changed before direct write; review again");
        seek(file.handle, 0);
        started = true;
        writeAll(file.handle, output);
        truncate(file.handle, output.size());
        sync(file.handle);
        seek(file.handle, 0);
        require(readHandle(file.handle) == output, "Direct write read-back verification failed");
        require(pathSignature(p) == signature(file.handle), "Source changed during direct write");
    } catch (const Error &e) {
        if (started)
            throw Error(
                std::string(
                    "Direct write failed; file may be partially modified. Restore from backup: ") +
                e.what());
        throw;
    }
}
struct StagedFile::State {
    DirectoryAnchor parent;
    Path target, temporary, parentPath;
    std::unique_ptr<DirectoryAnchor> staging;
    bool published = false;
    explicit State(const Path &path) : parent(cab::parent(path), true), target(filename(path)), parentPath(cab::parent(path)) {}
};
StagedFile::StagedFile(const Path &path) : state(std::make_unique<State>(absolute(path))) {
    auto &s = *state;
    s.temporary = ".cab-" + std::to_string(entropy());
#ifdef _WIN32
    file = s.parent.open(s.temporary, true, true, true);
#else
    require(!mkdirat((int)s.parent.file.handle, s.temporary.c_str(), 0700), "Cannot create private staging directory");
    int directory = openat((int)s.parent.file.handle, s.temporary.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) throw failure("Cannot open private staging directory");
    s.staging = std::make_unique<DirectoryAnchor>(File(directory));
    file = s.staging->open("payload", true, true);
#endif
}
File StagedFile::reopen() const {
#ifdef _WIN32
    auto opened = state->parent.open(state->temporary, false, false);
#else
    auto opened = state->staging->open("payload", false, false);
#endif
    auto before = signature(file.handle), now = signature(opened.handle);
    require(before == now, "Staged output replaced before verification");
    return opened;
}
void StagedFile::publish() {
    auto &s = *state;
    DirectoryAnchor currentParent(s.parentPath);
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    require(GetFileInformationByHandle(handle(s.parent.file.handle), &a) &&
            GetFileInformationByHandle(handle(currentParent.file.handle), &b) &&
            a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
            a.nFileIndexLow == b.nFileIndexLow, "Destination directory changed before publication");
#else
    struct stat a{}, b{};
    require(!fstat((int)s.parent.file.handle, &a) && !fstat((int)currentParent.file.handle, &b) &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino, "Destination directory changed before publication");
#endif
    // A second handle identifies the same staged object; Windows denies delete/write sharing.
    auto checked = reopen();
#ifdef _WIN32
    checked = File();
    if(!cab_publish_handle(handle(file.handle),handle(s.parent.file.handle),wide(s.target).c_str()))
        throw failure("Cannot publish output without overwrite");
#else
    auto current = s.staging->open("payload", false, false);
    require(signature(current.handle) == signature(file.handle), "Staged output replaced before publication");
    require(!linkat((int)s.staging->file.handle, "payload", (int)s.parent.file.handle, s.target.c_str(), 0),
            "Cannot publish output without overwrite");
#endif
    s.published = true;
}
StagedFile::~StagedFile() {
    if (!state) return;
    auto &s = *state;
#ifdef _WIN32
    if (!s.published && file.handle != Invalid) {
        FILE_DISPOSITION_INFO info = {TRUE};
        SetFileInformationByHandle(handle(file.handle), FileDispositionInfo, &info, sizeof(info));
    }
#else
    if (!s.staging) return;
    // Cleanup is confined to the private staging directory, never the requested output path.
    struct stat owned{}, current{};
    if (file.handle != Invalid && !fstat((int)file.handle, &owned) &&
        !fstatat((int)s.staging->file.handle, "payload", &current, AT_SYMLINK_NOFOLLOW) &&
        owned.st_dev == current.st_dev && owned.st_ino == current.st_ino)
        unlinkat((int)s.staging->file.handle, "payload", 0);
    struct stat parentInfo{};
    // Shared writable parents cannot safely authorize name-based directory cleanup.
    if (!fstat((int)s.parent.file.handle, &parentInfo) && !(parentInfo.st_mode & (S_IWGRP | S_IWOTH)) &&
        !fstat((int)s.staging->file.handle, &owned) &&
        !fstatat((int)s.parent.file.handle, s.temporary.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
        owned.st_dev == current.st_dev && owned.st_ino == current.st_ino)
        unlinkat((int)s.parent.file.handle, s.temporary.c_str(), AT_REMOVEDIR);
#endif
}
void publishCopy(const Path &target, const Bytes &data, const std::function<void()> &recheck) {
    require(!exists(target), "Destination appeared since preview; no overwrite");
    StagedFile output(target);
    writeAll(output.file.handle, data);
    sync(output.file.handle);
    auto verified = output.reopen();
    require(readHandle(verified.handle) == data, "Temporary output verification failed");
    verified = File();
    recheck();
    output.publish();
}
Path defaultBackup() {
    std::time_t now = std::time(nullptr);
    std::tm date = {};
#ifdef _WIN32
    localtime_s(&date, &now);
#else
    localtime_r(&now, &date);
#endif
    char b[64];
    std::strftime(b, sizeof(b), "neural_dsp_presets_%Y_%m_%d", &date);
    auto p = join(desktop(), std::string(b) + ".zip");
    for (int i = 2; exists(p); ++i)
        p = join(desktop(), std::string(b) + "_" + std::to_string(i) + ".zip");
    return p;
}
// Win32/POSIX define the symbol ABI; copy the pointer representation without
// incompatible function-type casts or aliasing through a differently typed pointer.
template <class Function> static Function symbol(void *library, const char *name) {
#ifdef _WIN32
    auto address = GetProcAddress((HMODULE)library, name);
#else
    auto address = dlsym(library, name);
#endif
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(address), "Unsupported dynamic symbol ABI");
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
Authorization::~Authorization() {
#ifdef CAB_WINDOWS_STATIC_AUTH
    if (batch) cab_windows_authorization_close(batch);
#else
    if (batch) {
#ifdef _WIN32
        auto close = symbol<void (*)(void *)>(library, "cab_windows_authorization_close");
#else
        auto close = symbol<void (*)(void *)>(library, "cab_authorization_close");
#endif
        if (close) close(batch);
    }
#ifdef _WIN32
    if (library) FreeLibrary((HMODULE)library);
#else
    if (library) dlclose(library);
#endif
#endif
}
[[maybe_unused]] static Path bridgeDirectory() {
#ifdef CAB_WINDOWS_SINGLE_EXE
    return parent(executable());
#elif defined(_WIN32)
    return parent(parent(executable()));
#else
    return parent(executable());
#endif
}
bool Authorization::available() const {
#if defined(CAB_WINDOWS_STATIC_AUTH) && defined(CAB_WINDOWS_SINGLE_EXE)
    return exists(executable());
#elif defined(CAB_WINDOWS_STATIC_AUTH)
    return exists(join(bridgeDirectory(), "Archetype Cab Disabler.exe"));
#elif defined(_WIN32)
    return exists(join(bridgeDirectory(), "CabAuthorization.dll")) &&
           exists(join(bridgeDirectory(), "Archetype Cab Disabler.exe"));
#elif defined(__APPLE__)
    return exists(join(bridgeDirectory(), "CabAuthorization.dylib")) &&
           exists(join(bridgeDirectory(), "CabPrivilege"));
#else
    return false;
#endif
}
void Authorization::authorize(const std::vector<Path> &paths, std::atomic<bool> *cancel) {
    (void)cancel;
    require(!batch && !library, "Authorisation batch already initialized");
    require(!paths.empty() && paths.size() <= 50000, "Invalid authorisation batch size");
    require(available(), "Native authorisation is not available");
#ifdef _WIN32
#ifdef CAB_WINDOWS_STATIC_AUTH
    auto fn = &cab_windows_authorize;
#else
    library =
        LoadLibraryExW(native(join(bridgeDirectory(), "CabAuthorization.dll")).c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(library, "Cannot load authorisation bridge");
    auto fn = symbol<void *(*)(const wchar_t *, const wchar_t *const *, DWORD, DWORD *)>(
        library, "cab_windows_authorize");
    require(fn, "Missing authorisation entry point");
#endif
    std::vector<std::wstring> storage;
    for (auto &p : paths) storage.push_back(windowsPath(p));
    std::vector<const wchar_t *> raw;
    for (auto &p : storage) raw.push_back(p.c_str());
    DWORD status = 0;
#ifdef CAB_WINDOWS_SINGLE_EXE
    auto helper = windowsPath(executable());
#else
    auto helper = windowsPath(join(bridgeDirectory(), "Archetype Cab Disabler.exe"));
#endif
    batch = fn(helper.c_str(), raw.data(), (DWORD)raw.size(), &status);
    if (!batch && status == ERROR_CANCELLED)
        throw Cancelled("Administrator authorisation cancelled; no presets edited");
    require(batch, "Administrator authorisation failed (Windows error " + std::to_string(status) +
                       "); no presets edited");
#elif defined(__APPLE__)
    library =
        dlopen(join(bridgeDirectory(), "CabAuthorization.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
    require(library, "Cannot load authorisation bridge");
    auto fn = symbol<void *(*)(const char *, const char *const *, uint32_t, int *, int (*)(void *), void *)>(
        library, "cab_authorize_v2");
    require(fn, "Missing authorisation entry point");
    std::vector<const char *> raw;
    for (auto &p : paths) raw.push_back(p.c_str());
    int status = 0;
    auto stopped = [](void *state) -> int {
        return state && static_cast<std::atomic<bool> *>(state)->load();
    };
    batch = fn(join(bridgeDirectory(), "CabPrivilege").c_str(), raw.data(), (uint32_t)raw.size(),
               &status, stopped, cancel);
    if (!batch && status == ECANCELED)
        throw Cancelled("Administrator authorisation cancelled; no presets edited");
    require(batch, "Administrator authorisation failed; no presets edited");
#else
    (void)paths;
    throw Error("Native authorisation is not available");
#endif
}
Handle Authorization::open(const Path &p) {
    require(batch, "Authorisation is not active");
#ifdef _WIN32
#ifdef CAB_WINDOWS_STATIC_AUTH
    auto fn = &cab_windows_authorized_open;
#else
    auto fn =
        symbol<uintptr_t (*)(void *, const wchar_t *)>(library, "cab_windows_authorized_open");
    require(fn, "Missing authorised open");
#endif
    auto h = fn(batch, windowsPath(p).c_str());
    require(h, "Administrator could not open selected preset");
    return (Handle)h;
#else
    auto fn = symbol<int (*)(void *, const char *)>(library, "cab_authorized_open");
    require(fn, "Missing authorised open");
    int fd = fn(batch, p.c_str());
    if (fd < 0 && errno == ECANCELED) throw Cancelled("Administrator operation cancelled");
    require(fd >= 0, "Administrator could not open selected preset");
    return fd;
#endif
}
} // namespace cab
