#pragma once
#include "../../shared/text.hpp"
#include "../../shared/preset_copy.hpp"
#include "../../shared/version.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace cab {
inline constexpr const char *Version = CAB_VERSION;
inline bool copying(const std::string &op) { return op == "copy" || op == "copy-enable"; }
inline bool enabling(const std::string &op) { return op == "enable" || op == "copy-enable"; }
inline constexpr size_t MaxBytes = 4 * 1024 * 1024, MaxPlanBytes = 64 * 1024 * 1024;
using Bytes = std::string;
using Path = std::string;
struct Error : std::runtime_error {
    using runtime_error::runtime_error;
};
struct IOError : Error {
    int code;
    explicit IOError(const std::string &message, int value = 0) : Error(message), code(value) {}
};
struct Cancelled : Error {
    using Error::Error;
};
inline void require(bool ok, const std::string &why) {
    if (!ok) throw Error(why);
}
std::string lower(std::string), trim(std::string), json(const std::string &);
inline bool generatedCopyFolder(std::string name) {
    name = lower(std::move(name));
    return name == "cab disabled" || name == "cab enabled";
}
using nd::hex;
using nd::unhex;
using nd::utf8;
std::string encode16(const std::string &, bool);
Path absolute(const Path &), join(const Path &, const Path &), parent(const Path &),
    filename(const Path &), stem(const Path &), extension(const Path &),
    relative(const Path &, const Path &), home(), desktop(), executable(), defaultBackup();
std::vector<std::string> parts(const Path &);
bool exists(const Path &), directory(const Path &), writable(const Path &), permissionError(int);
void safePath(const Path &), mkdirs(const Path &), removeFile(const Path &);
struct Entry {
    Path path;
    bool dir = false, regular = false, link = false;
    uint64_t volumeId = 0, fileIndex = 0; // Optional enumeration identity, not write authorization.
};
std::vector<Entry> entries(const Path &);
// Platform-specific identity and change metadata; compare within the same platform.
struct Signature {
    std::array<uint64_t, 8> values{};
    bool operator==(const Signature &o) const { return values == o.values; }
    bool operator!=(const Signature &o) const { return !(*this == o); }
};
using Handle = intptr_t;
inline constexpr Handle Invalid = -1;
struct File {
    Handle handle = Invalid;
    File() = default;
    explicit File(Handle h) : handle(h) {};
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&o) noexcept : handle(o.handle) { o.handle = Invalid; }
    File &operator=(File &&) noexcept;
    Handle release() {
        auto result = handle;
        handle = Invalid;
        return result;
    }
    ~File();
};
File openRead(const Path &), openWrite(const Path &), createExclusive(const Path &);
Signature signature(Handle), signature(const Path &);
std::string identity(const Path &);
std::string identity(const Entry &);
std::string identity(const Path &, const Signature &);
Bytes readHandle(Handle, size_t limit = MaxBytes);
Bytes readCount(Handle, size_t);
void writeAll(Handle, const Bytes &), sync(Handle), seek(Handle, uint64_t),
    truncate(Handle, uint64_t);
uint64_t position(Handle);
struct Snapshot {
    Bytes data;
    Signature stamp;
};
Snapshot readSnapshot(const Path &);
std::string sha256(const Bytes &);
uint64_t entropy();
struct Authorization;
void writeOriginal(const Path &, const Bytes &, const Signature &, const std::string &,
                   Authorization *authorization = nullptr);
void publishCopy(const Path &, const Bytes &, const std::function<void()> &recheck);
// An unpublished output lives in private staging; publication never replaces a destination.
struct StagedFile {
    struct State;
    std::unique_ptr<State> state;
    File file;
    explicit StagedFile(const Path &);
    ~StagedFile();
    File reopen() const;
    void publish();
};
enum class ValueKind { Void, Bool, Int, Int64, Double, String, Undefined, Binary, Array };
struct Scalar {
    std::string name, value;
    ValueKind kind = ValueKind::Void;
    int64_t integer = 0;
    double number = 0;
    size_t start = 0, end = 0;
};
struct Node {
    std::string name;
    std::vector<Scalar> props;
    std::vector<Node> children;
    std::optional<Scalar> text;
    const Scalar *findProperty(std::string_view) const;
    const Scalar &property(std::string_view) const;
};
struct Inspection {
    std::string format, product, version, schema, display;
    size_t start = 0, end = 0;
    bool enabled = false;
    Bytes off, on;
};
Inspection inspect(const Bytes &);
Bytes patch(const Bytes &, bool);
std::string representation(const Bytes &);
struct Source {
    Path path, library;
    std::string product, category;
};
struct Root {
    Path path;
    bool boundary = false;
};
struct Library {
    Path path;
    std::string product;
    std::vector<Source> sources;
};
struct Discovery {
    std::vector<Library> libraries;
    std::vector<std::string> issues;
};
std::vector<Root> defaultRoots();
Discovery discover(const std::vector<Root> &, std::atomic<bool> &);
bool categoryFolder(const std::string &);
struct Selection {
    int parent = -1;
    std::string label, location;
    std::optional<Source> source;
};
std::vector<Selection> selectionTree(const std::vector<Library> &, const std::set<Path> &);
struct Item {
    Source source;
    Path target;
    Signature stamp;
    std::string hash, status, reason;
    Bytes output;
};
// Review captures expected source state and output bytes; Apply must revalidate them.
struct Plan {
    std::string operation;
    std::vector<Item> items;
    bool complete = true;
};
struct Outcome {
    Path source;
    std::string status, reason;
};
Plan preview(const std::vector<Source> &, const std::string &, std::atomic<bool> &,
             const Path &destination = "", bool elevation = true);
using Progress = std::function<void(const Outcome &)>;
using Status = std::function<void(const std::string &)>;
void createBackup(const Plan &, const Path &, std::atomic<bool> &, Status);
struct Authorization {
    void *library = nullptr, *batch = nullptr;
    Authorization() = default;
    Authorization(const Authorization &) = delete;
    Authorization &operator=(const Authorization &) = delete;
    ~Authorization();
    bool available() const;
    void authorize(const std::vector<Path> &, std::atomic<bool> *cancel = nullptr);
    Handle open(const Path &);
};
std::vector<Outcome> apply(const Plan &, const Path &, std::atomic<bool> &, Progress = {},
                           Status = {}, bool elevation = true);
int server();
int cli(const std::vector<std::string> &);
} // namespace cab
