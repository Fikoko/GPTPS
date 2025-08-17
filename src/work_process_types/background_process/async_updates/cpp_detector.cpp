#include <string>
#include <nlohmann/json.hpp>

class CPPDetector {
public:
    bool is_relevant(const std::string& msg_json, const std::string& worker_id, const std::string& group_id) {
        auto msg = nlohmann::json::parse(msg_json, nullptr, false);
        if (msg.is_discarded()) return false;

        // If worker_id is "all", message is relevant
        if (msg.contains("worker_id") && msg["worker_id"] == "all") return true;

        // Both worker_id and group_id must match
        bool worker_match = msg.contains("worker_id") && msg["worker_id"] == worker_id;
        bool group_match = msg.contains("group_id") && msg["group_id"] == group_id;

        return worker_match && group_match;
    }
};

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;

PYBIND11_MODULE(cpp_detector, m) {
    py::class_<CPPDetector>(m, "CPPDetector")
        .def(py::init<>())
        .def("is_relevant", &CPPDetector::is_relevant);
}
