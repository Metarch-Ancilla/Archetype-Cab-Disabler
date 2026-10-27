#include "core.hpp"
#include <cstdio>
#include <ctime>
namespace cab {
static void u16(Bytes &b, uint16_t n) {
    b += char(n);
    b += char(n >> 8);
}
static void u32(Bytes &b, uint32_t n) {
    for (int i = 0; i < 4; ++i) b += char(n >> (i * 8));
}
static void u64(Bytes &b, uint64_t n) {
    for (int i = 0; i < 8; ++i) b += char(n >> (i * 8));
}
static uint32_t crc(const Bytes &data) {
    static const auto table = []() {
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c >> 1) ^ (0xedb88320u & uint32_t(-(int32_t)(c & 1)));
            out[i] = c;
        }
        return out;
    }();
    uint32_t c = 0xffffffff;
    for (unsigned char b : data) c = table[(c ^ b) & 255] ^ (c >> 8);
    return c ^ 0xffffffff;
}
struct ZipRecord {
    std::string name, hash;
    Bytes header;
    uint64_t offset = 0, dataOffset = 0;
    uint32_t size = 0, crc = 0, mode = 0;
    uint16_t time = 0, date = 33;
};
static ZipRecord append(File &file, const std::string &name, const Bytes &data, uint32_t mode,
                        uint64_t modified = 0, const std::string *verifiedHash = nullptr) {
    require(name.size() <= 65535 && data.size() < 0xffffffffu,
            "ZIP entry exceeds supported length");
    ZipRecord r;
    r.name = name;
    // Preset buffers were just verified; generated entries compute their own hash.
    r.hash = verifiedHash ? *verifiedHash : sha256(data);
    r.offset = position(file.handle);
    r.size = (uint32_t)data.size();
    r.crc = crc(data);
    r.mode = mode;
    if (modified) {
        time_t seconds = (time_t)(modified / 1000000000);
        std::tm local{};
#ifdef _WIN32
        bool valid = localtime_s(&local, &seconds) == 0;
#else
        bool valid = localtime_r(&seconds, &local) != nullptr;
#endif
        if (valid && local.tm_year >= 80 && local.tm_year <= 207) {
            r.time = uint16_t((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
            r.date =
                uint16_t(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
        }
    }
    auto &b = r.header;
    u32(b, 0x04034b50);
    u16(b, 20);
    u16(b, 0x0800);
    u16(b, 0);
    u16(b, r.time);
    u16(b, r.date);
    u32(b, r.crc);
    u32(b, r.size);
    u32(b, r.size);
    u16(b, (uint16_t)name.size());
    u16(b, 0);
    b += name;
    writeAll(file.handle, b);
    r.dataOffset = position(file.handle);
    writeAll(file.handle, data);
    return r;
}
static std::string anchor(const Path &p) {
    auto items = parts(p);
    if (p.rfind("//", 0) == 0 && items.size() >= 2) return "//" + items[0] + "/" + items[1];
    if (!items.empty() && items[0].size() == 2 && items[0][1] == ':') return items[0] + "/";
    return "/";
}
static bool inside(const Path &p, const Path &base) {
    if (p == base) return true;
    try {
        relative(p, base);
        return true;
    } catch (...) {
        return false;
    }
}
void createBackup(const Plan &plan, const Path &destination, std::atomic<bool> &cancel,
                  Status status) {
    require(plan.complete, "Preview incomplete; review before backup");
    std::vector<const Item *> items;
    std::set<std::string> seen;
    for (auto &i : plan.items)
        if (i.status == "ready" && seen.insert(identity(i.source.path, i.stamp)).second)
            items.push_back(&i);
    if (items.empty()) return;
    auto target = absolute(destination);
    require(lower(extension(target)) == ".zip", "Choose a .zip backup filename");
    safePath(target);
    require(!exists(target), "Backup already exists; choose another filename");
    mkdirs(parent(target));
    std::map<Path, Path> roots;
    for (auto *i : items) {
        auto a = anchor(i->source.path), p = parent(i->source.path);
        if (!roots.count(a))
            roots[a] = p;
        else
            while (!inside(p, roots[a])) {
                auto up = parent(roots[a]);
                require(up != roots[a], "Cannot group backup roots");
                roots[a] = up;
            }
    }
    std::map<Path, std::string> prefixes;
    for (auto &root : roots) {
        std::string prefix = "presets";
        // Different drives/shares need distinct namespaces; ordinary backups
        // place their relative directory tree directly inside presets/.
        if (roots.size() > 1)
            for (auto component : parts(root.first)) {
                component.erase(std::remove(component.begin(), component.end(), ':'), component.end());
                if (!component.empty()) prefix += "/" + component;
            }
        prefixes[root.first] = prefix;
    }
    StagedFile staged(target);
    std::vector<ZipRecord> records;
    Bytes central, tail, manifest;
    uint64_t centralOffset = 0, tailOffset = 0;
    {
        auto &file = staged.file;
        manifest = "{\"format\":1,\"description\":\"Original preset bytes before this "
                   "batch\",\"files\":[";
        size_t index = 0;
        for (auto *i : items) {
            if (cancel) throw Cancelled("Backup cancelled; no presets edited");
            auto snap = readSnapshot(i->source.path);
            require(snap.stamp == i->stamp && sha256(snap.data) == i->hash,
                    "Source changed since preview: " + i->source.path);
            auto a = anchor(i->source.path),
                 name = prefixes[a] + "/" + relative(i->source.path, roots[a]);
#ifdef _WIN32
            uint32_t mode = (i->stamp.values[5] & 1) ? 0100444 : 0100666;
            uint64_t modified = (i->stamp.values[3] - 116444736000000000ull) * 100;
#else
            uint32_t mode = (uint32_t)i->stamp.values[5];
            uint64_t modified = i->stamp.values[3];
#endif
            records.push_back(append(file, name, snap.data, mode, modified, &i->hash));
            if (index) manifest += ",";
            manifest += "{\"entry\":" + json(name) + ",\"source\":" + json(i->source.path) +
                        ",\"sha256\":" + json(i->hash) +
                        ",\"mtime_ns\":" + std::to_string(modified) +
                        ",\"mode\":" + std::to_string(mode & 07777) + "}";
            require(manifest.size() <= MaxPlanBytes, "Backup manifest resource limit exceeded");
            ++index;
            if (status)
                status("Creating ZIP: " + std::to_string(index) + "/" +
                       std::to_string(items.size()));
        }
        manifest += "]}";
        records.push_back(append(file, "backup-manifest.json", manifest, 0100600));
        centralOffset = position(file.handle);
        for (auto &r : records) {
            Bytes extra;
            if (r.offset >= 0xffffffffu) {
                u16(extra, 1);
                u16(extra, 8);
                u64(extra, r.offset);
            }
            u32(central, 0x02014b50);
            u16(central, 0x031e);
            u16(central, extra.empty() ? 20 : 45);
            u16(central, 0x0800);
            u16(central, 0);
            u16(central, r.time);
            u16(central, r.date);
            u32(central, r.crc);
            u32(central, r.size);
            u32(central, r.size);
            u16(central, (uint16_t)r.name.size());
            u16(central, (uint16_t)extra.size());
            u16(central, 0);
            u16(central, 0);
            u16(central, 0);
            u32(central, r.mode << 16);
            u32(central, (uint32_t)std::min<uint64_t>(r.offset, 0xffffffffu));
            central += r.name + extra;
        }
        writeAll(file.handle, central);
        tailOffset = position(file.handle);
        bool large = centralOffset >= 0xffffffffu || central.size() >= 0xffffffffu ||
                     records.size() >= 65535;
        if (large) {
            u32(tail, 0x06064b50);
            u64(tail, 44);
            u16(tail, 45);
            u16(tail, 45);
            u32(tail, 0);
            u32(tail, 0);
            u64(tail, records.size());
            u64(tail, records.size());
            u64(tail, central.size());
            u64(tail, centralOffset);
            u32(tail, 0x07064b50);
            u32(tail, 0);
            u64(tail, tailOffset);
            u32(tail, 1);
        }
        u32(tail, 0x06054b50);
        u16(tail, 0);
        u16(tail, 0);
        u16(tail, (uint16_t)std::min<size_t>(65535, records.size()));
        u16(tail, (uint16_t)std::min<size_t>(65535, records.size()));
        u32(tail, (uint32_t)std::min<uint64_t>(central.size(), 0xffffffffu));
        u32(tail, (uint32_t)std::min<uint64_t>(centralOffset, 0xffffffffu));
        u16(tail, 0);
        writeAll(file.handle, tail);
        sync(file.handle);
    }
    // Reopen and verify the completed archive, including headers and every stored payload.
    auto before = signature(staged.file.handle);
    require(before.values[2] == tailOffset + tail.size(), "Backup length mismatch");
    auto file = staged.reopen();
    for (auto &r : records) {
        if (cancel) throw Cancelled("Backup verification cancelled; no presets edited");
        seek(file.handle, r.offset);
        require(readCount(file.handle, r.header.size()) == r.header,
                "Backup local header verification failed");
        auto data = readCount(file.handle, r.size);
        require(sha256(data) == r.hash && crc(data) == r.crc,
                "Backup entry hash/CRC verification failed");
        if (r.name == "backup-manifest.json")
            require(data == manifest, "Backup manifest verification failed");
    }
    seek(file.handle, centralOffset);
    require(readCount(file.handle, central.size()) == central &&
                readCount(file.handle, tail.size()) == tail,
            "Backup central directory verification failed");
    require(signature(staged.file.handle) == before && signature(file.handle) == before,
            "Backup changed during verification");
    file = File();
    staged.publish();
}
} // namespace cab
