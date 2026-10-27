#pragma once
#include <string>

namespace nd {
inline const char *copyFolder(bool enabled) { return enabled ? "Cab Enabled" : "Cab Disabled"; }
inline std::string copyDescription(bool enabled) {
    return std::string("Keep original presets, create and modify copies in separate \"") +
           copyFolder(enabled) + "\" folder.";
}
}
