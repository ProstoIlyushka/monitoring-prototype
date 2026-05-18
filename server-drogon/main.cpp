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
#include <iomanip>
#include <future>
#include <map>
#include <hiredis/hiredis.h>

using namespace drogon;

static std::shared_ptr<orm::DbClient> globalDbClient;
static redisContext* redisCtx = nullptr;

// ============================================================
// Функции для работы с Redis через hiredis
// ============================================================

bool connectRedis() {
    redisCtx = redisConnect("redis", 6379);
    if (redisCtx == nullptr || redisCtx->err) {
        std::cerr << "Redis connection error: " << (redisCtx ? redisCtx->errstr : "can't allocate context") << std::endl;
        return false;
    }
    std::cout << "✅ Redis connected successfully" << std::endl;
    return true;
}

std::string getCached(const std::string& key) {
    if (!redisCtx) return "";
    
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "GET %s", key.c_str());
    if (reply == nullptr) return "";
    
    std::string result;
    if (reply->type == REDIS_REPLY_STRING) {
        result = std::string(reply->str, reply->len);
    }
    freeReplyObject(reply);
    return result;
}

void setCached(const std::string& key, const std::string& value, int ttl_seconds = 30) {
    if (!redisCtx) return;
    
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "SETEX %s %d %s", key.c_str(), ttl_seconds, value.c_str());
    if (reply) freeReplyObject(reply);
}

void invalidateCache(const std::string& key) {
    if (!redisCtx) return;
    
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "DEL %s", key.c_str());
    if (reply) freeReplyObject(reply);
    std::cout << "Cache invalidated: " << key << std::endl;
}

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

// ============================================================
// КЛАСС КОНТРОЛЛЕРА
// ============================================================
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
        ADD_METHOD_TO(MetricsController::getForecast, "/api/forecast/{1}/{2}", Get);
    METHOD_LIST_END

    void getRoot(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody("<!DOCTYPE html><html><body><h1>Monitoring Server</h1><p>Go to <a href='/dashboard'>/dashboard</a></p></body></html>");
        resp->setContentTypeCode(CT_TEXT_HTML);
        callback(resp);
    }

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
            "INSERT INTO agents (name, api_key) VALUES ($1, $2) RETURNING id, name, api_key",
            [callback, name, api_key](const orm::Result& result) {
                Json::Value respJson;
                respJson["id"] = result[0]["id"].as<int>();
                respJson["name"] = result[0]["name"].as<std::string>();
                respJson["api_key"] = result[0]["api_key"].as<std::string>();
                auto resp = HttpResponse::newHttpJsonResponse(respJson);
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                std::cerr << "DB error in registerAgent: " << e.base().what() << std::endl;
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            },
            name, api_key
        );
    }
    
    void getAgents(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        
        std::string cacheKey = "agents:list";
        std::string cachedJson = getCached(cacheKey);
        if (!cachedJson.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody(cachedJson);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            callback(resp);
            return;
        }
        
        globalDbClient->execSqlAsync(
            "SELECT id, name, api_key, EXTRACT(EPOCH FROM last_seen) * 1000 as last_seen_ms, "
            "EXTRACT(EPOCH FROM created_at) * 1000 as created_at_ms FROM agents ORDER BY id",
            [cacheKey, callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                for (const auto& row : result) {
                    Json::Value item;
                    item["id"] = row["id"].as<int>();
                    item["name"] = row["name"].as<std::string>();
                    item["api_key"] = row["api_key"].as<std::string>();
                    item["last_seen"] = row["last_seen_ms"].as<double>();
                    item["created_at"] = row["created_at_ms"].as<double>();
                    json.append(item);
                }
                
                Json::FastWriter writer;
                setCached(cacheKey, writer.write(json), 10);
                
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            }
        );
    }

    void receiveMetrics(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback) {
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
        
        globalDbClient->execSqlAsync(
            "SELECT id FROM agents WHERE api_key = $1",
            [this, json, callback](const orm::Result& result) {
                if (result.empty()) {
                    auto resp = HttpResponse::newHttpResponse(k401Unauthorized, CT_TEXT_PLAIN);
                    resp->setBody("Invalid API Key");
                    callback(resp);
                    return;
                }
                int agent_id = result[0]["id"].as<int>();
                
                globalDbClient->execSqlAsync(
                    "UPDATE agents SET last_seen = NOW() WHERE id = $1",
                    [](const orm::Result&) {},
                    [](const orm::DrogonDbException& e) {
                        std::cerr << "Failed to update last_seen: " << e.base().what() << std::endl;
                    },
                    agent_id
                );
                
                std::vector<std::pair<std::string, double>> metrics;
                
                if ((*json).isMember("cpu")) metrics.push_back({"cpu", (*json)["cpu"].asDouble()});
                if ((*json).isMember("memory")) metrics.push_back({"memory", (*json)["memory"].asDouble()});
                if ((*json).isMember("ram")) metrics.push_back({"ram", (*json)["ram"].asDouble()});
                if ((*json).isMember("network_rx")) metrics.push_back({"network_rx", (*json)["network_rx"].asDouble()});
                if ((*json).isMember("network_tx")) metrics.push_back({"network_tx", (*json)["network_tx"].asDouble()});
                if ((*json).isMember("load_avg")) metrics.push_back({"load_avg", (*json)["load_avg"].asDouble()});
                if ((*json).isMember("swap")) metrics.push_back({"swap", (*json)["swap"].asDouble()});
                if ((*json).isMember("processes_running")) metrics.push_back({"processes_running", (*json)["processes_running"].asDouble()});
                
                if (metrics.empty()) {
                    auto resp = HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN);
                    resp->setBody("No valid metrics provided");
                    callback(resp);
                    return;
                }
                
                std::shared_ptr<int> insertedCount = std::make_shared<int>(0);
                int totalMetrics = metrics.size();
                
                for (const auto& [name, value] : metrics) {
                    globalDbClient->execSqlAsync(
                        "INSERT INTO metrics (agent_id, metric_name, value, timestamp) VALUES ($1, $2, $3, NOW())",
                        [this, name, value, insertedCount, totalMetrics](const orm::Result&) {
                            if (name == "cpu" || name == "memory" || name == "ram") {
                                this->checkThresholds((name == "cpu") ? "cpu" : "memory", value);
                            }
                            std::cout << "Inserted " << name << "=" << value << std::endl;
                            
                            (*insertedCount)++;
                            
                            if (*insertedCount == totalMetrics) {
                                invalidateCache("history:all");
                            }
                        },
                        [](const orm::DrogonDbException& e) {
                            std::cerr << "DB error inserting metric: " << e.base().what() << std::endl;
                        },
                        agent_id, name, value
                    );
                }
                
                auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
                resp->setBody("{\"status\": \"ok\"}");
                callback(resp);
                
                std::cout << "Received " << metrics.size() << " metrics for Agent ID=" << agent_id << std::endl;
            },
            [callback](const orm::DrogonDbException& e) {
                std::cerr << "Auth error: " << e.base().what() << std::endl;
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                callback(resp);
            },
            api_key
        );
    }
    
    void getLatest(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        globalDbClient->execSqlAsync(
            "SELECT metric_name, value, EXTRACT(EPOCH FROM timestamp) * 1000 as ts, a.name as agent_name "
            "FROM metrics m JOIN agents a ON m.agent_id = a.id "
            "ORDER BY m.timestamp DESC LIMIT 10",
            [callback](const orm::Result& result) {
                Json::Value json;
                if (!result.empty()) {
                    Json::Value metrics(Json::objectValue);
                    for (const auto& row : result) {
                        std::string name = row["metric_name"].as<std::string>();
                        double value = row["value"].as<double>();
                        if (!metrics.isMember(name)) {
                            metrics[name] = value;
                        }
                    }
                    json["agent_name"] = result[0]["agent_name"].as<std::string>();
                    json["timestamp"] = result[0]["ts"].as<double>();
                    json["metrics"] = metrics;
                } else {
                    json["message"] = "no data yet";
                }
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            }
        );
    }
    
    void getHistory(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback) {
        
        std::string cacheKey = "history:all";
        std::string cachedJson = getCached(cacheKey);
        if (!cachedJson.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody(cachedJson);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            callback(resp);
            return;
        }
        
        globalDbClient->execSqlAsync(
            "SELECT metric_name, value, EXTRACT(EPOCH FROM timestamp) * 1000 as ts, a.name as agent_name "
            "FROM metrics m JOIN agents a ON m.agent_id = a.id "
            "ORDER BY m.timestamp ASC LIMIT 500",
            [cacheKey, callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                std::map<double, Json::Value> grouped;
                
                for (const auto& row : result) {
                    double ts = row["ts"].as<double>();
                    std::string name = row["metric_name"].as<std::string>();
                    double value = row["value"].as<double>();
                    
                    if (!grouped.count(ts)) {
                        grouped[ts] = Json::Value(Json::objectValue);
                        grouped[ts]["timestamp"] = ts;
                        grouped[ts]["agent_name"] = row["agent_name"].as<std::string>();
                        grouped[ts]["metrics"] = Json::Value(Json::objectValue);
                    }
                    grouped[ts]["metrics"][name] = value;
                }
                
                for (const auto& pair : grouped) {
                    json.append(pair.second);
                }
                
                Json::FastWriter writer;
                setCached(cacheKey, writer.write(json), 30);
                
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            }
        );
    }
    
    void getAgentHistory(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         int agent_id) {
        globalDbClient->execSqlAsync(
            "SELECT metric_name, value, EXTRACT(EPOCH FROM timestamp) * 1000 as ts "
            "FROM metrics WHERE agent_id = $1 "
            "ORDER BY timestamp ASC LIMIT 500",
            [callback](const orm::Result& result) {
                Json::Value json(Json::objectValue);
                std::map<std::string, Json::Value> series;
                for (const auto& row : result) {
                    std::string name = row["metric_name"].as<std::string>();
                    double ts = row["ts"].as<double>();
                    double value = row["value"].as<double>();
                    
                    if (!series.count(name)) {
                        series[name] = Json::Value(Json::arrayValue);
                    }
                    Json::Value point;
                    point["timestamp"] = ts;
                    point["value"] = value;
                    series[name].append(point);
                }
                for (const auto& pair : series) {
                    json[pair.first] = pair.second;
                }
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            },
            agent_id
        );
    }

    
    void getForecast(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    int agent_id,
                    const std::string& metric_name) {
        globalDbClient->execSqlAsync(
            "SELECT value FROM metrics WHERE agent_id = $1 AND metric_name = $2 "
            "ORDER BY timestamp DESC LIMIT 72",
            [callback, metric_name, agent_id](const orm::Result& result) {
                std::vector<double> history;
                for (const auto& row : result) {
                    history.push_back(row["value"].as<double>());
                }
                
                if (history.empty()) {
                    auto resp = HttpResponse::newHttpResponse(k404NotFound, CT_TEXT_PLAIN);
                    resp->setBody("No data found for this agent/metric");
                    callback(resp);
                    return;
                }
                
                std::reverse(history.begin(), history.end());

                Json::Value request;
                for (double v : history) request["history"].append(v);
                request["horizon"] = 12;

                auto client = HttpClient::newHttpClient("http://arima:8081");
                auto httpReq = HttpRequest::newHttpJsonRequest(request);
                httpReq->setPath("/forecast");
                httpReq->setMethod(Post);

                client->sendRequest(httpReq, [callback](ReqResult result, const HttpResponsePtr& response) {
                    if (result == ReqResult::Ok && response && response->getStatusCode() == k200OK) {
                        callback(response);
                    } else {
                        auto resp = HttpResponse::newHttpResponse();
                        resp->setStatusCode(k500InternalServerError);
                        resp->setBody("ARIMA service unavailable");
                        resp->setContentTypeCode(CT_TEXT_PLAIN);
                        callback(resp);
                    }
                });
            },
            [callback](const orm::DrogonDbException& e) {
                std::cerr << "DB error in getForecast: " << e.base().what() << std::endl;
                auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN);
                resp->setBody("Database error");
                callback(resp);
            },
            agent_id, metric_name
        );
    }
    
    void getAlerts(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        globalDbClient->execSqlAsync(
            "SELECT id, metric_name, threshold_value, actual_value, message, "
            "EXTRACT(EPOCH FROM sent_at) * 1000 as sent_at_ms "
            "FROM alerts WHERE resolved_at IS NULL ORDER BY sent_at DESC LIMIT 50",
            [callback](const orm::Result& result) {
                Json::Value json(Json::arrayValue);
                for (const auto& row : result) {
                    Json::Value item;
                    item["id"] = row["id"].as<int>();
                    item["metric_name"] = row["metric_name"].as<std::string>();
                    item["threshold"] = row["threshold_value"].as<double>();
                    item["actual_value"] = row["actual_value"].as<double>();
                    item["message"] = row["message"].as<std::string>();
                    item["triggered_at"] = row["sent_at_ms"].as<double>();
                    json.append(item);
                }
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            }
        );
    }
    
    void resolveAlert(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      int alertId) {
        globalDbClient->execSqlAsync(
            "UPDATE alerts SET resolved_at = NOW() WHERE id = $1 AND resolved_at IS NULL",
            [callback](const orm::Result&) {
                auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_JSON);
                resp->setBody("{\"status\": \"resolved\"}");
                callback(resp);
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            },
            alertId
        );
    }
    
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
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(buffer.str());
        resp->setContentTypeCode(CT_TEXT_HTML);
        callback(resp);
    }

private:
    void checkThresholds(const std::string& metric_name, double value) {
        if (!globalDbClient) return;
        
        double threshold = 0.0;
        if (metric_name == "cpu") threshold = 80.0;
        else if (metric_name == "memory" || metric_name == "ram") threshold = 85.0;
        else return;
        
        if (value > threshold) {
            createAlert(metric_name, threshold, value);
        }
    }
    
    void createAlert(const std::string& metric_name, double threshold, double actual_value) {
        std::string message = metric_name + " usage is " + std::to_string(actual_value) + 
                            "% (threshold: " + std::to_string(threshold) + "%)";
        
        // 1. Запись в базу данных
        globalDbClient->execSqlAsync(
            "INSERT INTO alerts (metric_name, threshold_value, actual_value, message) VALUES ($1, $2, $3, $4)",
            [](const orm::Result&) {},
            [](const orm::DrogonDbException& e) {
                std::cerr << "Alert creation failed: " << e.base().what() << std::endl;
            },
            metric_name, threshold, actual_value, message
        );
        
        // 2. Вывод в консоль
        std::cout << "🚨 ALERT: " << message << std::endl;
        
        // 3. Отправка email-уведомления (соответствие ТЗ)
        std::string admin_email = "admin@example.com";  // можно вынести в переменную окружения
        std::string subject = "⚠️ Monitoring Alert: " + metric_name;
        sendEmailNotification(admin_email, subject, message);
    }

    void sendEmailNotification(const std::string& to, const std::string& subject, const std::string& body) {
        std::cout << "📧 EMAIL NOTIFICATION (would be sent via SMTP):" << std::endl;
        std::cout << "   To: " << to << std::endl;
        std::cout << "   Subject: " << subject << std::endl;
        std::cout << "   Body: " << body << std::endl;
        
        // Для реальной отправки раскомментировать и настроить SMTP:
        /*
        std::string cmd = "curl -s --url 'smtps://smtp.gmail.com:465' "
                        "--ssl-reqd "
                        "--mail-from 'monitoring@example.com' "
                        "--mail-rcpt '" + to + "' "
                        "--user 'monitoring@example.com:your_app_password' "
                        "-T <(echo -e 'From: monitoring@example.com\\n"
                        "To: " + to + "\\n"
                        "Subject: " + subject + "\\n\\n"
                        + body + "')";
        system(cmd.c_str());
        */
    }
};

// ============================================================
// MAIN
// ============================================================
int main() {
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
    
    // Подключение к Redis
    connectRedis();
    
    app().addListener("0.0.0.0", 8080);
    app().setThreadNum(4);
    
    std::cout << "========================================" << std::endl;
    std::cout << "Drogon Monitoring Server Started" << std::endl;
    std::cout << "HTTP: http://localhost:8080" << std::endl;
    std::cout << "Dashboard: http://localhost:8080/dashboard" << std::endl;
    std::cout << "========================================" << std::endl;
    
    app().run();
    
    // Очистка Redis при завершении
    if (redisCtx) {
        redisFree(redisCtx);
    }
    
    return 0;
}