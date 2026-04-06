#include <cstdlib>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <iostream>
#include <ctime>
#include <fstream>
#include <sstream>
#include <memory>
#include <random>
#include <sstream>
#include <iomanip>

using namespace drogon;

static std::shared_ptr<orm::DbClient> globalDbClient;

// Генератор API-ключей
std::string generateApiKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    for (int i = 0; i < 32; i++) {
        ss << std::hex << dis(gen);
    }
    return ss.str();
}

class MetricsController : public HttpController<MetricsController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(MetricsController::getRoot, "/", Get);
        ADD_METHOD_TO(MetricsController::registerAgent, "/api/agents/register", Post);
        ADD_METHOD_TO(MetricsController::getAgents, "/api/agents", Get);
        ADD_METHOD_TO(MetricsController::receiveMetrics, "/api/metrics", Post);
        ADD_METHOD_TO(MetricsController::getLatest, "/api/metrics/latest", Get);
        ADD_METHOD_TO(MetricsController::getHistory, "/api/metrics/history", Get);
        ADD_METHOD_TO(MetricsController::getAgentHistory, "/api/metrics/{1}/history", Get);
        ADD_METHOD_TO(MetricsController::getAlerts, "/api/alerts", Get);
        ADD_METHOD_TO(MetricsController::resolveAlert, "/api/alerts/{1}/resolve", Post);
        ADD_METHOD_TO(MetricsController::getDashboard, "/dashboard", Get);
    METHOD_LIST_END

    // GET / — главная страница
    void getRoot(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody("<!DOCTYPE html><html><body><h1>Monitoring Server</h1><p>Go to <a href='/dashboard'>/dashboard</a></p></body></html>");
        resp->setContentTypeCode(CT_TEXT_HTML);
        callback(resp);
    }

    // POST /api/agents/register — регистрация нового агента
    void registerAgent(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback) {
        auto json = req->getJsonObject();
        if (!json || !(*json).isMember("name")) {
            auto resp = HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN);
            resp->setBody("Missing agent name");
            callback(resp);
            return;
        }
        
        std::string name = (*json)["name"].asString();
        std::string api_key = generateApiKey();
        
        globalDbClient->execSqlAsync(
            "INSERT INTO agents (name, api_key) VALUES ($1, $2) RETURNING id",
            [callback, api_key](const orm::Result& result) {
                Json::Value respJson;
                respJson["id"] = result[0]["id"].as<int>();
                respJson["name"] = result[0]["name"].as<std::string>();
                respJson["api_key"] = api_key;
                auto resp = HttpResponse::newHttpJsonResponse(respJson);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            },
            name, api_key
        );
    }
    
    // GET /api/agents — список всех агентов
    void getAgents(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        globalDbClient->execSqlAsync(
            "SELECT id, name, api_key, last_seen, created_at FROM agents ORDER BY id",
            [callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                for (const auto& row : result) {
                    Json::Value item;
                    item["id"] = row["id"].as<int>();
                    item["name"] = row["name"].as<std::string>();
                    item["api_key"] = row["api_key"].as<std::string>();
                    item["last_seen"] = row["last_seen"].as<time_t>();
                    item["created_at"] = row["created_at"].as<time_t>();
                    json.append(item);
                }
                auto resp = HttpResponse::newHttpJsonResponse(json);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            }
        );
    }

    // POST /api/metrics — приём метрик (с проверкой API-ключа)
    void receiveMetrics(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback) {
        
        // Проверяем API-ключ
        auto api_key = req->getHeader("X-API-Key");
        if (api_key.empty()) {
            auto resp = HttpResponse::newHttpResponse(k401Unauthorized, CT_TEXT_PLAIN);
            resp->setBody("Missing API Key");
            callback(resp);
            return;
        }
        
        auto json = req->getJsonObject();
        if (!json) {
            auto resp = HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN);
            resp->setBody("Invalid JSON");
            callback(resp);
            return;
        }
        
        double cpu = (*json)["cpu"].asDouble();
        double memory = (*json)["memory"].asDouble();
        
        // Находим агента по API-ключу
        globalDbClient->execSqlAsync(
            "SELECT id FROM agents WHERE api_key = $1",
            [this, cpu, memory, callback](const orm::Result& result) {
                if (result.empty()) {
                    auto resp = HttpResponse::newHttpResponse(k401Unauthorized, CT_TEXT_PLAIN);
                    resp->setBody("Invalid API Key");
                    callback(resp);
                    return;
                }
                
                int agent_id = result[0]["id"].as<int>();
                
                // Обновляем last_seen
                globalDbClient->execSqlAsync(
                    "UPDATE agents SET last_seen = NOW() WHERE id = $1",
                    [](const orm::Result&) {},
                    [](const orm::DrogonDbException& e) {
                        std::cerr << "Failed to update last_seen: " << e.base().what() << std::endl;
                    },
                    agent_id
                );
                
                // Сохраняем метрику
                globalDbClient->execSqlAsync(
                    "INSERT INTO metrics (agent_id, cpu, memory) VALUES ($1, $2, $3)",
                    [this, cpu, memory, callback](const orm::Result&) {
                        checkThresholds(cpu, memory);
                        auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
                        resp->setBody("{\"status\": \"ok\"}");
                        callback(resp);
                    },
                    [callback](const orm::DrogonDbException& e) {
                        std::cerr << "DB error: " << e.base().what() << std::endl;
                        auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                        callback(resp);
                    },
                    agent_id, cpu, memory
                );
                
                std::cout << "Saved: Agent=" << agent_id << " CPU=" << cpu << "%, MEM=" << memory << "%" << std::endl;
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            },
            api_key
        );
    }
    
    // GET /api/metrics/latest — последняя метрика от любого агента
    void getLatest(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        
        globalDbClient->execSqlAsync(
            "SELECT m.cpu, m.memory, m.timestamp, a.name as agent_name "
            "FROM metrics m JOIN agents a ON m.agent_id = a.id "
            "ORDER BY m.timestamp DESC LIMIT 1",
            [callback](const orm::Result& result) {
                Json::Value json;
                if (!result.empty()) {
                    json["cpu"] = result[0]["cpu"].as<double>();
                    json["memory"] = result[0]["memory"].as<double>();
                    json["timestamp"] = result[0]["timestamp"].as<time_t>();
                    json["agent_name"] = result[0]["agent_name"].as<std::string>();
                } else {
                    json["cpu"] = 0;
                    json["memory"] = 0;
                    json["message"] = "no data yet";
                }
                auto resp = HttpResponse::newHttpJsonResponse(json);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            }
        );
    }
    
    // GET /api/metrics/history — история всех метрик (последние 100)
    void getHistory(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback) {
        
        globalDbClient->execSqlAsync(
            "SELECT m.cpu, m.memory, EXTRACT(EPOCH FROM m.timestamp) * 1000 as ts, a.name as agent_name "
            "FROM metrics m JOIN agents a ON m.agent_id = a.id "
            "ORDER BY m.timestamp ASC LIMIT 100",
            [callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                for (const auto& row : result) {
                    Json::Value item;
                    item["cpu"] = row["cpu"].as<double>();
                    item["memory"] = row["memory"].as<double>();
                    item["timestamp"] = row["ts"].as<double>();
                    item["agent_name"] = row["agent_name"].as<std::string>();
                    json.append(item);
                }
                auto resp = HttpResponse::newHttpJsonResponse(json);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            }
        );
    }
    
    // GET /api/metrics/{agent_id}/history — история конкретного агента
    void getAgentHistory(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         int agent_id) {
        
        globalDbClient->execSqlAsync(
            "SELECT cpu, memory, EXTRACT(EPOCH FROM timestamp) * 1000 as ts "
            "FROM metrics WHERE agent_id = $1 "
            "ORDER BY timestamp ASC LIMIT 100",
            [callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                for (const auto& row : result) {
                    Json::Value item;
                    item["cpu"] = row["cpu"].as<double>();
                    item["memory"] = row["memory"].as<double>();
                    item["timestamp"] = row["ts"].as<double>();
                    json.append(item);
                }
                auto resp = HttpResponse::newHttpJsonResponse(json);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            },
            agent_id
        );
    }
    
    // GET /api/alerts
    void getAlerts(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        
        globalDbClient->execSqlAsync(
            "SELECT id, type, message, triggered_at FROM alerts WHERE is_resolved = false ORDER BY triggered_at DESC",
            [callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                for (const auto& row : result) {
                    Json::Value item;
                    item["id"] = row["id"].as<int>();
                    item["type"] = row["type"].as<std::string>();
                    item["message"] = row["message"].as<std::string>();
                    item["triggered_at"] = row["triggered_at"].as<time_t>();
                    json.append(item);
                }
                auto resp = HttpResponse::newHttpJsonResponse(json);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            }
        );
    }
    
    // POST /api/alerts/{id}/resolve
    void resolveAlert(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      int alertId) {
        
        globalDbClient->execSqlAsync(
            "UPDATE alerts SET is_resolved = true, resolved_at = NOW() WHERE id = $1",
            [callback](const orm::Result&) {
                auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
                resp->setBody("{\"status\": \"resolved\"}");
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            },
            alertId
        );
    }
    
    // GET /dashboard — веб-интерфейс с выбором агента
    void getDashboard(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback) {
        
        std::ifstream file("/app/static/dashboard.html");
        if (!file.is_open()) {
            auto resp = HttpResponse::newHttpResponse(k404NotFound, CT_TEXT_PLAIN);
            resp->setBody("Dashboard not found");
            callback(resp);
            return;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string html = buffer.str();
        
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(html);
        resp->setContentTypeCode(CT_TEXT_HTML);
        callback(resp);
    }

private:
    void checkThresholds(double cpu, double memory) {
        if (!globalDbClient) return;
        
        if (cpu > 80.0) {
            createAlert("CPU_HIGH", "CPU usage is " + std::to_string(cpu) + "% (threshold: 80%)");
        }
        
        if (memory > 85.0) {
            createAlert("MEMORY_HIGH", "Memory usage is " + std::to_string(memory) + "% (threshold: 85%)");
        }
    }
    
    void createAlert(const std::string& type, const std::string& message) {
        globalDbClient->execSqlAsync(
            "SELECT id FROM alerts WHERE type = $1 AND is_resolved = false",
            [this, type, message](const orm::Result& result) {
                if (result.empty()) {
                    globalDbClient->execSqlAsync(
                        "INSERT INTO alerts (type, message) VALUES ($1, $2)",
                        [](const orm::Result&) {},
                        [](const orm::DrogonDbException& e) {
                            std::cerr << "Alert creation failed: " << e.base().what() << std::endl;
                        },
                        type, message
                    );
                    std::cout << "🚨 ALERT: " << message << std::endl;
                }
            },
            [](const orm::DrogonDbException& e) {
                std::cerr << "Alert check failed: " << e.base().what() << std::endl;
            },
            type
        );
    }
};

int main() {
    // Получаем параметры БД из окружения
    const char* db_host = std::getenv("DB_HOST") ? std::getenv("DB_HOST") : "postgres";
    const char* db_port = std::getenv("DB_PORT") ? std::getenv("DB_PORT") : "5432";
    const char* db_name = std::getenv("DB_NAME") ? std::getenv("DB_NAME") : "monitoring";
    const char* db_user = std::getenv("DB_USER") ? std::getenv("DB_USER") : "admin";
    const char* db_pass = std::getenv("DB_PASSWORD") ? std::getenv("DB_PASSWORD") : "secret";
    
    std::string conn_str = std::string("host=") + db_host + 
                           " port=" + db_port +
                           " dbname=" + db_name +
                           " user=" + db_user +
                           " password=" + db_pass;
    
    globalDbClient = orm::DbClient::newPgClient(conn_str, 2);
    
    // Загружаем конфигурацию (HTTP + HTTPS на разных портах)
    app().loadConfigFile("../config.json");
    
    std::cout << "========================================" << std::endl;
    std::cout << "Drogon Monitoring Server Started" << std::endl;
    std::cout << "HTTP (Dashboard): http://localhost:8080" << std::endl;
    std::cout << "HTTPS (Agents): https://localhost:8443" << std::endl;
    std::cout << "Dashboard: http://localhost:8080/dashboard" << std::endl;
    std::cout << "========================================" << std::endl;
    
    app().run();
    
    return 0;
}