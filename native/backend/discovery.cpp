#include "core.hpp"
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace cab {
[[maybe_unused]] static Path environment(const char *name) {
#ifdef _WIN32
    std::wstring key;
    for (; *name; ++name) key += wchar_t(*name);
    DWORD n = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!n) return "";
    std::wstring value(n, 0);
    n = GetEnvironmentVariableW(key.c_str(), value.data(), n);
    value.resize(n);
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr,
                                   nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
#else
    auto value = getenv(name);
    return value ? value : "";
#endif
}
std::vector<Root> defaultRoots() {
#ifdef _WIN32
    auto path = environment("PROGRAMDATA");
    return path.empty() ? std::vector<Root>{}
                        : std::vector<Root>{{join(absolute(path), "Neural DSP"), false}};
#elif defined(__APPLE__)
    return {{"/Library/Audio/Presets/Neural DSP", false},
            {join(home(), "Library/Audio/Presets/Neural DSP"), false}};
#else
    return {};
#endif
}
static int folderCategory(std::string name) {
    name = lower(std::move(name));
    if (name == "user" || name == "users" || name == "user presets") return 3;
    if (name == "artist" || name == "artists") return 2;
    return name == "factory" || name == "factory presets" || name == "neural dsp" ? 1 : 0;
}
bool categoryFolder(const std::string &name) { return folderCategory(name) != 0; }
static std::string category(const Source &s) {
    auto folders = parts(relative(s.path, s.library));
    if (!folders.empty()) folders.pop_back();
    int found = 0;
    for (auto &name : folders) found = std::max(found, folderCategory(std::move(name)));
    static constexpr const char *names[] = {"Unknown", "Factory", "Artist", "User"};
    return names[found];
}
Discovery discover(const std::vector<Root> &roots, std::atomic<bool> &cancel) {
    Discovery result;
    std::map<std::string, Source> files;
    size_t remaining = 50000, sourceBytes = 0;
    for (auto &root : roots) {
        auto base = absolute(root.path);
        if (!exists(base) || generatedCopyFolder(filename(base))) continue;
        try {
            safePath(base);
            require(directory(base), "Not a preset folder");
        } catch (const std::exception &e) {
            result.issues.push_back(e.what());
            continue;
        }
        std::vector<std::pair<Path, int>> stack = {{base, 0}};
        std::set<Path> visited;
        while (!stack.empty() && remaining && !cancel) {
            auto current = std::move(stack.back());
            stack.pop_back();
            if (!visited.insert(current.first).second) continue;
            try {
                for (auto &e : entries(current.first)) {
                    if (cancel || !remaining) break;
                    --remaining;
                    if (e.link) continue;
                    if (e.dir) {
                        if (generatedCopyFolder(filename(e.path))) continue;
                        if (current.second < 8)
                            stack.emplace_back(e.path, current.second + 1);
                        else
                            result.issues.push_back("Scan depth limit");
                    } else if (e.regular && lower(extension(e.path)) == ".xml" &&
                               lower(filename(e.path)) != "reset all settings.xml") {
                        Path library = base;
                        auto rel = parts(relative(e.path, base));
                        if (!root.boundary && rel.size() > 1 && !categoryFolder(rel[0])) {
                            library = join(base, rel[0]);
                            if (rel.size() > 2 && lower(rel[1]) == "presets")
                                library = join(library, rel[1]);
                        }
                        Source source{e.path, library,
                                      lower(filename(library)) == "presets"
                                          ? filename(parent(library))
                                          : filename(library),
                                      "Unknown"};
                        source.category = category(source);
                        auto key = identity(e);
                        auto found = files.find(key);
                        if (found == files.end() ||
                            parts(source.library).size() > parts(found->second.library).size())
                        {
                            auto memory = [](const Source &s) { return s.path.size()+s.library.size()+s.product.size()+s.category.size(); };
                            size_t previous=found==files.end()?0:memory(found->second), current=memory(source);
                            require(current<=MaxPlanBytes-(sourceBytes-previous),"Discovery path memory limit exceeded");
                            sourceBytes=sourceBytes-previous+current;
                            files[key] = std::move(source);
                        }
                    }
                }
            } catch (const std::exception &e) {
                result.issues.push_back(e.what());
            }
        }
        if (!remaining) {
            result.issues.push_back("Scan entry limit reached");
            break;
        }
    }
    if (cancel) result.issues.push_back("Discovery cancelled");
    std::map<std::pair<std::string, Path>, std::vector<Source>> groups;
    for (auto &item : files)
        groups[{item.second.product, item.second.library}].push_back(std::move(item.second));
    for (auto &g : groups) {
        std::sort(g.second.begin(), g.second.end(),
                  [](const Source &a, const Source &b) { return nd::lessFolded(a.path, b.path); });
        result.libraries.push_back({g.first.second, g.first.first, std::move(g.second)});
    }
    return result;
}
std::vector<Selection> selectionTree(const std::vector<Library> &libraries,
                                     const std::set<Path> &selected) {
    std::vector<Selection> out;
    for (auto &lib : libraries) {
        if (!selected.count(lib.path)) continue;
        int root = (int)out.size();
        out.push_back({-1, lib.product, lib.path, {}});
        std::map<Path, int> folders{{"", root}};
        // Compare each path level so folders precede loose presets at every depth.
        using SortKey = std::vector<std::tuple<bool, std::string, std::string>>;
        struct Row {
            const Source *source;
            SortKey key;
        };
        std::vector<Row> rows;
        rows.reserve(lib.sources.size());
        for (auto &source : lib.sources) {
            auto names = parts(relative(source.path, lib.path));
            SortKey key;
            key.reserve(names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                auto folded = lower(names[i]);
                key.emplace_back(i + 1 == names.size(), std::move(folded), std::move(names[i]));
            }
            rows.push_back({&source, std::move(key)});
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Row &a, const Row &b) { return a.key < b.key; });
        for (auto &row : rows) {
            const auto &s = *row.source;
            Path dir = "";
            int p = root;
            for (size_t i = 0; i + 1 < row.key.size(); ++i) {
                const auto &name = std::get<2>(row.key[i]);
                dir = join(dir, name);
                if (!folders.count(dir)) {
                    folders[dir] = (int)out.size();
                    out.push_back({p, name, "", {}});
                }
                p = folders[dir];
            }
            out.push_back({p, stem(s.path), "", s});
        }
    }
    return out;
}
} // namespace cab
