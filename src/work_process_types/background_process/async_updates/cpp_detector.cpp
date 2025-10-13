// cpp_detector.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <set>
#include <fstream>
#include <sstream>
#include <iostream>

// Include JSON / YAML / TOML / XML / MsgPack
#include <nlohmann/json.hpp>      // JSON
#include <yaml-cpp/yaml.h>        // YAML
#include <toml++/toml.h>          // TOML++
#include <tinyxml2.h>             // XML
#include <msgpack.hpp>            // MsgPack

namespace py = pybind11;
using json = nlohmann::json;
using namespace tinyxml2;

// ---------------- Generic relevance check ----------------
bool relevance_check(const std::string& msg_worker, const std::string& msg_group,
                     const std::string& worker_id, const std::string& group_id) {
    if (msg_worker == "all" || msg_worker == worker_id) return true;
    if (!group_id.empty() && msg_group == group_id) return true;
    return false;
}

// ---------------- JSON parser ----------------
bool parse_json(const std::string& data, const std::string& worker_id, const std::string& group_id) {
    try {
        auto j = json::parse(data);
        std::string msg_worker = j.value("worker_id", "");
        std::string msg_group = j.value("group_id", "");
        return relevance_check(msg_worker, msg_group, worker_id, group_id);
    } catch(...) {
        return false;
    }
}

// ---------------- MsgPack parser ----------------
bool parse_msgpack(const std::string& data, const std::string& worker_id, const std::string& group_id) {
    try {
        msgpack::object_handle oh = msgpack::unpack(data.data(), data.size());
        msgpack::object obj = oh.get();
        auto map = obj.as<std::map<std::string, std::string>>();
        std::string msg_worker = map.count("worker_id") ? map.at("worker_id") : "";
        std::string msg_group = map.count("group_id") ? map.at("group_id") : "";
        return relevance_check(msg_worker, msg_group, worker_id, group_id);
    } catch(...) {
        return false;
    }
}

// ---------------- YAML parser ----------------
bool parse_yaml(const std::string& data, const std::string& worker_id, const std::string& group_id) {
    try {
        YAML::Node node = YAML::Load(data);
        std::string msg_worker = node["worker_id"] ? node["worker_id"].as<std::string>() : "";
        std::string msg_group = node["group_id"] ? node["group_id"].as<std::string>() : "";
        return relevance_check(msg_worker, msg_group, worker_id, group_id);
    } catch(...) {
        return false;
    }
}

// ---------------- TOML parser ----------------
bool parse_toml(const std::string& data, const std::string& worker_id, const std::string& group_id) {
    try {
        std::istringstream ss(data);
        auto tbl = toml::parse(ss);
        std::string msg_worker = tbl["worker_id"].value_or("");
        std::string msg_group = tbl["group_id"].value_or("");
        return relevance_check(msg_worker, msg_group, worker_id, group_id);
    } catch(...) {
        return false;
    }
}

// ---------------- XML parser ----------------
bool parse_xml(const std::string& data, const std::string& worker_id, const std::string& group_id) {
    try {
        XMLDocument doc;
        doc.Parse(data.c_str());
        XMLElement* root = doc.RootElement();
        std::string msg_worker = root->FirstChildElement("worker_id") ?
                                 root->FirstChildElement("worker_id")->GetText() : "";
        std::string msg_group = root->FirstChildElement("group_id") ?
                                root->FirstChildElement("group_id")->GetText() : "";
        return relevance_check(msg_worker, msg_group, worker_id, group_id);
    } catch(...) {
        return false;
    }
}

// ---------------- Main C++ Detector class ----------------
class CPPDetector {
public:
    CPPDetector() {}

    bool is_relevant(const std::string& message, const std::string& worker_id, const std::string& group_id,
                     const std::string& fmt="json") {
        if (fmt == "json") return parse_json(message, worker_id, group_id);
        else if (fmt == "msgpack") return parse_msgpack(message, worker_id, group_id);
        else if (fmt == "yaml") return parse_yaml(message, worker_id, group_id);
        else if (fmt == "toml") return parse_toml(message, worker_id, group_id);
        else if (fmt == "xml") return parse_xml(message, worker_id, group_id);
        else return false; // Avro/Protobuf handled separately in Python
    }
};

// ---------------- Extract method for Python ----------------
py::dict extract_message(const std::string& message, const std::string& fmt="json") {
    py::dict result;

    try {
        if (fmt == "json") {
            auto j = json::parse(message);
            for (auto& el : j.items())
                result[py::str(el.key())] = py::cast(el.value());
        }
        else if (fmt == "msgpack") {
            msgpack::object_handle oh = msgpack::unpack(message.data(), message.size());
            msgpack::object obj = oh.get();
            auto map = obj.as<std::map<std::string, std::string>>();
            for (auto& kv : map)
                result[py::str(kv.first)] = py::str(kv.second);
        }
        else if (fmt == "yaml") {
            YAML::Node node = YAML::Load(message);
            for (auto it = node.begin(); it != node.end(); ++it)
                result[py::str(it->first.as<std::string>())] = py::str(it->second.as<std::string>());
        }
        else if (fmt == "toml") {
            std::istringstream ss(message);
            auto tbl = toml::parse(ss);
            for (auto& kv : tbl) {
                std::string key_str = std::string(kv.first.str()); // Convert toml::v3::key to std::string
                std::string val_str = kv.second.value_or("");
                result[py::str(key_str)] = py::str(val_str);
            }
        }
        else if (fmt == "xml") {
            XMLDocument doc;
            doc.Parse(message.c_str());
            XMLElement* root = doc.RootElement();
            for (XMLElement* e = root->FirstChildElement(); e; e = e->NextSiblingElement()) {
                if (e->GetText()) result[py::str(e->Name())] = py::str(e->GetText());
            }
        }
    } catch (...) {
        // ignore errors, return empty dict
    }
    return result;
}

// ---------------- Pybind11 bindings ----------------
PYBIND11_MODULE(cpp_detector, m) {
    py::class_<CPPDetector>(m, "CPPDetector")
        .def(py::init<>())
        .def("is_relevant",
             &CPPDetector::is_relevant,
             py::arg("message"),
             py::arg("worker_id"),
             py::arg("group_id"),
             py::arg("fmt") = "json",
             "Check if message is relevant to this worker/group");

    m.def("extract",
          &extract_message,
          py::arg("message"),
          py::arg("fmt") = "json",
          "Return parsed dict from message");
}