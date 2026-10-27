#include "core.hpp"
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace cab {
static void output(const std::string &text) {
    require(std::fwrite(text.data(), 1, text.size(), stdout) == text.size(), "Cannot write output");
}
int cli(const std::vector<std::string> &args) {
    if (args.empty()) return server();
    auto command = args[0];
    if (command == "--version") {
        output(std::string(Version) + '\n');
        return 0;
    }
    if (command == "--help" || command == "help") {
        output("CabBackend adapters | scan [--preset-root FOLDER] | inspect FILE... | edit "
               "{disable|enable|copy|copy-enable} FILE... [--library FOLDER] [--product "
               "NAME] [--destination FOLDER] [--backup ZIP] [--apply]\n");
        return 0;
    }
    if (command == "adapters") {
        output("[{\"id\":\"binary.valuetree\",\"product\":\"Binary ValueTree "
               "presets\",\"version\":\"structure detected from file; no plugin/version "
               "whitelist\"},{\"id\":\"text.xml\",\"product\":\"Text XML "
               "presets\",\"version\":\"structure detected from file; no plugin/version "
               "whitelist\"}]\n");
        return 0;
    }
    if (command == "scan") {
        auto roots = defaultRoots();
        for (size_t i = 1; i < args.size(); ++i) {
            require(args[i] == "--preset-root" && i + 1 < args.size(),
                    "Expected --preset-root FOLDER");
            roots.push_back({absolute(args[++i]), false});
        }
        std::atomic<bool> cancel{false};
        auto found = discover(roots, cancel);
        output("{\"libraries\":[");
        bool first = true;
        for (auto &l : found.libraries) {
            if (!first) output(",");
            first = false;
            output("{\"product\":" + json(l.product) + ",\"path\":" + json(l.path) +
                   ",\"count\":" + std::to_string(l.sources.size()) +
                   ",\"support\":\"Format detected per file during review\"}");
        }
        output("],\"issues\":[");
        first = true;
        for (auto &s : found.issues) {
            if (!first) output(",");
            first = false;
            output(json(s));
        }
        output("]}\n");
        return found.issues.empty() ? 0 : 1;
    }
    if (command == "inspect") {
        require(args.size() > 1, "Choose preset files");
        bool failed = false;
        for (size_t i = 1; i < args.size(); ++i) {
            auto p = absolute(args[i]);
            try {
                auto snap = readSnapshot(p);
                std::string adapter = "null", reason = "null";
                try {
                    auto info = inspect(snap.data);
                    adapter = "{\"product\":" + json(info.product) +
                              ",\"version\":" + json(info.version) +
                              ",\"format\":" + json(info.format) + ",\"cab_enabled\":[" +
                              (info.enabled ? "true" : "false") +
                              "],\"support\":\"structure validated; plugin loading "
                              "unverified\",\"span\":[" +
                              std::to_string(info.start) + "," + std::to_string(info.end) + "]}";
                } catch (const std::exception &e) {
                    reason = json(e.what());
                }
                output("{\"path\":" + json(p) + ",\"bytes\":" + std::to_string(snap.data.size()) +
                       ",\"representation\":" + json(representation(snap.data)) +
                       ",\"adapter\":" + adapter + ",\"reason\":" + reason + ",\"support\":" +
                       json(adapter == "null" ? "unsupported / read-only" : "structure validated") +
                       "}\n");
            } catch (const std::exception &e) {
                failed = true;
                output("{\"path\":" + json(p) + ",\"error\":" + json(e.what()) + "}\n");
            }
        }
        return failed ? 1 : 0;
    }
    if (command == "edit") {
        require(args.size() > 2, "Choose edit operation and preset files");
        auto operation = args[1];
        Path library, destination, backup;
        std::string product = "Auto-detected";
        bool perform = false, options = true;
        std::vector<Path> files;
        for (size_t i = 2; i < args.size(); ++i) {
            auto token = args[i];
            if (options && token == "--") {
                options = false;
                continue;
            }
            if (options && token == "--apply") {
                perform = true;
                continue;
            }
            if (options && (token == "--library" || token == "--product" ||
                            token == "--destination" || token == "--backup")) {
                require(i + 1 < args.size(), "Missing option value");
                auto value = args[++i];
                if (token == "--library")
                    library = absolute(value);
                else if (token == "--destination")
                    destination = absolute(value);
                else if (token == "--backup")
                    backup = absolute(value);
                else
                    product = value;
            } else {
                require(!options || token.rfind("--", 0) != 0, "Unknown edit option");
                files.push_back(absolute(token));
            }
        }
        require(!files.empty(), "Choose preset files");
        std::vector<Source> sources;
        for (auto &p : files)
            sources.push_back({p, library.empty() ? parent(p) : library, product, "Unknown"});
        std::atomic<bool> cancel{false};
        auto plan = preview(sources, operation, cancel, destination, false);
        bool failed = false;
        for (auto &i : plan.items) {
            output(i.status + ": " + i.source.path + " -> " +
                   (i.target.empty() ? "untouched" : i.target) + ": " + i.reason + '\n');
            if (i.status == "failed" || i.status == "skipped") failed = true;
        }
        if (!perform) return failed ? 1 : 0;
        auto result = apply(
            plan, backup, cancel, {},
            [](const std::string &s) { std::fprintf(stderr, "%s\n", s.c_str()); }, false);
        std::map<std::string, int> counts;
        failed = false;
        for (auto &o : result) {
            ++counts[o.status];
            if (o.status == "failed" || o.status == "skipped" || o.status == "cancelled")
                failed = true;
        }
        output("{");
        bool first = true;
        for (auto &c : counts) {
            if (!first) output(",");
            first = false;
            output(json(c.first) + ':' + std::to_string(c.second));
        }
        output("}\n");
        return failed ? 1 : 0;
    }
    throw Error("Unknown command; use --help");
}
} // namespace cab
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr,
                                    nullptr);
        if (!n) return 2;
        std::string value(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), n, nullptr, nullptr);
        value.resize(n - 1);
        args.push_back(value);
    }
    try {
        return cab::cli(args);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
#else
int main(int argc, char **argv) {
    try {
        return cab::cli(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
#endif
