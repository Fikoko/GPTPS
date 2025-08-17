
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp> // for JSON handling

namespace py = pybind11;
using json = nlohmann::json;

class AnalyticsLoggerCPP {
public:
    AnalyticsLoggerCPP(const std::string& file_path, int max_records)
        : file_path(file_path), max_records(max_records) {
        std::ifstream f(file_path);
        if (!f.good()) {
            std::ofstream out(file_path);
            out << "{}";
        }
    }

    void log(const std::string& task_name,
             const std::string& start_time,
             const std::string& end_time,
             double duration) 
    {
        std::lock_guard<std::mutex> guard(mtx);
        
        json data;
        {
            std::ifstream f(file_path);
            if (f.good()) f >> data;
        }

        json rec = {
            {"start_time", start_time},
            {"end_time", end_time},
            {"duration_seconds", duration}
        };

        if (!data.contains(task_name)) {
            data[task_name] = json::array();
        }
        data[task_name].insert(data[task_name].begin(), rec);

        if (data[task_name].size() > max_records) {
            data[task_name].erase(data[task_name].begin() + max_records, data[task_name].end());
        }

        {
            std::ofstream f(file_path);
            f << data.dump(2);
        }
    }

private:
    std::string file_path;
    int max_records;
    std::mutex mtx;
};

PYBIND11_MODULE(fast_logger, m) {
    py::class_<AnalyticsLoggerCPP>(m, "AnalyticsLoggerCPP")
        .def(py::init<const std::string&, int>())
        .def("log", &AnalyticsLoggerCPP::log);
}
