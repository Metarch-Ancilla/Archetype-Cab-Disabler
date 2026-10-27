#include "../../frontends/shared/wire.hpp"
#include "core.hpp"
#include "../../shared/channel.hpp"
#include <condition_variable>
#include <csignal>
#include <deque>
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace cab {
namespace {
struct Queue {
    struct Event { std::function<void()> run; size_t bytes; std::string key; };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Event> events;
    size_t commandBytes = 0, commandCount = 0;
    bool post(std::function<void()> f, size_t bytes = 0, std::string key = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (bytes && (commandCount >= 256 || bytes > 8*1024*1024-commandBytes)) return false;
            if (!key.empty()) for (auto &event : events) if (event.key == key) {
                event.run = std::move(f); return true;
            }
            if (bytes) { commandBytes += bytes; ++commandCount; }
            events.push_back({std::move(f),bytes,std::move(key)});
        }
        ready.notify_one(); return true;
    }
    std::function<void()> next() {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return !events.empty(); });
        auto event = std::move(events.front()); events.pop_front();
        if(event.bytes){commandBytes -= event.bytes; --commandCount;}
        return std::move(event.run);
    }
};
// Session state belongs to the event loop; workers post progress and completion through Queue.
struct Session {
    Queue &queue;
    nd::CommandWriter output{128*1024*1024};
    std::atomic<bool> cancel{false};
    std::thread worker;
    bool busy = false, quit = false, disconnected = false, backup = true;
    int page = 0;
    size_t edited = 0, total = 0;
    std::vector<Root> roots = defaultRoots();
    Discovery discovery;
    bool scanned = false;
    std::set<Path> selected, selectedFiles, initialized;
    std::vector<Selection> nodes;
    std::shared_ptr<Plan> plan;
    Path backupPath = defaultBackup(), reviewedBackup;
    std::string operation = "disable", reviewSummary, report, resultSummary, resultDetails, status,
                completion = "Ready";
    explicit Session(Queue &q) : queue(q) {}
    ~Session() {
        cancel = true;
        if (worker.joinable()) worker.join();
        closeOutput();
    }
    void startOutput() {
#ifndef _WIN32
        int flags=fcntl(STDOUT_FILENO,F_GETFL);
        require(flags>=0&&!fcntl(STDOUT_FILENO,F_SETFL,flags|O_NONBLOCK),"Cannot configure backend output");
#endif
        output.start([this](const std::string &text) {
            size_t offset=0;
#ifndef _WIN32
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
#endif
            while(offset<text.size()&&!output.stopping()) {
#ifdef _WIN32
                DWORD amount=0;
                if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data()+offset,
                    (DWORD)std::min<size_t>(8192,text.size()-offset),&amount,nullptr)||!amount)return false;
#else
                if(std::chrono::steady_clock::now()>=deadline)return false;
                pollfd pipe{STDOUT_FILENO,POLLOUT,0};int ready=poll(&pipe,1,100);
                if(ready<0&&errno==EINTR)continue;
                if(ready<0)return false;if(!ready)continue;
                ssize_t amount=::write(STDOUT_FILENO,text.data()+offset,std::min<size_t>(8192,text.size()-offset));
                if(amount<0&&(errno==EINTR||errno==EAGAIN))continue;
                if(amount<=0)return false;
#endif
                offset+=(size_t)amount;
            }
            return offset==text.size();
        },[this] {
            cancel=true;
            queue.post([this]{disconnected=true;quit=true;});
        });
    }
    void closeOutput() {
        output.close();
#ifdef _WIN32
        output.interrupt();
#endif
        output.join();
    }
    bool copying() const { return cab::copying(operation); }
    bool enabling() const { return cab::enabling(operation); }
    void write(const std::string &s) {
        if (disconnected) return;
        if (!output.post(s)) {
            disconnected = true; cancel = true; quit = true;
        }
    }
    static nd::Control control(std::string id, std::string kind, std::string label = "",
                               std::string value = "", int height = 30) {
        nd::Control c;
        c.id = std::move(id);
        c.kind = std::move(kind);
        c.label = std::move(label);
        c.value = std::move(value);
        c.height = height;
        return c;
    }
    static nd::Option option(std::string value, std::string label, bool selected = false,
                             int parent = -1, std::string kind = "", std::string location = "") {
        nd::Option o;
        o.value = std::move(value);
        o.label = std::move(label);
        o.selected = selected;
        o.parent = parent;
        o.kind = std::move(kind);
        o.location = std::move(location);
        return o;
    }
    nd::Model view() const {
        static const char *titles[] = {"Select Plugins", "Select presets", "Backup",
                                       "Edit mode",      "Review",         "Results"};
        nd::Model m;
        m.page = page;
        m.busy = busy;
        m.back = page != 0 && page != 5 && !busy;
        m.next = !busy;
        m.nextLabel = page == 4 ? "Apply" : page == 5 ? "Finish" : "Next";
        m.title = titles[page];
        m.status = status;
        m.completion = completion;
        m.edited = (long)edited;
        m.total = (long)total;
        if (page == 0) {
            auto c = control("plugins", "plugins", "", "", 400);
            c.options.reserve(discovery.libraries.size());
            for (size_t i = 0; !busy && i < discovery.libraries.size(); ++i) {
                auto &l = discovery.libraries[i];
                c.options.push_back(option(std::to_string(i), l.product, selected.count(l.path), -1,
                                           "plugin", l.path));
            }
            m.controls.push_back(std::move(c));
            c = control("browse", "buttons", "Preset folders not detected?");
            c.options.push_back(option("add", "Browse…"));
            m.controls.push_back(std::move(c));
        } else if (page == 1) {
            auto c = control("presets", "tree", "", "", 440);
            c.options.reserve(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i) {
                auto &n = nodes[i];
                c.options.push_back(option(std::to_string(i), n.label,
                                           n.source && selectedFiles.count(n.source->path),
                                           n.parent, n.source ? "preset" : "folder", n.location));
            }
            m.controls.push_back(std::move(c));
        } else if (page == 2) {
            auto c = control("backup", "radio", "Create backup?", backup ? "1" : "0");
            c.options = {option("1", "Yes", backup), option("0", "No", !backup)};
            m.controls.push_back(std::move(c));
            m.controls.push_back(control("backup_path", "readonly", "Location", backupPath, 24));
            c = control("save", "buttons");
            c.options.push_back(option("save_backup", "Browse…"));
            m.controls.push_back(std::move(c));
        } else if (page == 3) {
            auto c = control("disable", "pair", "Cab simulation", enabling() ? "0" : "1", 42);
            c.options = {option("1", "Disable", !enabling(), -1, "",
                                "Disable cab simulation in the selected presets."),
                         option("0", "Enable", enabling(), -1, "",
                                "Enable cab simulation in the selected presets.")};
            m.controls.push_back(std::move(c));
            c = control("copy", "pair", "Edit mode", copying() ? "1" : "0", 116);
            c.options = {
                option("0", "Direct Edit", !copying(), -1, "",
                       "Modify the saved presets directly, maintaining their names and locations "
                       "within the preset browser.\nDirect edit may require elevated permissions."),
                option("1", "Copy and Edit", copying(), -1, "",
                       nd::copyDescription(enabling()))};
            m.controls.push_back(std::move(c));
        } else if (page == 4) {
            m.controls.push_back(control("review_summary", "readonly", "", reviewSummary, 72));
            m.controls.push_back(control("report", "text", "", report, 410));
            if (!backup && !busy)
                m.controls.push_back(control("backup_notice", "notice", "",
                                             "Backup not enabled. Proceed at your own risk.",
                                             24));
        } else {
            m.controls.push_back(control("summary", "readonly", "", resultSummary, 44));
            m.controls.push_back(control("details", "text", "Errors:", resultDetails, 350));
        }
        return m;
    }
    void emit() {
        auto m = view();
        std::string s = "STATE\t" + std::to_string(m.page) + "\t" + (m.busy ? "1" : "0") + "\t" +
                        (m.back ? "1" : "0") + "\t" + (m.next ? "1" : "0") + "\t" +
                        hex(m.nextLabel) + "\t" + hex(m.title) + "\t" + hex(m.status) + "\t" +
                        hex(m.completion) + "\t" + std::to_string(m.edited) + "\t" +
                        std::to_string(m.total) + "\n";
        for (auto &c : m.controls) {
            s += "CONTROL\t" + c.id + "\t" + c.kind + "\t" + std::to_string(c.height) + "\t" +
                 hex(c.label) + "\t" + hex(c.value) + "\n";
            for (auto &o : c.options)
                s += "OPTION\t" + c.id + "\t" + hex(o.value) + "\t" + hex(o.label) + "\t" +
                     (o.selected ? "1" : "0") + "\t" + std::to_string(o.parent) + "\t" + o.kind +
                     "\t" + (o.enabled ? "1" : "0") + "\t" + hex(o.location) + "\n";
        }
        write(s + "END\n");
    }
    // Work runs off-thread and returns the state update to execute on the event loop.
    void launch(const std::string &kind, std::function<std::function<void()>()> work) {
        require(!busy && !worker.joinable(), "Wait for current operation");
        busy = true;
        cancel = false;
        completion = "Working...";
        status = kind == "scan"     ? ""
                 : kind == "review" ? "Preparing review..."
                                    : "Editing presets...";
        if (kind != "apply") edited = total = 0;
        worker = std::thread([this, work = std::move(work)]() {
            try {
                auto done = work();
                queue.post([this, done = std::move(done)]() {
                    worker.join();
                    busy = false;
                    completion = "Ready";
                    done();
                    emit();
                });
            } catch (const std::exception &e) {
                auto why = std::string(e.what());
                queue.post([this, why]() {
                    worker.join();
                    busy = false;
                    completion = "Failed";
                    status = why;
                    write("ERROR\t" + hex(why) + "\n");
                    emit();
                });
            }
        });
    }
    void scan() {
        launch("scan", [this, scanRoots = roots]() {
            auto result = discover(scanRoots, cancel);
            return [this, result = std::move(result)]() mutable {
                discovery = std::move(result);
                scanned = true;
                std::set<Path> keep;
                for (auto &l : discovery.libraries)
                    if (selected.count(l.path)) keep.insert(l.path);
                selected = std::move(keep);
                status.clear();
                plan.reset();
            };
        });
    }
    static std::set<size_t> indices(const std::string &raw, size_t count) {
        std::set<size_t> out;
        size_t begin = 0;
        while (begin < raw.size()) {
            auto end = raw.find(',', begin);
            if (end == raw.npos) end = raw.size();
            auto token = raw.substr(begin, end - begin);
            if (!token.empty()) {
                require(token.find_first_not_of("0123456789") == token.npos && token.size() <= 10,
                        "Invalid selection");
                auto n = std::stoull(token);
                require(n < count, "Invalid selection");
                out.insert((size_t)n);
            }
            begin = end + 1;
        }
        return out;
    }
    void fields(const std::map<std::string, std::string> &values) {
        auto get = [&](const std::string &key, const std::string &fallback) {
            auto i = values.find(key);
            return i == values.end() ? fallback : i->second;
        };
        if (page == 0 && values.count("plugins")) {
            selected.clear();
            for (auto i : indices(values.at("plugins"), discovery.libraries.size()))
                selected.insert(discovery.libraries[i].path);
        } else if (page == 1 && values.count("presets")) {
            auto picked = indices(values.at("presets"), nodes.size());
            for (auto i : picked)
                require(nodes[i].source.has_value(), "Preset selection must contain files");
            for (auto &n : nodes)
                if (n.source) selectedFiles.erase(n.source->path);
            for (auto i : picked) selectedFiles.insert(nodes[i].source->path);
        } else if (page == 2) {
            auto value = get("backup", backup ? "1" : "0");
            require(value == "0" || value == "1", "Choose Yes or No for backup");
            backup = value == "1";
            backupPath = get("backup_path", backupPath);
        } else if (page == 3) {
            auto op = get("operation", operation);
            if (values.count("disable") || values.count("copy")) {
                auto disable = get("disable", enabling() ? "0" : "1"),
                     copy = get("copy", copying() ? "1" : "0");
                require((disable == "0" || disable == "1") && (copy == "0" || copy == "1"),
                        "Invalid edit options");
                op = copy == "1" ? (disable == "1" ? "copy" : "copy-enable")
                                 : (disable == "1" ? "disable" : "enable");
            }
            require(op == "copy" || op == "copy-enable" || op == "enable" || op == "disable",
                    "Invalid edit mode");
            operation = op;
        }
    }
    std::string hierarchy(const std::map<Path, std::string> &chosen, bool separate = false) const {
        std::vector<bool> visible(nodes.size());
        for (size_t i = nodes.size(); i-- > 0;)
            if ((nodes[i].source && chosen.count(nodes[i].source->path)) || visible[i]) {
                visible[i] = true;
                if (nodes[i].parent >= 0) visible[(size_t)nodes[i].parent] = true;
            }
        std::vector<int> depths;
        depths.reserve(nodes.size());
        std::string out;
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto &n = nodes[i];
            int depth = n.parent < 0 ? 0 : depths[(size_t)n.parent] + 1;
            depths.push_back(depth);
            if (!visible[i]) continue;
            if (!out.empty()) {
                out += '\n';
                if (separate && n.parent < 0) out += '\n';
            }
            out += std::string((size_t)depth * 4, ' ') + n.label;
            if (n.source) {
                auto found = chosen.find(n.source->path);
                if (found != chosen.end() && !found->second.empty()) out += " — " + found->second;
            }
        }
        return out;
    }
    using Replacements = std::vector<std::pair<std::string, std::string>>;
    Replacements reasonPaths() const {
        Replacements paths;
        for (auto &n : nodes) {
            if (!n.location.empty()) paths.emplace_back(n.location, n.label);
            if (n.source) paths.emplace_back(n.source->path, n.label);
        }
        if (plan)
            for (auto &i : plan->items)
                if (!i.target.empty()) paths.emplace_back(i.target, stem(i.target));
        if (!reviewedBackup.empty()) paths.emplace_back(reviewedBackup, filename(reviewedBackup));
        std::sort(paths.begin(), paths.end(),
                  [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
        return paths;
    }
    static void replaceAll(std::string &text, const std::string &from, const std::string &to) {
        size_t at = 0;
        while ((at = text.find(from, at)) != text.npos) {
            text.replace(at, from.size(), to);
            at += to.size();
        }
    }
    static std::string relativeReason(std::string why, const Replacements &paths) {
        // Longest paths first also handle preset names that prefix another filename.
        if (why.find_first_of("/\\") != why.npos)
            for (auto &path : paths) replaceAll(why, path.first, path.second);
        return why;
    }
    void next() {
        if (page == 0) {
            if (!scanned) {
                scan();
                return;
            }
            require(!selected.empty(), "Select at least one plugin");
            for (auto &l : discovery.libraries)
                if (selected.count(l.path) && !initialized.count(l.path)) {
                    for (auto &s : l.sources)
                        if (s.category != "User") selectedFiles.insert(s.path);
                    initialized.insert(l.path);
                }
            nodes = selectionTree(discovery.libraries, selected);
            page = 1;
        } else if (page == 1) {
            bool any = false;
            for (auto &n : nodes)
                if (n.source && selectedFiles.count(n.source->path)) any = true;
            require(any, "Select at least one preset");
            page = 2;
        } else if (page == 2) {
            if (backup) {
                require(!trim(backupPath).empty(), "Choose a new .zip filename or select No");
                bool abs = backupPath[0] == '/' || backupPath[0] == '~' ||
                           (backupPath.size() > 2 && backupPath[1] == ':');
                backupPath = absolute(abs ? backupPath : join(desktop(), backupPath));
                require(lower(extension(backupPath)) == ".zip" && !exists(backupPath),
                        "Choose a new .zip filename");
            }
            page = 3;
        } else if (page == 3) {
            std::vector<Source> sources;
            for (auto &n : nodes)
                if (n.source && selectedFiles.count(n.source->path)) sources.push_back(*n.source);
            require(!sources.empty(), "Select at least one preset");
            reviewedBackup = backup ? backupPath : "";
            auto op = operation;
            launch("review", [this, sources = std::move(sources), op]() {
                Authorization auth;
                auto result =
                    std::make_shared<Plan>(preview(sources, op, cancel, "", auth.available()));
                return [this, result]() {
                    plan = result;
                    std::map<Path, std::string> chosen;
                    for (auto &i : plan->items) chosen[i.source.path] = "";
                    reviewSummary =
                        "Mode: " + std::string(copying() ? "Copy and Edit" : "Direct edit") +
                        "\nBackup: " + (reviewedBackup.empty() ? "No" : reviewedBackup) +
                        "\nOperation: " + (enabling() ? "Enable" : "Disable") +
                        "\nPresets: " + std::to_string(plan->items.size());
                    report = hierarchy(chosen, true);
                    page = 4;
                    status.clear();
                };
            });
        } else if (page == 4) {
            require(plan && plan->complete && !cancel, "Return to Edit mode and review again");
            edited = 0;
            total = plan->items.size();
            auto reviewed = plan;
            auto saved = reviewedBackup;
            auto op = operation;
            launch("apply", [this, reviewed, saved, op]() {
                size_t writes = 0, processed = 0;
                auto last = std::chrono::steady_clock::time_point{}, lastStatus = last;
                auto progress = [&](const Outcome &o) {
                    if (o.status == "changed") ++writes;
                    ++processed;
                    auto now = std::chrono::steady_clock::now();
                    // Coalesce progress to limit UI traffic, but always deliver the final item.
                    if (now - last < std::chrono::milliseconds(50) &&
                        processed != reviewed->items.size())
                        return;
                    last = now;
                    auto count = writes;
                    std::string state =
                        o.status == "changed" ? "File"
                        : o.status == "already-correct"
                            ? (op == "enable" || op == "copy-enable" ? "already enabled"
                                                                     : "already disabled")
                            : o.status;
                    auto message = state + ": " + stem(o.source);
                    queue.post([this, count, message]() {
                        edited = count;
                        status = message;
                        write("PROGRESS\t" + std::to_string(edited) + "\t" + std::to_string(total) +
                              "\nSTATUS\t" + hex(status) + "\n");
                    },0,"progress");
                };
                auto notify = [&](const std::string &text) {
                    auto now = std::chrono::steady_clock::now();
                    if (text != "Direct edit requires elevated permissions" &&
                        now - lastStatus < std::chrono::milliseconds(50))
                        return;
                    lastStatus = now;
                    queue.post([this, text]() {
                        status = text;
                        write("STATUS\t" + hex(text) + "\n");
                    },0,"status");
                };
                auto result = apply(*reviewed, saved, cancel, progress, notify);
                return [this, result = std::move(result)]() {
                    size_t success = 0;
                    edited = 0;
                    bool stopped = false;
                    std::map<Path, std::string> errors, correct;
                    std::map<std::string, std::string> reasons;
                    std::optional<Replacements> paths;
                    for (auto &r : result) {
                        if (r.status == "changed" || r.status == "already-correct")
                            ++success;
                        else {
                            if (!paths) paths = reasonPaths();
                            auto found = reasons.find(r.reason);
                            if (found == reasons.end())
                                found = reasons.emplace(r.reason, relativeReason(r.reason, *paths))
                                            .first;
                            errors[r.source] = found->second;
                        }
                        if (r.status == "changed") ++edited;
                        if (r.status == "already-correct") correct[r.source] = "";
                        if (r.status == "cancelled") stopped = true;
                    }
                    completion = stopped ? "Cancelled" : "Finished";
                    resultSummary = "Successful: " + std::to_string(success) +
                                    "\nFailed: " + std::to_string(result.size() - success);
                    resultDetails = hierarchy(errors);
                    if (!correct.empty()) {
                        if (!resultDetails.empty()) resultDetails += "\n\n";
                        resultDetails +=
                            std::string(enabling() ? "Already enabled:" : "Already disabled:") +
                            "\n" + hierarchy(correct);
                    }
                    if (resultDetails.empty()) resultDetails = "No errors.";
                    // Results are complete strings; the reviewed bytes/tree cannot be reused.
                    Bytes{}.swap(report);
                    plan.reset();
                    discovery = {};
                    std::vector<Selection>{}.swap(nodes);
                    selected.clear();
                    selectedFiles.clear();
                    initialized.clear();
                    page = 5;
                    status.clear();
                };
            });
        } else
            throw Error("Finish closes the tool");
    }
    void command(const std::string &line) {
        auto columns = nd::split(line);
        std::string name = columns[0];
        require(!name.empty(), "Invalid command");
        for (char c : name)
            require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_',
                    "Invalid command");
        std::map<std::string, std::string> values;
        for (size_t i = 1; i < columns.size(); ++i) {
            auto at = columns[i].find('=');
            require(at != columns[i].npos, "Invalid command field");
            auto key = columns[i].substr(0, at);
            require(!values.count(key), "Duplicate command field");
            values[key] = unhex(columns[i].substr(at + 1));
        }
        if (name == "quit") {
            quit = true;
            cancel = true;
            if (busy) status = "Cancelling...";
            return;
        }
        if (name == "cancel") {
            cancel = true;
            status = "Cancelling...";
            write("STATUS\t" + hex(status) + "\n");
            return;
        }
        require(!busy, "Wait for current operation to finish");
        fields(values);
        if (name == "back") {
            if (page != 0 && page != 5) --page;
        } else if (name == "next")
            next();
        else if (name == "scan") {
            require(page == 0, "Return to Select Plugins");
            scan();
        } else if (name == "add") {
            require(page == 0 && values.count("path"), "Return to Select Plugins");
            auto path = absolute(values.at("path"));
            require(directory(path), "Choose a preset folder");
            if (categoryFolder(filename(path)) && lower(filename(path)) != "neural dsp")
                path = parent(path);
            bool collection = lower(filename(path)) == "neural dsp";
            if (!collection && directory(join(path, "Presets"))) path = join(path, "Presets");
            bool duplicate = false;
            for (auto &r : roots)
                if (r.path == path && r.boundary == !collection) duplicate = true;
            if (!duplicate) {
                require(roots.size()<1024,"Too many search folders");
                roots.push_back({path, !collection});
            }
            scan();
        } else if (name == "save_backup") {
            require(page == 2 && backup && values.count("path"),
                    "Select Yes on Backup to choose a location");
            backupPath = values.at("path");
        } else
            throw Error("Unknown action");
    }
};
} // namespace
int server() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    Queue queue;
    Session session(queue);
    session.startOutput();
    std::atomic<bool> stop{false}, urgentQueued{false}, quitRequested{false};
    std::thread reader([&]() {
        nd::Lines inputLines(1024 * 1024);
        char buffer[4096];
        while (!stop) {
#ifdef _WIN32
            HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                queue.post([&]() {
                    session.disconnected = true;
                    session.quit = true;
                    session.cancel = true;
                });
                return;
            }
            if (!available) {
                Sleep(20);
                continue;
            }
            DWORD amount = 0;
            if (!ReadFile(input, buffer, (DWORD)std::min<size_t>(sizeof(buffer), available),
                          &amount, nullptr) ||
                !amount) {
                queue.post([&]() {
                    session.disconnected = true;
                    session.quit = true;
                    session.cancel = true;
                });
                return;
            }
            size_t n = amount;
#else
            pollfd input{STDIN_FILENO, POLLIN, 0};
            int ready = poll(&input, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) continue;
            ssize_t amount = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (amount <= 0) {
                queue.post([&]() {
                    session.disconnected = true;
                    session.quit = true;
                    session.cancel = true;
                });
                return;
            }
            size_t n = (size_t)amount;
#endif
            try {
                inputLines.append(std::string_view(buffer, n), [&](std::string line) {
                    if(line=="cancel"||line=="quit") {
                        session.cancel=true;
                        if(line=="quit")quitRequested=true;
                        if(!urgentQueued.exchange(true))queue.post([&] {
                            urgentQueued=false;
                            session.command(quitRequested?"quit":"cancel");session.emit();
                        });
                        return;
                    }
                    size_t weight=line.size()+1;
                    if(!queue.post([&, line = std::move(line)]() {
                        try {
                            session.command(line);
                            session.emit();
                        } catch (const std::exception &e) {
                            session.write("ERROR\t" + hex(e.what()) + "\n");
                        }
                    },weight)) throw Error("Backend command queue exceeds safety limit");
                });
            } catch (const std::exception &) {
                queue.post([&]() {
                    session.quit = true;
                    session.cancel = true;
                    session.disconnected = true;
                });
                return;
            }
        }
    });
    session.scan();
    session.write("HELLO\t2\n");
    session.emit();
    while (!session.quit || session.busy) queue.next()();
    stop = true;
    session.cancel = true;
    reader.join();
    return 0;
}
} // namespace cab
