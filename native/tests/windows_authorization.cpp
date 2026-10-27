#include "../backend/core.hpp"
#include <cstdio>
#ifdef _WIN32
int main() {
    using namespace cab;
    try {
        auto base = join(home(), "cab-authorization-probe-" + std::to_string(entropy()));
        mkdirs(base);
        auto selected = join(base, "authorization-selected.xml");
        auto excluded = join(base, "authorization-excluded.xml");
        const Bytes original = "<test cabActive=\"true\"/>";
        {
            auto f = createExclusive(selected);
            writeAll(f.handle, original);
            sync(f.handle);
        }
        {
            auto f = createExclusive(excluded);
            writeAll(f.handle, original);
            sync(f.handle);
        }
        auto before = readSnapshot(selected);
        Authorization batch;
        batch.authorize({selected});
        bool rejected = false;
        try {
            File unselected(batch.open(excluded));
        } catch (const Error &) {
            rejected = true;
        }
        require(rejected, "Unselected path was authorized");
        File authorized(batch.open(selected));
        require(signature(authorized.handle) == before.stamp, "Wrong authorized identity");
        require(readHandle(authorized.handle) == original, "Wrong authorized bytes");
        Bytes output = "<test cabActive=\"false\"/>";
        seek(authorized.handle, 0);
        writeAll(authorized.handle, output);
        truncate(authorized.handle, output.size());
        sync(authorized.handle);
        seek(authorized.handle, 0);
        require(readHandle(authorized.handle) == output, "Authorized readback failed");
        require(readSnapshot(excluded).data == original, "Unselected path changed");
        std::puts("{\"cpp_native_bridge\":true,\"main_exe_helper\":true,\"selected_handle_write_"
                  "readback\":true,\"unselected_rejected\":true,\"scope\":\"Wine; real Windows UAC "
                  "unverified\"}");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

#else
int main() { return 0; }
#endif
