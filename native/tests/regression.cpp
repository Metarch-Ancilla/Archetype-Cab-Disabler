#include "../backend/core.hpp"
#include "../../frontends/shared/wire.hpp"
#include <cstdio>
#include <type_traits>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace cab;
static_assert(!std::is_copy_constructible_v<File> && !std::is_copy_assignable_v<Authorization>);
static void rejects(const std::function<void()> &work) {
    bool rejected = false;
    try {
        work();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Invalid input unexpectedly accepted");
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Provide an empty temporary directory");
        auto startup = nd::indexingModel();
        require(startup.indexing() && !startup.next && !startup.back && startup.status.empty() &&
                    startup.controls.size() == 2 && startup.controls[0].kind == "plugins" &&
                    startup.controls[0].options.empty() &&
                    startup.controls[1].label == "Preset folders not detected?", "Invalid immediate startup model");
        startup.busy = false;
        require(!startup.indexing(), "Indexing notice survived discovery completion");
        startup.busy = true;startup.page = 4;
        require(!startup.indexing(), "Indexing notice shown during editing");
        nd::Control choice;choice.value="1";
        choice.options={{"1","Disable",true,-1,"",true,"Disable description"},
                        {"0","Enable",false,-1,"",true,"Enable description"}};
        require(choice.selectedOption().location=="Disable description","Reordered description inverted");
        choice.value="0";
        require(choice.selectedOption().location=="Enable description","Enable description inverted");
        std::reverse(choice.options.begin(),choice.options.end());
        require(choice.selectedOption().location=="Enable description","Description depends on position");
        nd::Control mode;mode.value="0";
        nd::Option direct;direct.value="0";direct.location="Direct description\nPermissions note";
        nd::Option copied;copied.value="1";copied.location="Copy description";
        mode.options={direct,copied};
        require(nd::editDescription(choice,mode)=="Enable description\nDirect description\nPermissions note",
                "Direct edit description line order changed");
        choice.value="1";mode.value="1";
        require(nd::editDescription(choice,mode)=="Disable description\n"+nd::copyDescription(false),
                "Copy description contains the wrong operation or permissions note");
        choice.value="0";
        require(nd::editDescription(choice,mode)=="Enable description\n"+nd::copyDescription(true),
                "Copy description did not follow the cab selection");
        const std::string unicode = "ASCII \xc2\xa3 \xe2\x82\xac \xf0\x9f\x98\x80";
        require(utf8(unicode) && unhex(hex(unicode)) == unicode, "Unicode codec mismatch");
        require(unhex("41E282AC") == "A\xe2\x82\xac", "Uppercase hex rejected");
        for (const auto &bad :
             {Bytes("\xc0\x80", 2), Bytes("\xed\xa0\x80", 3), Bytes("\xf4\x90\x80\x80", 4),
              Bytes("\xe2\x82", 2), Bytes("\x80", 1)}) {
            require(!utf8(bad), "Invalid UTF-8 accepted");
            rejects([&] { unhex(hex(bad)); });
        }
        rejects([] { unhex("0"); });
        rejects([] { unhex("gg"); });
        require(encode16("\xf0\x9f\x98\x80", false) == Bytes("\x3d\xd8\0\xde", 4),
                "UTF-16 surrogate mismatch");
        std::vector<std::string> lines;
        auto receive = [&](std::string line) { lines.push_back(std::move(line)); };
        nd::Lines framing(12);
        for (auto chunk : {"one\r", "\ntw", "o\n\nthree", "\n"}) framing.append(chunk, receive);
        require(lines == std::vector<std::string>{"one", "two", "", "three"},
                "Fragmented CRLF framing mismatch");
        nd::Lines bounded(3);
        bounded.append("abc", receive);
        bounded.append("\n", receive);
        rejects([&] { bounded.append("abcd\n", receive); });
        Library library{absolute(argv[1]), "Future", {}};
        for (int i = 2500; i-- > 0;) {
            auto name = "Tone " + std::to_string(i) + ".xml";
            library.sources.push_back(
                {join(library.path, "Artists/Artist/" + name), library.path, "Future", "Artist"});
        }
        for (auto relative : {"default.xml", "User/Own.xml", "Factory/Z.xml", "Factory/A.xml"})
            library.sources.push_back(
                {join(library.path, relative), library.path, "Future", "Unknown"});
        auto nodes = selectionTree({library}, {library.path});
        require(nodes.back().source && nodes.back().label == "default",
                "Loose preset did not sort last");
        require(nodes[1].label == "Artists", "Folder sort mismatch");
        size_t presets = 0;
        std::string previous;
        for (size_t i = 0; i < nodes.size(); ++i) {
            require(nodes[i].parent < (int)i, "Invalid tree parent");
            if (nodes[i].source) ++presets;
            if (nodes[i].source && nodes[i].label.rfind("Tone ", 0) == 0) {
                require(previous.empty() || !nd::lessFolded(nodes[i].label, previous),
                        "Preset order mismatch");
                previous = nodes[i].label;
            }
        }
        require(presets == library.sources.size(), "Preset lost in tree");
        mkdirs(library.path);
        Path path = join(library.path, "Already.xml");
        Bytes original =
            "<preset cabActive=\"false\"><!--" + Bytes(2 * 1024 * 1024, 'x') + "--></preset>";
        {
            auto file = createExclusive(path);
            writeAll(file.handle, original);
            sync(file.handle);
        }
        std::atomic<bool> cancel{false};
        Path scanRoot = join(library.path, "Discovery");
        for (const auto &relative : {"Plugin/Reset All Settings.xml",
                                     "Plugin/Artists/RESET ALL SETTINGS.XML",
                                     "Plugin/User/reset all settings.Xml",
                                     "Plugin/Default.xml", "Plugin/User/Reset Tone.xml",
                                     "Plugin/Cab Disabled/User/Disabled.xml",
                                     "Plugin/Cab Enabled/User/Enabled.xml",
                                     "Plugin/cAb EnAbLeD/Case.xml",
                                     "ResetOnly/Reset All Settings.xml"}) {
            auto path = join(scanRoot, relative);
            mkdirs(parent(path));
            auto output = createExclusive(path);
            writeAll(output.handle, "<preset cabActive=\"true\"/>");
        }
        for (bool boundary : {false, true}) {
            auto discovered = discover({{scanRoot, boundary}}, cancel);
            size_t indexed = 0;
            std::set<Path> selected;
            for (const auto &lib : discovered.libraries) {
                selected.insert(lib.path);
                for (const auto &source : lib.sources) {
                    require(lower(filename(source.path)) != "reset all settings.xml",
                            "Reset settings indexed");
                    ++indexed;
                }
            }
            require(indexed == 2 && discovered.issues.empty(), "Wrong preset discovery count");
            auto tree = selectionTree(discovered.libraries, selected);
            size_t selectable = 0;
            for (const auto &row : tree) if (row.source) ++selectable;
            require(selectable == 2, "Reset settings selectable");
        }
        for (auto folder : {"Cab Disabled", "Cab Enabled", "cAb EnAbLeD"}) {
            auto generated = discover({{join(scanRoot, std::string("Plugin/") + folder), true}}, cancel);
            require(generated.libraries.empty(), "Generated copy root accepted by Browse");
        }

#ifndef _WIN32
        Path metadataRoot=join(library.path,u8"Metadata-é");
        mkdirs(metadataRoot);
        Path first=join(metadataRoot,"First.xml");
        {
            auto output=createExclusive(first);writeAll(output.handle,"<x cabActive=\"true\"/>");
        }
        require(!link(first.c_str(),join(metadataRoot,"Alias.xml").c_str()),"Cannot create scan alias");
        auto listed=entries(metadataRoot);
        require(listed.size()==2,"Metadata scan lost an entry");
        for(const auto &entry:listed)
            require(entry.fileIndex&&identity(entry)==identity(entry.path),"Enumeration identity mismatch");
        auto metadataScan=discover({{metadataRoot,true}},cancel);
        require(metadataScan.issues.empty()&&metadataScan.libraries.size()==1&&
                    metadataScan.libraries[0].sources.size()==1,"Scan hard-link deduplication changed");
        require(!chmod(first.c_str(),0000),"Cannot prepare unreadable entry");
        require(identity(listed[0])==identity(listed[1]),"Indexing unnecessarily requires content access");
        require(!chmod(first.c_str(),0600),"Cannot restore fixture permissions");
        Path movedDirectory=metadataRoot+"-moved";
        require(!std::rename(metadataRoot.c_str(),movedDirectory.c_str())&&
                    !symlink(movedDirectory.c_str(),metadataRoot.c_str()),"Cannot substitute scan directory");
        rejects([&]{entries(metadataRoot);});
        rejects([&]{identity(listed[0]);});
#endif

        auto plan = preview({{path, library.path, "Future", "Factory"}}, "disable", cancel);
        require(plan.items.size() == 1 && plan.items[0].status == "already-correct",
                "No-op classification changed");
        require(plan.items[0].output.empty() && plan.items[0].output.capacity() < 1024,
                "No-op retained preset allocation");
        auto file = openRead(path);
        auto handle = file.handle;
        File moved(std::move(file));
        File assigned;
        assigned = std::move(moved);
        require(file.handle == Invalid && moved.handle == Invalid && assigned.handle == handle,
                "File move lost ownership");
        require(readHandle(assigned.handle) == original, "Moved handle unreadable");
        require(readSnapshot(path).data == original, "No-op modified original");
        std::puts(
            "Codec/framing, 2,504-preset tree, no-op buffer retention and file ownership passed");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
