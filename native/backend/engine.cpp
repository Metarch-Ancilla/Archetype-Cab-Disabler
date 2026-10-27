#include "core.hpp"
namespace cab {
static std::string targetKey(const Path &p) {
#ifdef _WIN32
    return lower(p);
#else
    return p;
#endif
}
// Preview computes edits without writing; failed or unsupported items retain a reason.
Plan preview(const std::vector<Source> &sources, const std::string &operation,
             std::atomic<bool> &cancel, const Path &destination, bool elevation) {
    require(operation == "disable" || operation == "enable" || copying(operation),
            "Unknown operation");
    Plan plan;
    plan.operation = operation;
    std::set<std::string> seen, targets;
    size_t total = 0;
    bool enabled = enabling(operation);
    for (auto &s : sources) {
        if (cancel) break;
        Item i;
        i.source = s;
        try {
            i.source.path = absolute(s.path);
            auto snap = readSnapshot(i.source.path);
            if (!seen.insert(identity(i.source.path, snap.stamp)).second) continue;
            i.stamp = snap.stamp;
            i.hash = sha256(snap.data);
            i.output = patch(snap.data, enabled);
            i.target = i.source.path;
            std::optional<Snapshot> existing;
            if (copying(operation)) {
                const auto library = absolute(s.library);
                auto rel = relative(i.source.path, library);
                for (auto &part : parts(parent(rel)))
                    require(!generatedCopyFolder(part),
                            "Generated copies cannot be copied recursively");
                Path folder = join(library, nd::copyFolder(enabled));
                if (!destination.empty()) {
                    require(!s.product.empty() && s.product != "." && s.product != ".." &&
                                s.product.find_first_of("/\\:") == s.product.npos,
                            "Unsafe product folder name");
                    folder = join(absolute(destination), s.product);
                }
                i.target = join(folder, rel);
                require(i.target != i.source.path, "Copy destination aliases source");
                // Use one verified snapshot for both alias detection and copy classification.
                if (exists(i.target)) {
                    existing = readSnapshot(i.target);
                    require(identity(i.target, existing->stamp) != identity(i.source.path, i.stamp),
                            "Copy destination aliases source");
                } else safePath(i.target);
            }
            require(targets.insert(targetKey(i.target)).second,
                    "Multiple sources map to the same destination");
            i.status = "ready";
            i.reason = "Validated cab scalar change";
            if (existing) {
                if (existing->data == i.output) {
                    i.status = "already-correct";
                    i.reason = "Identical copy already exists";
                } else {
                    i.status = "skipped";
                    i.reason = "Destination collision; choose another destination";
                }
            } else if (!copying(operation) && i.output == snap.data) {
                i.status = "already-correct";
                i.reason = "Cab section already in requested state";
            }
            if (i.status == "ready") {
                if (copying(operation)) {
                    Path folder = parent(i.target);
                    while (!exists(folder) && parent(folder) != folder) folder = parent(folder);
                    if (!writable(folder))
                        throw IOError("Write access denied for copy destination");
                }
                if (!copying(operation) && !elevation) {
                    try {
                        auto probe = openWrite(i.source.path);
                    } catch (...) {
                        throw IOError("Write access denied; no automatic elevation");
                    }
                }
                require(total + i.output.size() <= MaxPlanBytes,
                        "64 MiB plan limit exceeded; select a smaller batch");
                total += i.output.size();
            } else
                Bytes{}.swap(i.output);
        } catch (const IOError &e) {
            i.status = "failed";
            i.reason = e.what();
            Bytes{}.swap(i.output);
        } catch (const std::exception &e) {
            i.status = "skipped";
            i.reason = e.what();
            Bytes{}.swap(i.output);
        }
        plan.items.push_back(std::move(i));
    }
    plan.complete = !cancel;
    return plan;
}
std::vector<Outcome> apply(const Plan &plan, const Path &backup, std::atomic<bool> &cancel,
                           Progress progress, Status status, bool elevation) {
    std::vector<Outcome> out;
    out.reserve(plan.items.size());
    auto stopped = [&](const std::string &state, const std::string &why) {
        for (auto &i : plan.items) {
            Outcome o{i.source.path, i.status == "ready" ? state : i.status,
                      i.status == "ready" ? why : i.reason};
            out.push_back(o);
            if (progress) progress(o);
        }
        return out;
    };
    if (!plan.complete)
        return stopped("cancelled", "Preview incomplete; review again; no presets edited");
    if (cancel) return stopped("cancelled", "Cancelled before editing; no presets edited");
    // A requested backup must finish verification before permission prompts or preset writes.
    if (!backup.empty()) try {
            createBackup(plan, backup, cancel, status);
        } catch (const Cancelled &e) {
            return stopped("cancelled",
                           std::string("Backup stopped batch; no presets edited: ") + e.what());
        } catch (const std::exception &e) {
            return stopped("failed",
                           std::string("Backup stopped batch; no presets edited: ") + e.what());
        }
    // Probe the entire direct-edit batch first so authorization failure stops all edits.
    Authorization auth;
    bool needs = false;
    std::vector<Path> selected;
    if (!copying(plan.operation))
        for (auto &i : plan.items)
            if (i.status == "ready") {
                if (cancel) return stopped("cancelled", "Cancelled before authorization; no presets edited");
                selected.push_back(i.source.path);
                try {
                    auto probe = openWrite(i.source.path);
                } catch (const IOError &e) {
                    if (!permissionError(e.code)) return stopped("failed", e.what());
                    needs = true;
                } catch (const std::exception &e) {
                    return stopped("failed", e.what());
                }
            }
    if (needs) {
        try {
            if (cancel)
                throw Cancelled("Cancelled before administrator authorisation; no presets edited");
            require(elevation && auth.available(),
                    "Write access denied; native authorisation unavailable");
            if (status) status("Direct edit requires elevated permissions");
            auth.authorize(selected, &cancel);
        } catch (const Cancelled &e) {
            return stopped("cancelled", e.what());
        } catch (const std::exception &e) {
            return stopped("failed", e.what());
        }
    }
    for (auto &i : plan.items) {
        Outcome o{i.source.path, i.status, i.reason};
        if (cancel) {
            o.status = "cancelled";
            o.reason = "Not written; completed files remain valid";
        } else if (i.status == "ready") {
            try {
                auto recheck = [&]() {
                    auto snap = readSnapshot(i.source.path);
                    require(snap.stamp == i.stamp && sha256(snap.data) == i.hash,
                            "Source changed since preview; review again");
                };
                if (copying(plan.operation)) {
                    recheck();
                    publishCopy(i.target, i.output, recheck);
                } else {
                    // writeOriginal owns the reviewed identity/hash check on the opened handle.
                    writeOriginal(i.target, i.output, i.stamp, i.hash, needs ? &auth : nullptr);
                }
                o.status = "changed";
                o.reason = i.target;
            } catch (const Cancelled &e) {
                o.status = "cancelled";
                o.reason = e.what();
            } catch (const std::exception &e) {
                o.status = "failed";
                o.reason = e.what();
            }
        }
        out.push_back(o);
        if (progress) progress(o);
    }
    return out;
}
} // namespace cab
