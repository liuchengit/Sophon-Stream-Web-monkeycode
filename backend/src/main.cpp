#include <drogon/drogon.h>
#include <drogon/HttpAppFramework.h>
#include <iostream>
#include <filesystem>
#include <random>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <string>
#include <regex>
#include <sys/statvfs.h>
#include "db/database_manager.h"
#include "auth/auth_service.h"
#include "stream_engine.h"
#include "middleware/auth_middleware.h"
#include "utils/api_helper.h"
#include "utils/json_converter.h"
#include "db/repositories.h"
#include "models/entities.h"
#include "websocket/websocket_manager.h"
#include <nlohmann/json.hpp>
#include <drogon/drogon.h>

using namespace drogon;
using namespace sophon::web::middleware;
using namespace sophon::web::models;

struct CpuStats {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
    unsigned long long total() const {
        return user + nice + sys + idle + iowait + irq + softirq + steal;
    }
    unsigned long long active() const {
        return total() - idle - iowait;
    }
};

static CpuStats readCpuStats() {
    CpuStats s{0, 0, 0, 0, 0, 0, 0, 0};
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return s;
    std::string line;
    if (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string cpu;
        if (iss >> cpu) {
            iss >> s.user >> s.nice >> s.sys >> s.idle >> s.iowait >> s.irq >> s.softirq >> s.steal;
        }
    }
    return s;
}

static double readMemoryUsage() {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return 0.0;
    unsigned long long total = 0, available = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::istringstream iss(line.substr(9));
            iss >> total;
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream iss(line.substr(13));
            iss >> available;
        }
    }
    if (total == 0) return 0.0;
    return (double)(total - available) / total * 100.0;
}

static std::optional<double> readGpuUsage() {
    if (std::filesystem::exists("/proc/bmstream")) {
        std::ifstream f("/proc/bmstream");
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.find("gpu_usage") != std::string::npos) {
                    std::istringstream iss(line);
                    std::string key;
                    double val = 0.0;
                    if (iss >> key >> val) return val;
                }
            }
        }
    }
    return std::nullopt;
}

static double readDiskUsage() {
    struct statvfs stat;
    if (statvfs("/", &stat) == 0) {
        double total = (double)stat.f_blocks * stat.f_bsize;
        double free = (double)stat.f_bavail * stat.f_bsize;
        if (total > 0) return (total - free) / total * 100.0;
    }
    return 0.0;
}

static void handleWorkflow(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback,
                           const std::string& idStr) {
    using namespace sophon::web;
    using namespace sophon::web::db;

    auto method = req->method();

    if (method == Get) {
        if (idStr.empty()) {
            auto page = getQueryParamInt(req, "page", 1);
            auto limit = getQueryParamInt(req, "limit", 20);
            auto status = req->getParameter("status");

            auto items = WorkflowRepository::findAll(status, page, limit);
            auto total = WorkflowRepository::count(status);

            nlohmann::json jArr = nlohmann::json::array();
            for (const auto& item : items) {
                nlohmann::json j;
                j["id"] = item.id;
                j["name"] = item.name;
                j["description"] = item.description;
                j["status"] = item.status;
                j["created_at"] = item.created_at;
                j["updated_at"] = item.updated_at;
                jArr.push_back(j);
            }

            callback(successListResponse(toCppJson(jArr), total, page, limit));
        } else {
            try {
                int id = std::stoi(idStr);
                auto opt = WorkflowRepository::findById(id);
                if (!opt) {
                    callback(errorResponse(404, "Workflow not found", k404NotFound));
                    return;
                }

                auto nodes = WorkflowNodeRepository::findByWorkflowId(id);
                auto edges = WorkflowEdgeRepository::findByWorkflowId(id);

                nlohmann::json j;
                j["id"] = opt->id;
                j["name"] = opt->name;
                j["description"] = opt->description;
                j["status"] = opt->status;
                j["created_at"] = opt->created_at;
                j["updated_at"] = opt->updated_at;

                nlohmann::json jNodes = nlohmann::json::array();
                for (const auto& n : nodes) {
                    nlohmann::json node;
                    node["id"] = n.node_id;
                    node["type"] = n.node_type;
                    node["position"]["x"] = n.position_x;
                    node["position"]["y"] = n.position_y;
                    node["data"]["label"] = n.label;
                    node["data"]["config"] = nlohmann::json::parse(n.config_json, nullptr, false);
                    jNodes.push_back(node);
                }

                nlohmann::json jEdges = nlohmann::json::array();
                for (const auto& e : edges) {
                    nlohmann::json edge;
                    edge["id"] = e.edge_id;
                    edge["source"] = e.source_node;
                    edge["target"] = e.target_node;
                    edge["sourceHandle"] = e.source_handle;
                    edge["targetHandle"] = e.target_handle;
                    jEdges.push_back(edge);
                }

                j["nodes"] = jNodes;
                j["edges"] = jEdges;

                callback(successResponse(toCppJson(j)));
            } catch (...) {
                callback(errorResponse(400, "Invalid ID", k400BadRequest));
            }
        }
    } else if (method == Post && idStr.empty()) {
        auto jsonPtr = req->getJsonObject();
        if (!jsonPtr) {
            callback(errorResponse(400, "Invalid JSON body"));
            return;
        }

        Workflow wf;
        wf.name = (*jsonPtr)["name"].asString();
        wf.description = jsonPtr->isMember("description") ? (*jsonPtr)["description"].asString() : "";
        wf.status = jsonPtr->isMember("status") ? (*jsonPtr)["status"].asString() : "draft";

        int newId = WorkflowRepository::create(wf);

        nlohmann::json j;
        j["id"] = newId;
        j["name"] = wf.name;
        j["description"] = wf.description;
        j["status"] = wf.status;

        callback(successResponse(toCppJson(j)));
    } else if (method == Put && !idStr.empty()) {
        try {
            int id = std::stoi(idStr);
            auto jsonPtr = req->getJsonObject();
            if (!jsonPtr) {
                callback(errorResponse(400, "Invalid JSON body"));
                return;
            }

            auto opt = WorkflowRepository::findById(id);
            if (!opt) {
                callback(errorResponse(404, "Workflow not found", k404NotFound));
                return;
            }

            Workflow wf = *opt;
            if (jsonPtr->isMember("name")) wf.name = (*jsonPtr)["name"].asString();
            if (jsonPtr->isMember("description")) wf.description = (*jsonPtr)["description"].asString();
            if (jsonPtr->isMember("status")) wf.status = (*jsonPtr)["status"].asString();

            WorkflowRepository::update(id, wf);

            if (jsonPtr->isMember("nodes")) {
                WorkflowNodeRepository::removeByWorkflowId(id);
                for (const auto& jNode : (*jsonPtr)["nodes"]) {
                    WorkflowNode node;
                    node.workflow_id = id;
                    node.node_id = jNode["id"].asString();
                    node.node_type = jNode.isMember("type") ? jNode["type"].asString() : "default";
                    node.position_x = jNode["position"]["x"].asDouble();
                    node.position_y = jNode["position"]["y"].asDouble();
                    if (jNode["data"].isMember("label"))
                        node.label = jNode["data"]["label"].asString();
                    if (jNode["data"].isMember("config"))
                        node.config_json = jNode["data"]["config"].toStyledString();
                    else
                        node.config_json = "{}";
                    WorkflowNodeRepository::create(node);
                }
            }

            if (jsonPtr->isMember("edges")) {
                WorkflowEdgeRepository::removeByWorkflowId(id);
                for (const auto& jEdge : (*jsonPtr)["edges"]) {
                    WorkflowEdge edge;
                    edge.workflow_id = id;
                    edge.edge_id = jEdge["id"].asString();
                    edge.source_node = jEdge["source"].asString();
                    edge.target_node = jEdge["target"].asString();
                    edge.source_handle = jEdge.isMember("sourceHandle") ? jEdge["sourceHandle"].asString() : "default";
                    edge.target_handle = jEdge.isMember("targetHandle") ? jEdge["targetHandle"].asString() : "default";
                    WorkflowEdgeRepository::create(edge);
                }
            }

            nlohmann::json j;
            j["id"] = id;
            j["name"] = wf.name;
            j["status"] = wf.status;
            callback(successResponse(toCppJson(j)));
        } catch (...) {
            callback(errorResponse(400, "Invalid ID", k400BadRequest));
        }
    } else if (method == Delete && !idStr.empty()) {
        try {
            int id = std::stoi(idStr);
            WorkflowNodeRepository::removeByWorkflowId(id);
            WorkflowEdgeRepository::removeByWorkflowId(id);
            bool removed = WorkflowRepository::remove(id);
            if (!removed) {
                callback(errorResponse(404, "Workflow not found", k404NotFound));
                return;
            }
            callback(successResponse(Json::objectValue));
        } catch (...) {
            callback(errorResponse(400, "Invalid ID", k400BadRequest));
        }
    } else {
        callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
    }
}

static void handleWorkflowExecution(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& callback,
                                     const std::string& idStr,
                                     const std::string& action) {
    using namespace sophon::web;
    using namespace sophon::web::db;
    using namespace sophon::web::websocket;

    try {
        int wfId = std::stoi(idStr);
        auto wfOpt = WorkflowRepository::findById(wfId);
        if (!wfOpt) {
            callback(errorResponse(404, "Workflow not found", k404NotFound));
            return;
        }

        if (action == "start") {
            auto running = WorkflowExecutionRepository::findLatestRunning(wfId);
            if (running) {
                callback(errorResponse(400, "Workflow already running", k400BadRequest));
                return;
            }

            int execId = WorkflowExecutionRepository::create(wfId);
            WorkflowRepository::updateStatus(wfId, "running");

            auto nodes = WorkflowExecutionNodeRepository::findByExecutionId(execId);
            nlohmann::json nodeStates = nlohmann::json::array();
            for (const auto& n : nodes) {
                nlohmann::json ns;
                ns["nodeId"] = n.node_id;
                ns["nodeType"] = n.node_type;
                ns["label"] = n.label;
                ns["status"] = n.status;
                nodeStates.push_back(ns);
            }

            nlohmann::json j;
            j["type"] = "execution";
            j["action"] = "start";
            j["executionId"] = execId;
            j["workflowId"] = wfId;
            j["nodes"] = nodeStates;
            WebSocketManager::instance().broadcast(j);

            nlohmann::json resp;
            resp["executionId"] = execId;
            resp["workflowId"] = wfId;
            resp["status"] = "running";
            resp["nodeCount"] = (int)nodes.size();
            callback(successResponse(toCppJson(resp)));

        } else if (action == "stop") {
            auto running = WorkflowExecutionRepository::findLatestRunning(wfId);
            if (!running) {
                callback(errorResponse(400, "No running execution found", k400BadRequest));
                return;
            }

            auto execNodes = WorkflowExecutionNodeRepository::findByExecutionId(running->id);
            for (const auto& n : execNodes) {
                if (n.status == "running" || n.status == "pending") {
                    WorkflowExecutionNodeRepository::updateStatus(running->id, n.node_id, "cancelled");
                }
            }

            WorkflowExecutionRepository::finish(running->id, "Stopped by user");
            WorkflowRepository::updateStatus(wfId, "stopped");

            nlohmann::json j;
            j["type"] = "execution";
            j["action"] = "stop";
            j["executionId"] = running->id;
            j["workflowId"] = wfId;
            WebSocketManager::instance().broadcast(j);

            callback(successResponse(Json::objectValue));

        } else if (action == "status") {
            auto running = WorkflowExecutionRepository::findLatestRunning(wfId);
            if (!running) {
                auto executions = WorkflowExecutionRepository::findByWorkflowId(wfId, 1);
                if (executions.empty()) {
                    nlohmann::json j;
                    j["status"] = "idle";
                    callback(successResponse(toCppJson(j)));
                    return;
                }
                running = executions[0];
            }

            auto execNodes = WorkflowExecutionNodeRepository::findByExecutionId(running->id);
            nlohmann::json nodeStates = nlohmann::json::array();
            for (const auto& n : execNodes) {
                nlohmann::json ns;
                ns["nodeId"] = n.node_id;
                ns["nodeType"] = n.node_type;
                ns["label"] = n.label;
                ns["status"] = n.status;
                if (!n.started_at.empty()) ns["startedAt"] = n.started_at;
                if (!n.finished_at.empty()) ns["finishedAt"] = n.finished_at;
                if (!n.error_message.empty()) ns["errorMessage"] = n.error_message;
                nodeStates.push_back(ns);
            }

            nlohmann::json j;
            j["executionId"] = running->id;
            j["workflowId"] = running->workflow_id;
            j["status"] = running->status;
            j["startedAt"] = running->started_at;
            if (!running->finished_at.empty()) j["finishedAt"] = running->finished_at;
            if (!running->error_message.empty()) j["errorMessage"] = running->error_message;
            j["nodes"] = nodeStates;
            callback(successResponse(toCppJson(j)));

        } else if (action == "history") {
            auto executions = WorkflowExecutionRepository::findByWorkflowId(wfId, 20);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : executions) {
                nlohmann::json j;
                j["id"] = e.id;
                j["status"] = e.status;
                j["startedAt"] = e.started_at;
                if (!e.finished_at.empty()) j["finishedAt"] = e.finished_at;
                if (!e.error_message.empty()) j["errorMessage"] = e.error_message;
                arr.push_back(j);
            }
            callback(successListResponse(toCppJson(arr), (int)executions.size(), 1, 20));

        } else {
            callback(errorResponse(400, "Unknown action", k400BadRequest));
        }
    } catch (...) {
        callback(errorResponse(400, "Invalid ID", k400BadRequest));
    }
}

int main() {
    std::cout << "Starting Sophon-Stream Web Management System..." << std::endl;

    // Ensure required directories exist
    std::filesystem::create_directories("logs");
    std::filesystem::create_directories("uploads");
    std::filesystem::create_directories("data");
    std::filesystem::create_directories("engine/configs");

    // Initialize database
    if (!sophon::web::db::DatabaseManager::instance().initialize("data/sophon-web.db")) {
        std::cerr << "Failed to initialize database" << std::endl;
        return 1;
    }

    // Initialize authentication
    if (!sophon::web::auth::AuthService::instance().initialize()) {
        std::cerr << "Failed to initialize auth service" << std::endl;
        return 1;
    }

    // Initialize sophon-stream engine
    if (!sophon::stream::StreamEngine::instance().initialize("engine/configs/default.json")) {
        std::cerr << "Warning: Failed to initialize sophon-stream engine (running without hardware)" << std::endl;
    }

    // Register middlewares
    drogon::app().registerMiddleware(std::make_shared<AuthMiddleware>());
    drogon::app().registerMiddleware(std::make_shared<RBACMiddleware>());

    // Register auth handlers
    drogon::app().registerHandler("/api/v1/auth/login",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            auto jsonPtr = req->getJsonObject();
            if (!jsonPtr) {
                nlohmann::json error = {{"code", 400}, {"message", "Invalid request body"}};
                auto resp = drogon::HttpResponse::newHttpJsonResponse(toCppJson(error));
                resp->setStatusCode(drogon::k400BadRequest);
                callback(resp);
                return;
            }

            std::string username = (*jsonPtr)["username"].asString();
            std::string password = (*jsonPtr)["password"].asString();

            auto token = sophon::web::auth::AuthService::instance().login(username, password);
            if (!token) {
                nlohmann::json error = {{"code", 401}, {"message", "Invalid username or password"}};
                auto resp = drogon::HttpResponse::newHttpJsonResponse(toCppJson(error));
                resp->setStatusCode(drogon::k401Unauthorized);
                callback(resp);
                return;
            }

            nlohmann::json response = {
                {"code", 0},
                {"message", "success"},
                {"data", {
                    {"token", *token},
                    {"user", {
                        {"id", 1},
                        {"username", username},
                        {"role", "admin"}
                    }}
                }}
            };

            auto resp = drogon::HttpResponse::newHttpJsonResponse(toCppJson(response));
            callback(resp);
        },
        {Post});

    drogon::app().registerHandler("/api/v1/auth/logout",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            nlohmann::json response = {{"code", 0}, {"message", "success"}};
            auto resp = drogon::HttpResponse::newHttpJsonResponse(toCppJson(response));
            callback(resp);
        },
        {Post});

    // Register workflow handlers
    drogon::app().registerHandler("/api/v1/workflows",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            handleWorkflow(req, std::move(callback), "");
        },
        {Get, Post});

    drogon::app().registerHandler("/api/v1/workflows/{id}",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            handleWorkflow(req, std::move(callback), id);
        },
        {Get, Put, Delete, Options});

    drogon::app().registerHandler("/api/v1/workflows/{id}/start",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            handleWorkflowExecution(req, std::move(callback), id, "start");
        },
        {Post});

    drogon::app().registerHandler("/api/v1/workflows/{id}/stop",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            handleWorkflowExecution(req, std::move(callback), id, "stop");
        },
        {Post});

    drogon::app().registerHandler("/api/v1/workflows/{id}/status",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            handleWorkflowExecution(req, std::move(callback), id, "status");
        },
        {Get});

    // Register device handlers
    drogon::app().registerHandler("/api/v1/devices",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            if (req->method() == Get) {
                using namespace sophon::web::db;
                auto status = getQueryParam(req, "status");
                auto type = getQueryParam(req, "type");
                auto page = getQueryParamInt(req, "page", 1);
                auto limit = getQueryParamInt(req, "limit", 20);

                auto devices = DeviceRepository::findAll(status, type, page, limit);
                auto total = DeviceRepository::count(status, type);

                nlohmann::json jArr = nlohmann::json::array();
                for (const auto& d : devices) {
                    nlohmann::json j;
                    j["id"] = d.id;
                    j["name"] = d.name;
                    j["type"] = d.type;
                    j["ip_address"] = d.ip_address;
                    j["port"] = d.port;
                    j["status"] = d.status;
                    j["model"] = d.model;
                    j["firmware_version"] = d.firmware_version;
                    j["created_at"] = d.created_at;
                    j["updated_at"] = d.updated_at;
                    jArr.push_back(j);
                }
                callback(successListResponse(toCppJson(jArr), total, page, limit));
            } else if (req->method() == Post) {
                using namespace sophon::web::db;
                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }
                Device device;
                device.name = (*jsonPtr)["name"].asString();
                device.type = (*jsonPtr)["type"].asString();
                device.ip_address = (*jsonPtr)["ip_address"].asString();
                device.port = (*jsonPtr)["port"].asInt();
                device.status = "offline";
                if (jsonPtr->isMember("model")) device.model = (*jsonPtr)["model"].asString();
                if (jsonPtr->isMember("firmware_version")) device.firmware_version = (*jsonPtr)["firmware_version"].asString();
                int newId = DeviceRepository::create(device);
                nlohmann::json j;
                j["id"] = newId;
                j["name"] = device.name;
                j["type"] = device.type;
                j["ip_address"] = device.ip_address;
                j["port"] = device.port;
                j["status"] = device.status;
                j["model"] = device.model;
                j["firmware_version"] = device.firmware_version;
                auto resp = successResponse(toCppJson(j));
                resp->setStatusCode(k201Created);
                callback(resp);
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Post});

    drogon::app().registerHandler("/api/v1/devices/{id}",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);
            auto method = req->method();

            if (method == Get) {
                auto opt = DeviceRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Device not found", k404NotFound));
                    return;
                }
                nlohmann::json j;
                j["id"] = opt->id;
                j["name"] = opt->name;
                j["type"] = opt->type;
                j["ip_address"] = opt->ip_address;
                j["port"] = opt->port;
                j["status"] = opt->status;
                j["model"] = opt->model;
                j["firmware_version"] = opt->firmware_version;
                j["created_at"] = opt->created_at;
                j["updated_at"] = opt->updated_at;
                callback(successResponse(toCppJson(j)));
            } else if (method == Put) {
                auto opt = DeviceRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Device not found", k404NotFound));
                    return;
                }
                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }
                Device device = *opt;
                if (jsonPtr->isMember("name")) device.name = (*jsonPtr)["name"].asString();
                if (jsonPtr->isMember("type")) device.type = (*jsonPtr)["type"].asString();
                if (jsonPtr->isMember("ip_address")) device.ip_address = (*jsonPtr)["ip_address"].asString();
                if (jsonPtr->isMember("port")) device.port = (*jsonPtr)["port"].asInt();
                if (jsonPtr->isMember("status")) device.status = (*jsonPtr)["status"].asString();
                if (jsonPtr->isMember("model")) device.model = (*jsonPtr)["model"].asString();
                if (jsonPtr->isMember("firmware_version")) device.firmware_version = (*jsonPtr)["firmware_version"].asString();
                DeviceRepository::update(idInt, device);
                nlohmann::json j;
                j["id"] = device.id;
                j["name"] = device.name;
                callback(successResponse(toCppJson(j)));
            } else if (method == Delete) {
                DeviceRepository::remove(idInt);
                callback(successResponse(Json::objectValue));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Put, Delete});

    // Register task handlers
    drogon::app().registerHandler("/api/v1/tasks",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            if (req->method() == Get) {
                auto status = getQueryParam(req, "status");
                auto page = getQueryParamInt(req, "page", 1);
                auto limit = getQueryParamInt(req, "limit", 20);

                auto tasks = TaskRepository::findAll(status, page, limit);
                auto total = TaskRepository::count(status);

                nlohmann::json jArr = nlohmann::json::array();
                for (const auto& task : tasks) {
                    nlohmann::json j;
                    j["id"] = task.id;
                    j["name"] = task.name;
                    j["description"] = task.description;
                    j["device_id"] = task.device_id;
                    j["graph_config"] = task.graph_config;
                    j["status"] = task.status;
                    j["schedule_cron"] = task.schedule_cron;
                    j["created_at"] = task.created_at;
                    j["updated_at"] = task.updated_at;
                    jArr.push_back(j);
                }
                callback(successListResponse(toCppJson(jArr), total, page, limit));
            } else if (req->method() == Post) {
                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }
                Task task;
                task.name = (*jsonPtr)["name"].asString();
                if (jsonPtr->isMember("description")) task.description = (*jsonPtr)["description"].asString();
                if (jsonPtr->isMember("device_id")) task.device_id = (*jsonPtr)["device_id"].asInt();
                if (jsonPtr->isMember("graph_config")) task.graph_config = (*jsonPtr)["graph_config"].asString();
                task.status = "stopped";
                if (jsonPtr->isMember("schedule_cron")) task.schedule_cron = (*jsonPtr)["schedule_cron"].asString();

                int newId = TaskRepository::create(task);

                nlohmann::json j;
                j["id"] = newId;
                j["name"] = task.name;
                j["description"] = task.description;
                j["device_id"] = task.device_id;
                j["graph_config"] = task.graph_config;
                j["status"] = task.status;
                j["schedule_cron"] = task.schedule_cron;
                j["created_at"] = task.created_at;
                j["updated_at"] = task.updated_at;

                auto resp = successResponse(toCppJson(j));
                resp->setStatusCode(k201Created);
                callback(resp);
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Post});

    drogon::app().registerHandler("/api/v1/tasks/{id}",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);
            auto method = req->method();

            if (method == Get) {
                auto opt = TaskRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Task not found", k404NotFound));
                    return;
                }
                nlohmann::json j;
                j["id"] = opt->id;
                j["name"] = opt->name;
                j["description"] = opt->description;
                j["device_id"] = opt->device_id;
                j["graph_config"] = opt->graph_config;
                j["status"] = opt->status;
                j["schedule_cron"] = opt->schedule_cron;
                j["created_at"] = opt->created_at;
                j["updated_at"] = opt->updated_at;
                callback(successResponse(toCppJson(j)));
            } else if (method == Put) {
                auto opt = TaskRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Task not found", k404NotFound));
                    return;
                }

                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }

                Task task = *opt;
                if (jsonPtr->isMember("name")) task.name = (*jsonPtr)["name"].asString();
                if (jsonPtr->isMember("description")) task.description = (*jsonPtr)["description"].asString();
                if (jsonPtr->isMember("device_id")) task.device_id = (*jsonPtr)["device_id"].asInt();
                if (jsonPtr->isMember("graph_config")) task.graph_config = (*jsonPtr)["graph_config"].asString();
                if (jsonPtr->isMember("status")) task.status = (*jsonPtr)["status"].asString();
                if (jsonPtr->isMember("schedule_cron")) task.schedule_cron = (*jsonPtr)["schedule_cron"].asString();

                bool updated = TaskRepository::update(idInt, task);
                if (!updated) {
                    callback(errorResponse(500, "Failed to update task", k500InternalServerError));
                    return;
                }

                nlohmann::json j;
                j["id"] = task.id;
                j["name"] = task.name;
                j["description"] = task.description;
                j["device_id"] = task.device_id;
                j["graph_config"] = task.graph_config;
                j["status"] = task.status;
                j["schedule_cron"] = task.schedule_cron;
                callback(successResponse(toCppJson(j)));

            } else if (method == Delete) {
                bool removed = TaskRepository::remove(idInt);
                if (!removed) {
                    callback(errorResponse(404, "Task not found", k404NotFound));
                    return;
                }
                callback(successResponse(Json::objectValue));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Put, Delete});

    drogon::app().registerHandler("/api/v1/tasks/{id}/start",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = TaskRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Task not found", k404NotFound));
                return;
            }

            bool updated = TaskRepository::updateStatus(idInt, "running");
            if (!updated) {
                callback(errorResponse(500, "Failed to start task", k500InternalServerError));
                return;
            }

            nlohmann::json j;
            j["id"] = idInt;
            j["status"] = "running";
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/tasks/{id}/stop",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = TaskRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Task not found", k404NotFound));
                return;
            }

            bool updated = TaskRepository::updateStatus(idInt, "stopped");
            if (!updated) {
                callback(errorResponse(500, "Failed to stop task", k500InternalServerError));
                return;
            }

            nlohmann::json j;
            j["id"] = idInt;
            j["status"] = "stopped";
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/tasks/{id}/pause",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = TaskRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Task not found", k404NotFound));
                return;
            }

            bool updated = TaskRepository::updateStatus(idInt, "paused");
            if (!updated) {
                callback(errorResponse(500, "Failed to pause task", k500InternalServerError));
                return;
            }

            nlohmann::json j;
            j["id"] = idInt;
            j["status"] = "paused";
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/tasks/{id}/resume",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = TaskRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Task not found", k404NotFound));
                return;
            }

            bool updated = TaskRepository::updateStatus(idInt, "running");
            if (!updated) {
                callback(errorResponse(500, "Failed to resume task", k500InternalServerError));
                return;
            }

            nlohmann::json j;
            j["id"] = idInt;
            j["status"] = "running";
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/tasks/{id}/config",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = TaskRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Task not found", k404NotFound));
                return;
            }

            auto jsonPtr = req->getJsonObject();
            if (!jsonPtr) {
                callback(errorResponse(400, "Invalid JSON body"));
                return;
            }

            Task task = *opt;
            if (jsonPtr->isMember("graph_config")) task.graph_config = (*jsonPtr)["graph_config"].asString();

            bool updated = TaskRepository::update(idInt, task);
            if (!updated) {
                callback(errorResponse(500, "Failed to update config", k500InternalServerError));
                return;
            }

            nlohmann::json j;
            j["id"] = task.id;
            j["graph_config"] = task.graph_config;
            callback(successResponse(toCppJson(j)));
        },
        {Put});

    drogon::app().registerHandler("/api/v1/workflows/{id}/history",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            handleWorkflowExecution(req, std::move(callback), id, "history");
        },
        {Get});

    // Register algorithm handlers
    drogon::app().registerHandler("/api/v1/algorithms",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            if (req->method() == Get) {
                auto page = getQueryParamInt(req, "page", 1);
                auto limit = getQueryParamInt(req, "limit", 20);

                auto items = AlgorithmRepository::findAll(page, limit);
                auto total = AlgorithmRepository::count();

                nlohmann::json jArr = nlohmann::json::array();
                for (const auto& item : items) {
                    nlohmann::json j;
                    j["id"] = item.id;
                    j["name"] = item.name;
                    j["version"] = item.version;
                    j["type"] = item.type;
                    j["model_path"] = item.model_path;
                    j["config_schema"] = item.config_schema;
                    j["plugin_path"] = item.plugin_path;
                    j["status"] = item.status;
                    j["created_at"] = item.created_at;
                    jArr.push_back(j);
                }

                callback(successListResponse(toCppJson(jArr), total, page, limit));
            } else if (req->method() == Post) {
                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }

                Algorithm algo;
                algo.name = (*jsonPtr)["name"].asString();
                algo.version = (*jsonPtr)["version"].asString();
                algo.type = (*jsonPtr)["type"].asString();
                algo.model_path = jsonPtr->isMember("model_path") ? (*jsonPtr)["model_path"].asString() : "";
                algo.config_schema = jsonPtr->isMember("config_schema") ? (*jsonPtr)["config_schema"].asString() : "";
                algo.plugin_path = jsonPtr->isMember("plugin_path") ? (*jsonPtr)["plugin_path"].asString() : "";
                algo.status = "inactive";

                int newId = AlgorithmRepository::create(algo);

                nlohmann::json j;
                j["id"] = newId;
                j["name"] = algo.name;
                j["version"] = algo.version;
                j["type"] = algo.type;
                j["status"] = algo.status;

                callback(successResponse(toCppJson(j)));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Post});

    drogon::app().registerHandler("/api/v1/algorithms/{id}",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);
            auto method = req->method();

            if (method == Get) {
                auto opt = AlgorithmRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Algorithm not found", k404NotFound));
                    return;
                }

                nlohmann::json j;
                j["id"] = opt->id;
                j["name"] = opt->name;
                j["version"] = opt->version;
                j["type"] = opt->type;
                j["model_path"] = opt->model_path;
                j["config_schema"] = opt->config_schema;
                j["plugin_path"] = opt->plugin_path;
                j["status"] = opt->status;
                j["created_at"] = opt->created_at;

                callback(successResponse(toCppJson(j)));
            } else if (method == Put) {
                auto opt = AlgorithmRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Algorithm not found", k404NotFound));
                    return;
                }

                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }

                Algorithm algo = *opt;
                if (jsonPtr->isMember("name")) algo.name = (*jsonPtr)["name"].asString();
                if (jsonPtr->isMember("version")) algo.version = (*jsonPtr)["version"].asString();
                if (jsonPtr->isMember("type")) algo.type = (*jsonPtr)["type"].asString();
                if (jsonPtr->isMember("model_path")) algo.model_path = (*jsonPtr)["model_path"].asString();
                if (jsonPtr->isMember("config_schema")) algo.config_schema = (*jsonPtr)["config_schema"].asString();
                if (jsonPtr->isMember("plugin_path")) algo.plugin_path = (*jsonPtr)["plugin_path"].asString();
                if (jsonPtr->isMember("status")) algo.status = (*jsonPtr)["status"].asString();

                bool updated = AlgorithmRepository::update(idInt, algo);
                if (!updated) {
                    callback(errorResponse(500, "Failed to update", k500InternalServerError));
                    return;
                }

                nlohmann::json j;
                j["id"] = idInt;
                j["name"] = algo.name;
                callback(successResponse(toCppJson(j)));

            } else if (method == Delete) {
                bool removed = AlgorithmRepository::remove(idInt);
                if (!removed) {
                    callback(errorResponse(404, "Algorithm not found", k404NotFound));
                    return;
                }

                callback(successResponse(Json::objectValue));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Put, Delete});

    // Register alarm handlers
    drogon::app().registerHandler("/api/v1/alarms/rules",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            if (req->method() == Get) {
                auto rules = AlarmRuleRepository::findAll(false);
                nlohmann::json jArr = nlohmann::json::array();
                for (const auto& r : rules) {
                    nlohmann::json j;
                    j["id"] = r.id;
                    j["name"] = r.name;
                    j["condition_expr"] = r.condition_expr;
                    j["debounce_seconds"] = r.debounce_seconds;
                    j["notification_channels"] = r.notification_channels;
                    j["enabled"] = r.enabled;
                    j["created_at"] = r.created_at;
                    j["gb_alarm_type"] = r.gb_alarm_type;
                    j["alarm_method"] = r.alarm_method;
                    j["subscribe_status"] = r.subscribe_status;
                    j["subscribe_expires"] = r.subscribe_expires;
                    j["device_id"] = r.device_id;
                    j["channel_id"] = r.channel_id;
                    j["alarm_priority"] = r.alarm_priority;
                    j["alarm_description"] = r.alarm_description;
                    jArr.push_back(j);
                }
                callback(successListResponse(toCppJson(jArr), static_cast<int>(rules.size()), 1, static_cast<int>(rules.size())));
            } else if (req->method() == Post) {
                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }

                AlarmRule rule;
                rule.name = (*jsonPtr)["name"].asString();
                rule.condition_expr = (*jsonPtr)["condition_expr"].asString();
                rule.debounce_seconds = jsonPtr->isMember("debounce_seconds") ? (*jsonPtr)["debounce_seconds"].asInt() : 0;
                rule.notification_channels = jsonPtr->isMember("notification_channels") ? (*jsonPtr)["notification_channels"].asString() : "webhook";
                rule.enabled = jsonPtr->isMember("enabled") ? (*jsonPtr)["enabled"].asBool() : true;
                rule.gb_alarm_type = jsonPtr->isMember("gb_alarm_type") ? (*jsonPtr)["gb_alarm_type"].asString() : "";
                rule.alarm_method = jsonPtr->isMember("alarm_method") ? (*jsonPtr)["alarm_method"].asInt() : 5;
                rule.device_id = jsonPtr->isMember("device_id") ? (*jsonPtr)["device_id"].asInt() : 0;
                rule.channel_id = jsonPtr->isMember("channel_id") ? (*jsonPtr)["channel_id"].asInt() : 0;
                rule.alarm_priority = jsonPtr->isMember("alarm_priority") ? (*jsonPtr)["alarm_priority"].asString() : "medium";
                rule.alarm_description = jsonPtr->isMember("alarm_description") ? (*jsonPtr)["alarm_description"].asString() : "";

                int newId = AlarmRuleRepository::create(rule);
                nlohmann::json j;
                j["id"] = newId;
                j["name"] = rule.name;
                j["enabled"] = rule.enabled;
                callback(successResponse(toCppJson(j)));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Post});

    drogon::app().registerHandler("/api/v1/alarms/rules/{id}",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);
            auto method = req->method();

            if (method == Get) {
                auto opt = AlarmRuleRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Alarm rule not found", k404NotFound));
                    return;
                }

                nlohmann::json j;
                j["id"] = opt->id;
                j["name"] = opt->name;
                j["condition_expr"] = opt->condition_expr;
                j["debounce_seconds"] = opt->debounce_seconds;
                j["notification_channels"] = opt->notification_channels;
                j["enabled"] = opt->enabled;
                j["created_at"] = opt->created_at;
                j["gb_alarm_type"] = opt->gb_alarm_type;
                j["alarm_method"] = opt->alarm_method;
                j["subscribe_status"] = opt->subscribe_status;
                j["subscribe_expires"] = opt->subscribe_expires;
                j["device_id"] = opt->device_id;
                j["channel_id"] = opt->channel_id;
                j["alarm_priority"] = opt->alarm_priority;
                j["alarm_description"] = opt->alarm_description;

                callback(successResponse(toCppJson(j)));
            } else if (method == Put) {
                auto opt = AlarmRuleRepository::findById(idInt);
                if (!opt.has_value()) {
                    callback(errorResponse(404, "Alarm rule not found", k404NotFound));
                    return;
                }

                auto jsonPtr = req->getJsonObject();
                if (!jsonPtr) {
                    callback(errorResponse(400, "Invalid JSON body"));
                    return;
                }

                AlarmRule rule = *opt;
                if (jsonPtr->isMember("name")) rule.name = (*jsonPtr)["name"].asString();
                if (jsonPtr->isMember("condition_expr")) rule.condition_expr = (*jsonPtr)["condition_expr"].asString();
                if (jsonPtr->isMember("debounce_seconds")) rule.debounce_seconds = (*jsonPtr)["debounce_seconds"].asInt();
                if (jsonPtr->isMember("notification_channels")) rule.notification_channels = (*jsonPtr)["notification_channels"].asString();
                if (jsonPtr->isMember("enabled")) rule.enabled = (*jsonPtr)["enabled"].asBool();
                if (jsonPtr->isMember("gb_alarm_type")) rule.gb_alarm_type = (*jsonPtr)["gb_alarm_type"].asString();
                if (jsonPtr->isMember("alarm_method")) rule.alarm_method = (*jsonPtr)["alarm_method"].asInt();
                if (jsonPtr->isMember("device_id")) rule.device_id = (*jsonPtr)["device_id"].asInt();
                if (jsonPtr->isMember("channel_id")) rule.channel_id = (*jsonPtr)["channel_id"].asInt();
                if (jsonPtr->isMember("alarm_priority")) rule.alarm_priority = (*jsonPtr)["alarm_priority"].asString();
                if (jsonPtr->isMember("alarm_description")) rule.alarm_description = (*jsonPtr)["alarm_description"].asString();

                AlarmRuleRepository::update(idInt, rule);
                nlohmann::json j;
                j["id"] = idInt;
                j["name"] = rule.name;
                callback(successResponse(toCppJson(j)));

            } else if (method == Delete) {
                bool removed = AlarmRuleRepository::remove(idInt);
                if (!removed) {
                    callback(errorResponse(404, "Alarm rule not found", k404NotFound));
                    return;
                }
                callback(successResponse(Json::objectValue));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get, Put, Delete});

    drogon::app().registerHandler("/api/v1/alarms/rules/{id}/toggle",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = AlarmRuleRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Alarm rule not found", k404NotFound));
                return;
            }

            AlarmRuleRepository::toggle(idInt, !opt->enabled);
            nlohmann::json j;
            j["id"] = idInt;
            j["enabled"] = !opt->enabled;
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/alarms/rules/{id}/subscribe",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = AlarmRuleRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Alarm rule not found", k404NotFound));
                return;
            }

            auto now = std::chrono::system_clock::now();
            auto expires = now + std::chrono::hours(24);
            std::time_t expiresTime = std::chrono::system_clock::to_time_t(expires);
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&expiresTime), "%Y-%m-%d %H:%M:%S");
            std::string expiresStr = ss.str();

            AlarmRuleRepository::updateSubscribeStatus(idInt, "subscribed", expiresStr);
            nlohmann::json j;
            j["id"] = idInt;
            j["subscribe_status"] = "subscribed";
            j["subscribe_expires"] = expiresStr;
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/alarms/rules/{id}/unsubscribe",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = AlarmRuleRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Alarm rule not found", k404NotFound));
                return;
            }

            AlarmRuleRepository::updateSubscribeStatus(idInt, "unsubscribed", "");
            nlohmann::json j;
            j["id"] = idInt;
            j["subscribe_status"] = "unsubscribed";
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    drogon::app().registerHandler("/api/v1/alarms/events",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            if (req->method() == Get) {
                int page = getQueryParamInt(req, "page", 1);
                int limit = getQueryParamInt(req, "limit", 20);
                std::string status = getQueryParam(req, "status");

                auto events = AlarmEventRepository::findAll(page, limit);
                int total = AlarmEventRepository::count();

                nlohmann::json jArr = nlohmann::json::array();
                for (const auto& e : events) {
                    nlohmann::json j;
                    j["id"] = e.id;
                    j["rule_id"] = e.rule_id;
                    j["task_id"] = e.task_id;
                    j["evidence_path"] = e.evidence_path;
                    j["context"] = e.context;
                    j["triggered_at"] = e.triggered_at;
                    j["gb_alarm_code"] = e.gb_alarm_code;
                    j["alarm_priority"] = e.alarm_priority;
                    j["alarm_type"] = e.alarm_type;
                    j["device_id"] = e.device_id;
                    j["channel_id"] = e.channel_id;
                    j["sip_transaction_id"] = e.sip_transaction_id;
                    j["alarm_description"] = e.alarm_description;
                    j["handled_status"] = e.handled_status;
                    j["handled_at"] = e.handled_at;
                    j["handled_by"] = e.handled_by;
                    j["handle_result"] = e.handle_result;
                    j["alarm_method"] = e.alarm_method;
                    jArr.push_back(j);
                }
                callback(successListResponse(toCppJson(jArr), total, page, limit));
            } else {
                callback(errorResponse(405, "Method not allowed", k405MethodNotAllowed));
            }
        },
        {Get});

    drogon::app().registerHandler("/api/v1/alarms/events/{id}/handle",
        [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
            using namespace sophon::web::db;
            using namespace sophon::web::models;
            int idInt = std::stoi(id);

            auto opt = AlarmEventRepository::findById(idInt);
            if (!opt.has_value()) {
                callback(errorResponse(404, "Alarm event not found", k404NotFound));
                return;
            }

            auto jsonPtr = req->getJsonObject();
            if (!jsonPtr) {
                callback(errorResponse(400, "Invalid JSON body"));
                return;
            }

            int userId = 1;
            std::string result = jsonPtr->isMember("result") ? (*jsonPtr)["result"].asString() : "已处理";
            bool updated = AlarmEventRepository::updateHandled(idInt, userId, result);
            if (!updated) {
                callback(errorResponse(500, "Failed to handle alarm", k500InternalServerError));
                return;
            }

            nlohmann::json j;
            j["id"] = idInt;
            j["handled_status"] = "handled";
            callback(successResponse(toCppJson(j)));
        },
        {Post});

    // Load configuration
    drogon::app().loadConfigFile("config.json");

    // Start periodic metrics broadcast via WebSocket
    auto lastCpu = readCpuStats();
    auto metricsTimer = drogon::app().getLoop()->runEvery(3.0, [&lastCpu]() {
        using namespace sophon::web::websocket;
        if (WebSocketManager::instance().getConnectionCount() == 0) return;

        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        std::string ts;
        {
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&timeT), "%Y-%m-%dT%H:%M:%SZ");
            ts = ss.str();
        }

        auto currCpu = readCpuStats();
        auto totalDiff = currCpu.total() - lastCpu.total();
        double cpuUsage = 0.0;
        if (totalDiff > 0) {
            cpuUsage = (double)(currCpu.active() - lastCpu.active()) / totalDiff * 100.0;
        }
        lastCpu = currCpu;

        WebSocketManager::instance().broadcastMetrics("cpu_usage", cpuUsage, ts);
        WebSocketManager::instance().broadcastMetrics("memory_usage", readMemoryUsage(), ts);
        WebSocketManager::instance().broadcastMetrics("disk_usage", readDiskUsage(), ts);

        auto gpuOpt = readGpuUsage();
        if (gpuOpt.has_value()) {
            WebSocketManager::instance().broadcastMetrics("gpu_usage", gpuOpt.value(), ts);
        }
    });

    // Simulate execution progress for running workflows
    auto execTimer = drogon::app().getLoop()->runEvery(2.0, []() {
        using namespace sophon::web::db;
        using namespace sophon::web::websocket;

        std::vector<int> wfIds;
        {
            auto wfs = WorkflowRepository::findAll("running");
            for (const auto& wf : wfs) wfIds.push_back(wf.id);
        }

        for (int wfId : wfIds) {
            auto execOpt = WorkflowExecutionRepository::findLatestRunning(wfId);
            if (!execOpt) continue;

            int execId = execOpt->id;
            auto nodes = WorkflowExecutionNodeRepository::findByExecutionId(execId);
            if (nodes.empty()) continue;

            bool allDone = true;
            for (const auto& n : nodes) {
                if (n.status != "completed" && n.status != "failed" && n.status != "cancelled") {
                    allDone = false;
                    break;
                }
            }
            if (allDone) {
                WorkflowExecutionRepository::finish(execId);
                WorkflowRepository::updateStatus(wfId, "stopped");
                nlohmann::json j;
                j["type"] = "execution";
                j["action"] = "complete";
                j["executionId"] = execId;
                j["workflowId"] = wfId;
                WebSocketManager::instance().broadcast(j);
                continue;
            }

            bool hasRunning = false;
            for (const auto& n : nodes) {
                if (n.status == "running") { hasRunning = true; break; }
            }

            if (hasRunning) {
                for (const auto& n : nodes) {
                    if (n.status == "running") {
                        if (rand() % 10 < 4) {
                            WorkflowExecutionNodeRepository::updateStatus(execId, n.node_id, "completed");
                            nlohmann::json j;
                            j["type"] = "execution";
                            j["action"] = "nodeUpdate";
                            j["executionId"] = execId;
                            j["workflowId"] = wfId;
                            j["nodeId"] = n.node_id;
                            j["status"] = "completed";
                            WebSocketManager::instance().broadcast(j);
                        }
                    }
                }
            }

            if (!hasRunning) {
                for (const auto& n : nodes) {
                    if (n.status == "pending") {
                        WorkflowExecutionNodeRepository::updateStatus(execId, n.node_id, "running");
                        nlohmann::json j;
                        j["type"] = "execution";
                        j["action"] = "nodeUpdate";
                        j["executionId"] = execId;
                        j["workflowId"] = wfId;
                        j["nodeId"] = n.node_id;
                        j["status"] = "running";
                        WebSocketManager::instance().broadcast(j);
                        break;
                    }
                }
            }
        }
    });

    std::cout << "Sophon-Stream Web Management System started successfully." << std::endl;
    std::cout << "API: http://localhost:8080" << std::endl;
    std::cout << "WebSocket: ws://localhost:8080/api/v1/ws/notifications?token=<JWT_TOKEN>" << std::endl;
    std::cout << "Default admin: admin / admin123" << std::endl;

    // Start the application
    drogon::app().run();

    // Cleanup
    sophon::stream::StreamEngine::instance().shutdown();
    sophon::web::db::DatabaseManager::instance().close();

    return 0;
}
