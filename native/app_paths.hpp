#pragma once

#include <cstdlib>
#include <filesystem>

namespace knife_edge::paths {

// Finder and launchd start a bundle with the working directory at "/", so
// nothing may be resolved against the CWD.
inline std::filesystem::path game_data_path() {
#if defined(__APPLE__)
    const char* home_directory = std::getenv("HOME");
    if (home_directory == nullptr || home_directory[0] == '\0') {
        return {};
    }
    return std::filesystem::path(home_directory) /
           "Library" / "Application Support" / "KnifeEdgeRecompiled";
#else
    std::error_code current_path_error;
    const std::filesystem::path working_directory =
        std::filesystem::current_path(current_path_error);
    if (current_path_error) {
        return {};
    }
    return working_directory / ".knife-edge";
#endif
}

}
