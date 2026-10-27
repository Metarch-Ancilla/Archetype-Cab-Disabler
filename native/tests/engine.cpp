// Generated-input integration checks for immutable plans and cancellation.
#include "../backend/core.hpp"
#include <iostream>
#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#endif
int main(int argc, char **argv) {
    using namespace cab;
    try {
        require(argc == 2, "Provide an empty temporary test directory");
        Path base = absolute(argv[1]);
        mkdirs(base);
        Path source = join(base, "Factory/Tone.xml");
        mkdirs(parent(source));
        const Bytes original = "<future cabActive=\"true\"/>";
        {
            auto file = createExclusive(source);
            writeAll(file.handle, original);
            sync(file.handle);
        }
        std::atomic<bool> cancel{false};
        Source input{source, base, "Future", "Factory"};
        auto plan = preview({input}, "disable", cancel);
        require(plan.items.size() == 1 && plan.items[0].status == "ready", "Missing ready plan");
        Bytes concurrent = "<future cabActive=\"true\" name=\"external\"/>";
        {
            auto file = openWrite(source);
            writeAll(file.handle, concurrent);
            truncate(file.handle, concurrent.size());
            sync(file.handle);
        }
        auto results = apply(plan, "", cancel);
        require(results[0].status == "failed" && readSnapshot(source).data == concurrent,
                "Concurrent edit overwritten");
        auto backup = join(base, "stale.zip");
        results = apply(plan, backup, cancel);
        require(results[0].status == "failed" && !exists(backup), "Stale backup was accepted");
        plan = preview({input}, "disable", cancel);
        cancel = true;
        results = apply(plan, join(base, "cancelled.zip"), cancel);
        require(results[0].status == "cancelled" && !exists(join(base, "cancelled.zip")),
                "Cancelled backup left an archive");
        require(readSnapshot(source).data == concurrent, "Cancelled batch wrote source");
        cancel = false;
        plan = preview({input}, "disable", cancel);
        removeFile(source);
        cancel = true;
        results = apply(plan, "", cancel);
        require(results[0].status == "cancelled", "Cancelled batch probed an unavailable source");
        {
            auto restored=createExclusive(source);writeAll(restored.handle,concurrent);
        }
        cancel = false;
        // Opposite copy operations coexist without modifying the same original.
        for (auto operation : {"copy", "copy-enable"}) {
            auto copy = preview({input}, operation, cancel);
            const auto target = join(base, std::string(nd::copyFolder(enabling(operation))) +
                                               "/Factory/Tone.xml");
            require(copy.items[0].status == "ready" && copy.items[0].target == target,
                    "Wrong operation-specific copy folder");
            results = apply(copy, "", cancel);
            require(results[0].status == "changed" &&
                        readSnapshot(target).data == patch(concurrent, enabling(operation)) &&
                        readSnapshot(source).data == concurrent,
                    "Copy direction changed an original or produced wrong bytes");
            for (auto other : {"copy", "copy-enable"}) {
                auto recursive = preview({{target, base, "Future", "Factory"}}, other, cancel);
                require(recursive.items[0].status == "skipped" &&
                            recursive.items[0].reason == "Generated copies cannot be copied recursively",
                        "Generated copy accepted as a new copy source");
            }
        }
        require(readSnapshot(join(base,"Cab Disabled/Factory/Tone.xml")).data ==
                    patch(concurrent, false) &&
                    readSnapshot(join(base,"Cab Enabled/Factory/Tone.xml")).data == concurrent,
                "Opposite copy operations failed to coexist");
        for (auto operation : {"copy", "copy-enable"}) {
            auto copy = preview({input}, operation, cancel, join(base, operation));
            results = apply(copy, "", cancel);
            require(results[0].status == "changed", "Copy combination failed");
            require(inspect(readSnapshot(copy.items[0].target).data).enabled ==
                        (std::string(operation) == "copy-enable"),
                    "Wrong copy flag");
            require(readSnapshot(source).data == concurrent, "Copy modified original");
            auto repeated=preview({input},operation,cancel,join(base,operation));
            require(repeated.items[0].status=="already-correct",
                    "Identical existing copy misclassified");
            {
                auto target=openWrite(copy.items[0].target);
                writeAll(target.handle,"different");truncate(target.handle,9);
            }
            auto collision=preview({input},operation,cancel,join(base,operation));
            require(collision.items[0].status=="skipped"&&
                    readSnapshot(copy.items[0].target).data=="different",
                    "Existing copy collision modified or misclassified");
        }
#ifndef _WIN32
        Path alias=join(base,"Alias/Future/Factory/Tone.xml");
        mkdirs(parent(alias));
        require(!link(source.c_str(),alias.c_str()),"Cannot prepare hard-link alias test");
        auto aliased=preview({input},"copy",cancel,join(base,"Alias"));
        require(aliased.items[0].status=="skipped"&&
                aliased.items[0].reason=="Copy destination aliases source"&&
                readSnapshot(source).data==concurrent,"Hard-link copy alias accepted");
#endif
        // Parsed paths must handle Unicode and normalization without retaining safety results.
        Path unicode=join(base,u8"Unicodé/音/Tone.xml");
        mkdirs(parent(unicode));
        {
            auto file=createExclusive(unicode);
            writeAll(file.handle,original);
        }
        Source normalized{join(parent(unicode),"./Tone.xml"),base,"Future","Factory"};
        auto direct=preview({normalized},"disable",cancel);
        results=apply(direct,"",cancel);
        require(results[0].status=="changed"&&!inspect(readSnapshot(unicode).data).enabled,
                "Unicode normalized direct edit failed");
        direct=preview({normalized},"enable",cancel);
        results=apply(direct,join(base,"unicode-backup.zip"),cancel);
        require(results[0].status=="changed"&&readSnapshot(unicode).data==original,
                "Unicode verified-backup direct edit failed");
#ifndef _WIN32
        direct=preview({normalized},"disable",cancel);
        Path folder=parent(unicode),moved=folder+"-moved";
        require(!std::rename(folder.c_str(),moved.c_str())&&!symlink(moved.c_str(),folder.c_str()),
                "Cannot prepare ancestor substitution test");
        results=apply(direct,"",cancel);
        require(results[0].status=="failed"&&readSnapshot(join(moved,"Tone.xml")).data==original,
                "Replaced symlink ancestor escaped fresh validation");
#endif
        std::cout << "Immutable plan, concurrent edit rejection, stale/cancelled backup and both "
                     "copy directions passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
