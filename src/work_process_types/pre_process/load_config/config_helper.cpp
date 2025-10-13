

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
namespace py = pybind11;
using json = nlohmann::json;

// Function to match hardware pattern
json match_hardware_pattern(const json &hardware_info, const json &patterns) {
    for (auto &pattern : patterns["patterns"]) {
        auto cond = pattern["conditions"];
        bool match_cpu = cond["cpu"].get<std::string>().empty() ||
                         hardware_info["cpu"].get<std::string>().find(cond["cpu"].get<std::string>()) != std::string::npos;
        bool match_ram = cond["ram_gb"].get<int>() == 0 ||
                         cond["ram_gb"].get<int>() <= hardware_info["ram_gb"].get<int>();
        bool match_gpu = cond["gpu"].empty() ||
                         std::all_of(cond["gpu"].begin(), cond["gpu"].end(), [&](const std::string &g) {
                             return std::find(hardware_info["gpu"].begin(), hardware_info["gpu"].end(), g) != hardware_info["gpu"].end();
                         });
        if (match_cpu && match_ram && match_gpu) {
            return pattern["defaults"];
        }
    }
    return json::object(); // No match
}

// Function to create resource subfolders
void create_resource_folders(const std::vector<std::string> &paths) {
    for (const auto &p : paths) {
        fs::create_directories(p);
    }
}

PYBIND11_MODULE(config_helper, m) {
    m.def("match_hardware_pattern", &match_hardware_pattern);
    m.def("create_resource_folders", &create_resource_folders);
}
