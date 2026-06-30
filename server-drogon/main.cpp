#include <cstdlib>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/Utilities.h>
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
#include <thread>
#include <chrono>
#include <hiredis/hiredis.h>

using namespace drogon;

static std::shared_ptr<orm::DbClient> globalDbClient;
static redisContext* redisCtx = nullptr;

// ============================================================
// СТРУКТУРА ДЛЯ НАСТРОЕК СЕРВЕРА
// ============================================================
struct ServerConfig {
    // SMTP
    std::string smtp_host = "smtp.yandex.com";
    std::string smtp_port = "465";
    std::string smtp_from = "monitoring@example.com";
    std::string smtp_password;
    std::string smtp_to = "admin@example.com";
};

static ServerConfig serverConfig;

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

bool ensureRedisConnected() {
    std::cout << "🔍 [DEBUG] ensureRedisConnected called" << std::endl;
    std::cout.flush();
    
    // Проверяем, есть ли контекст
    if (redisCtx == nullptr) {
        std::cout << "🔍 [DEBUG] redisCtx is NULL, connecting..." << std::endl;
        std::cout.flush();
        return connectRedis();
    }
    
    std::cout << "🔍 [DEBUG] redisCtx exists, checking err: " << redisCtx->err << std::endl;
    std::cout.flush();
    
    // Проверяем, есть ли ошибка в контексте
    if (redisCtx->err != 0) {
        std::cerr << "⚠️ Redis connection error: " << redisCtx->errstr << ", reconnecting..." << std::endl;
        redisFree(redisCtx);
        redisCtx = nullptr;
        return connectRedis();
    }
    
    std::cout << "🔍 [DEBUG] redisCtx seems valid, trying PING..." << std::endl;
    std::cout.flush();
    
    // Проверяем соединение через PING
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "PING");
    
    if (reply == nullptr) {
        std::cerr << "⚠️ Redis PING returned NULL, reconnecting..." << std::endl;
        redisFree(redisCtx);
        redisCtx = nullptr;
        return connectRedis();
    }
    
    std::cout << "🔍 [DEBUG] PING reply->type = " << reply->type << std::endl;
    std::cout.flush();
    
    if (reply->type == REDIS_REPLY_ERROR) {
        std::cerr << "⚠️ Redis PING error: " << reply->str << ", reconnecting..." << std::endl;
        freeReplyObject(reply);
        redisFree(redisCtx);
        redisCtx = nullptr;
        return connectRedis();
    }
    
    std::cout << "🔍 [DEBUG] PING successful (reply->type = " << reply->type << ")" << std::endl;
    std::cout.flush();
    
    freeReplyObject(reply);
    return true;
}

std::string getCached(const std::string& key) {
    std::cout << "🔍 [DEBUG] getCached: key = " << key << std::endl;
    std::cout.flush();
    
    if (!ensureRedisConnected()) {
        std::cerr << "❌ getCached: Redis not connected" << std::endl;
        return "";
    }
    
    std::cout << "🔍 [DEBUG] getCached: redisCtx is valid, calling redisCommand..." << std::endl;
    std::cout.flush();
    
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "GET %s", key.c_str());
    
    if (reply == nullptr) {
        std::cerr << "❌ getCached: redisCommand returned NULL" << std::endl;
        return "";
    }
    
    std::cout << "🔍 [DEBUG] getCached: reply->type = " << reply->type << std::endl;
    std::cout.flush();
    
    if (reply->type == REDIS_REPLY_STRING) {
        std::string result = std::string(reply->str, reply->len);
        std::cout << "🔍 [DEBUG] getCached: value = '" << result << "'" << std::endl;
        freeReplyObject(reply);
        return result;
    } else if (reply->type == REDIS_REPLY_NIL) {
        std::cout << "🔍 [DEBUG] getCached: value is NIL (empty)" << std::endl;
        freeReplyObject(reply);
        return "";
    } else if (reply->type == REDIS_REPLY_ERROR) {
        std::cout << "🚨 [DEBUG] REDIS ERROR: " << reply->str << std::endl;  // <-- добавить эту строку
        std::cout << "🔍 [DEBUG] getCached: value is ERROR" << std::endl;
        freeReplyObject(reply);
        return "";
    } else if (reply->type == REDIS_REPLY_INTEGER) {
    std::cout << "🔍 [DEBUG] getCached: value is INTEGER: " << reply->integer << std::endl;
    freeReplyObject(reply);
    return "";
    } else {
        std::cout << "🔍 [DEBUG] getCached: unexpected type = " << reply->type << std::endl;
        std::cout << "🚨 [DEBUG] REDIS ERROR: " << reply->str << std::endl;  // <-- добавить эту строку
        freeReplyObject(reply);
        return "";
    }
}

void setCached(const std::string& key, const std::string& value, int ttl_seconds = 120) {
    if (!ensureRedisConnected()) {
        std::cerr << "❌ setCached: Redis not connected" << std::endl;
        return;
    }
    
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "SETEX %s %d %s", 
                                                    key.c_str(), ttl_seconds, value.c_str());
    
    if (reply == nullptr) {
        std::cerr << "⚠️ setCached: redisCommand returned nullptr, reconnecting..." << std::endl;
        if (redisCtx) {
            redisFree(redisCtx);
            redisCtx = nullptr;
        }
        if (connectRedis()) {
            reply = (redisReply*)redisCommand(redisCtx, "SETEX %s %d %s", 
                                              key.c_str(), ttl_seconds, value.c_str());
        }
    }
    
    if (reply) freeReplyObject(reply);
}

void invalidateCache(const std::string& key) {
    if (!ensureRedisConnected()) {
        std::cerr << "❌ invalidateCache: Redis not connected" << std::endl;
        return;
    }
    
    // Если ключ заканчивается на ":", удаляем все ключи с этим префиксом
    if (key.back() == ':') {
        redisReply* reply = (redisReply*)redisCommand(redisCtx, "KEYS %s*", key.c_str());
        if (reply && reply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < reply->elements; ++i) {
                std::string k = reply->element[i]->str;
                redisReply* delReply = (redisReply*)redisCommand(redisCtx, "DEL %s", k.c_str());
                if (delReply) freeReplyObject(delReply);
                std::cout << "Cache invalidated: " << k << std::endl;
            }
        }
        if (reply) freeReplyObject(reply);
        return;
    }
    
    redisReply* reply = (redisReply*)redisCommand(redisCtx, "DEL %s", key.c_str());
    if (reply) {
        freeReplyObject(reply);
        std::cout << "Cache invalidated: " << key << std::endl;
    }
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
// ЗАГРУЗКА КОНФИГА СЕРВЕРА
// ============================================================
ServerConfig loadServerConfig() {
    ServerConfig cfg;
    
    // Путь к конфигу из переменной окружения
    const char* config_path = std::getenv("CONFIG_FILE");
    std::string path = config_path ? config_path : "/etc/monitoring-server/config";
    
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "⚠️ Server config not found: " << path << std::endl;
        std::cerr << "   Using default SMTP settings" << std::endl;
        return cfg;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("SMTP_HOST=") == 0) cfg.smtp_host = line.substr(10);
        else if (line.find("SMTP_PORT=") == 0) cfg.smtp_port = line.substr(10);
        else if (line.find("SMTP_FROM=") == 0) cfg.smtp_from = line.substr(10);
        else if (line.find("SMTP_PASSWORD=") == 0) cfg.smtp_password = line.substr(14);
        else if (line.find("SMTP_TO=") == 0) cfg.smtp_to = line.substr(8);
    }
    file.close();
    
    // Очистка от пробелов, кавычек и CR
    auto clean = [](std::string& s) {
        s.erase(remove(s.begin(), s.end(), ' '), s.end());
        s.erase(remove(s.begin(), s.end(), '"'), s.end());
        s.erase(remove(s.begin(), s.end(), '\r'), s.end());
    };
    clean(cfg.smtp_host);
    clean(cfg.smtp_port);
    clean(cfg.smtp_from);
    clean(cfg.smtp_password);
    clean(cfg.smtp_to);
    
    return cfg;
}

    void generateAndSaveForecasts(int agent_id) {
        std::vector<std::string> metrics = {"cpu", "memory", "swap", "load_avg"};
        
        for (const auto& metric : metrics) {
            // Получить историю за последние 72 часа
            globalDbClient->execSqlAsync(
                "SELECT value FROM metrics WHERE agent_id = $1 AND metric_name = $2 "
                "ORDER BY timestamp ASC LIMIT 72",
                [agent_id, metric](const orm::Result& result) {
                    std::vector<double> history;
                    for (const auto& row : result) {
                        history.push_back(row["value"].as<double>());
                    }
                    
                    if (history.size() < 10) return;
                    
                    // Вызвать ARIMA-модуль
                    Json::Value request;
                    for (double v : history) request["history"].append(v);
                    request["horizon"] = 12;
                    
                    auto client = HttpClient::newHttpClient("http://arima:8081");
                    auto req = HttpRequest::newHttpJsonRequest(request);
                    req->setPath("/forecast");
                    req->setMethod(Post);
                    
                    client->sendRequest(req, [agent_id, metric](ReqResult result, const HttpResponsePtr& response) {
                        if (result == ReqResult::Ok && response && response->getStatusCode() == k200OK) {
                            auto json = response->getJsonObject();
                            if (json && (*json).isMember("forecast")) {
                                // Сохранить прогнозы в БД
                                auto forecasts = (*json)["forecast"];
                                auto now = std::chrono::system_clock::now();
                                
                                for (int i = 0; i < forecasts.size(); ++i) {
                                    auto forecast_time = now + std::chrono::hours(i + 1);
                                    std::stringstream ss;
                                    ss << std::chrono::duration_cast<std::chrono::seconds>(
                                        forecast_time.time_since_epoch()).count();
                                    
                                    globalDbClient->execSqlAsync(
                                        "INSERT INTO forecasts (agent_id, metric_name, forecast_value, forecast_timestamp) "
                                        "VALUES ($1, $2, $3, to_timestamp($4))",
                                        [](const orm::Result&) {},
                                        [](const orm::DrogonDbException& e) {
                                            std::cerr << "Failed to save forecast: " << e.base().what() << std::endl;
                                        },
                                        agent_id, metric, forecasts[i].asDouble(), ss.str()
                                    );
                                }
                                std::cout << "Saved forecasts for agent " << agent_id << " metric " << metric << std::endl;
                            }
                            invalidateCache("forecast:" + std::to_string(agent_id) + ":" + metric);
                        }
                    });
                },
                [](const orm::DrogonDbException& e) {
                    std::cerr << "DB error in generateAndSaveForecasts: " << e.base().what() << std::endl;
                },
                agent_id, metric
            );
        }
    }

    void runScheduledForecast() {
        std::thread([&]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::minutes(30)); // Каждые 30 мин
                
                std::cout << "🔄 Running scheduled forecast..." << std::endl;
                
                // 1. Получить всех агентов
                globalDbClient->execSqlAsync(
                    "SELECT id FROM agents",
                    [](const orm::Result& result) {
                        for (const auto& row : result) {
                            int agent_id = row["id"].as<int>();
                            generateAndSaveForecasts(agent_id);
                        }
                    },
                    [](const orm::DrogonDbException& e) {
                        std::cerr << "Error getting agents: " << e.base().what() << std::endl;
                    }
                );
            }
        }).detach();
    }
    
// ============================================================
// ВСПОМОГАТЕЛЬНАЯ ФУНКЦИЯ ДЛЯ ПРОВЕРКИ СЕССИИ
// ============================================================
bool checkSession(const HttpRequestPtr& req, 
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string& session_id) {
    if (!ensureRedisConnected()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("{\"error\": \"Redis unavailable\"}");
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        callback(resp);
        return false;
    }
    
    auto cookies = req->getCookies();
    auto it = cookies.find("session_id");
    if (it == cookies.end()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k401Unauthorized);
        resp->setBody("Unauthorized");
        callback(resp);
        return false;
    }
    
    session_id = it->second;

    std::cout << "🔍 [DEBUG] checkSession called with session_id: " << session_id << std::endl;
    std::string cached = getCached("session:" + session_id);
    std::cout << "🔍 [DEBUG] cached value: '" << cached << "'" << std::endl;

    if (cached.empty()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k401Unauthorized);
        resp->setBody("Session expired");
        callback(resp);
        return false;
    }
    
    // Обновляем TTL
    setCached("session:" + session_id, cached, 3600);
    return true;
}

// ============================================================
// КЛАСС КОНТРОЛЛЕРА
// ============================================================
class MetricsController : public HttpController<MetricsController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(MetricsController::getRoot, "/", Get);
        ADD_METHOD_TO(MetricsController::login, "/api/login", Post);

        // Агенты
        ADD_METHOD_TO(MetricsController::registerAgent, "/api/agents/register", Post);
        ADD_METHOD_TO(MetricsController::getAgents, "/api/agents", Get);
        
        // Метрики
        ADD_METHOD_TO(MetricsController::receiveMetrics, "/api/metrics", Post);
        ADD_METHOD_TO(MetricsController::getLatest, "/api/metrics/latest", Get);
        ADD_METHOD_TO(MetricsController::getHistory, "/api/metrics/history", Get);
        ADD_METHOD_TO(MetricsController::getAgentHistory, "/api/metrics/{1}/history", Get);
        
        // Алёрты и прогнозы
        ADD_METHOD_TO(MetricsController::getAlerts, "/api/alerts", Get);
        ADD_METHOD_TO(MetricsController::resolveAlert, "/api/alerts/{1}/resolve", Post);
        ADD_METHOD_TO(MetricsController::getForecast, "/api/forecast/{1}/{2}", Get);
        ADD_METHOD_TO(MetricsController::getStoredForecast, "/api/forecast/stored/{1}/{2}", Get);
    METHOD_LIST_END

    void getRoot(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody("<!DOCTYPE html><html><body><h1>Monitoring Server</h1><p>Go to <a href='/dashboard'>/dashboard</a></p></body></html>");
        resp->setContentTypeCode(CT_TEXT_HTML);
        callback(resp);
    }

    void login(const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback) {
        auto json = req->getJsonObject();
        if (!json || !(*json).isMember("username") || !(*json).isMember("password")) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setBody("Missing credentials");
            callback(resp);
            return;
        }

        std::string username = (*json)["username"].asString();
        std::string password = (*json)["password"].asString();

        if (username == "admin" && password == "admin") {
            std::string session_id = generateApiKey();
            setCached("session:" + session_id, username, 3600);

            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k200OK);
            resp->addHeader("Set-Cookie", "session_id=" + session_id + "; HttpOnly; Path=/; Max-Age=3600; Secure; SameSite=Lax");
            resp->setBody("{\"status\": \"ok\"}");
            callback(resp);
        } else {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k401Unauthorized);
            resp->setBody("Invalid credentials");
            callback(resp);
        }
    }

    void registerAgent(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback) {
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 

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
        auto cookies = req->getCookies();
        std::cout << "Cookies: " << cookies.size() << std::endl;  // ← для отладки
        
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 

        
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
                setCached(cacheKey, writer.write(json), 30);
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
                
                // ============================================================
                // КЛЮЧЕВОЕ ИЗМЕНЕНИЕ: извлекаем timestamp из JSON
                // ============================================================
                std::string timestamp_expr = "NOW()";
                if ((*json).isMember("timestamp")) {
                    long long ts = (*json)["timestamp"].asInt64();
                    timestamp_expr = "to_timestamp(" + std::to_string(ts / 1000) + ")";
                }
                
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
                        "INSERT INTO metrics (agent_id, metric_name, value, timestamp) VALUES ($1, $2, $3, " + timestamp_expr + ")",
                        [this, name, value, insertedCount, totalMetrics](const orm::Result&) {
                            if (name == "cpu" || name == "memory" || name == "ram") {
                                this->checkThresholds((name == "cpu") ? "cpu" : "memory", value);
                            }
                            std::cout << "Inserted " << name << "=" << value << std::endl;
                            
                            (*insertedCount)++;
                            
                            if (*insertedCount == totalMetrics) {
                                invalidateCache("history:");
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
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return;
        }
        
        // Читаем параметр agent_id
        auto params = req->getParameters();
        std::string agent_filter = "";
        if (params.find("agent_id") != params.end()) {
            agent_filter = "AND m.agent_id = " + params["agent_id"];
        }
        
        std::string cacheKey = "latest" + agent_filter;
        std::string cachedJson = getCached(cacheKey);
        if (!cachedJson.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody(cachedJson);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            callback(resp);
            return;
        }
        
        std::string sql = 
            "SELECT metric_name, value, EXTRACT(EPOCH FROM timestamp) * 1000 as ts, a.name as agent_name "
            "FROM metrics m JOIN agents a ON m.agent_id = a.id "
            "WHERE 1=1 " + agent_filter + " "
            "ORDER BY m.timestamp DESC LIMIT 10";
        
        globalDbClient->execSqlAsync(
            sql,
            [cacheKey, callback](const orm::Result& result) {
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
                
                Json::FastWriter writer;
                setCached(cacheKey, writer.write(json), 30);
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                std::cerr << "DB error in getLatest: " << e.base().what() << std::endl;
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            }
        );
    }
    
    void getHistory(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback) {
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return;
        }
        
        // Читаем параметры из запроса
        auto params = req->getParameters();
        int period_hours = 72;
        std::string agent_id = "";
        
        if (params.find("period") != params.end()) {
            period_hours = std::stoi(params["period"]);
        }
        if (params.find("agent_id") != params.end()) {
            agent_id = params["agent_id"];
        }
        
        // Кэш зависит от периода и агента
        std::string cacheKey = "history:period:" + std::to_string(period_hours) +
                            ":agent:" + (agent_id.empty() ? "all" : agent_id);
        
        // Проверяем кэш
        std::string cachedJson = getCached(cacheKey);
        if (!cachedJson.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody(cachedJson);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            callback(resp);
            return;
        }
        
        // ✅ ЗАПРОС: все данные за период (без LIMIT)
        std::string sql = 
            "SELECT metric_name, value, EXTRACT(EPOCH FROM timestamp) * 1000 as ts, a.name as agent_name "
            "FROM metrics m JOIN agents a ON m.agent_id = a.id "
            "WHERE m.timestamp > NOW() - INTERVAL '" + std::to_string(period_hours) + " hours' ";
        
        if (!agent_id.empty()) {
            sql += "AND m.agent_id = " + agent_id + " ";
        }
        
        sql += "ORDER BY m.timestamp DESC";  // Новые сверху, старые снизу
        
        globalDbClient->execSqlAsync(
            sql,
            [cacheKey, callback](const orm::Result& result) {
                // Собираем данные
                std::vector<std::tuple<double, std::string, double, std::string>> rows;
                for (const auto& row : result) {
                    double ts = row["ts"].as<double>();
                    std::string name = row["metric_name"].as<std::string>();
                    double value = row["value"].as<double>();
                    std::string agent = row["agent_name"].as<std::string>();
                    rows.emplace_back(ts, name, value, agent);
                }
                
                // Группируем по времени
                std::map<double, Json::Value> grouped;
                for (const auto& [ts, name, value, agent] : rows) {
                    if (!grouped.count(ts)) {
                        grouped[ts] = Json::Value(Json::objectValue);
                        grouped[ts]["timestamp"] = ts;
                        grouped[ts]["agent_name"] = agent;
                        grouped[ts]["metrics"] = Json::Value(Json::objectValue);
                    }
                    grouped[ts]["metrics"][name] = value;
                }
                
                // От старых к новым (для графика)
                Json::Value json(Json::arrayValue);
                for (const auto& pair : grouped) {
                    json.append(pair.second);
                }
                
                Json::FastWriter writer;
                setCached(cacheKey, writer.write(json), 120);
                
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                std::cerr << "DB error in getHistory: " << e.base().what() << std::endl;
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            }
        );
    }
    
    void getAgentHistory(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        int agent_id) {
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 

        globalDbClient->execSqlAsync(
            "SELECT metric_name, value, EXTRACT(EPOCH FROM timestamp) * 1000 as ts "
            "FROM metrics WHERE agent_id = $1 "
            "ORDER BY timestamp DESC LIMIT 500",  // ← DESC
            [callback](const orm::Result& result) {
                // Собираем строки и переворачиваем
                std::vector<std::tuple<double, std::string, double>> rows;
                for (const auto& row : result) {
                    rows.emplace_back(row["ts"].as<double>(), 
                                    row["metric_name"].as<std::string>(), 
                                    row["value"].as<double>());
                }
                std::reverse(rows.begin(), rows.end());
                
                // Группируем по метрикам
                std::map<std::string, Json::Value> series;
                for (const auto& [ts, name, value] : rows) {
                    if (!series.count(name)) {
                        series[name] = Json::Value(Json::arrayValue);
                    }
                    Json::Value point;
                    point["timestamp"] = ts;
                    point["value"] = value;
                    series[name].append(point);
                }
                
                Json::Value json(Json::objectValue);
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
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 
                    
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
                        auto resp = HttpResponse::newHttpResponse();
                        resp->setBody(std::string(response->getBody()));
                        resp->setContentTypeCode(CT_APPLICATION_JSON);
                        callback(resp);
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

    void getStoredForecast(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        int agent_id,
                        const std::string& metric_name) {
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 

        std::string cacheKey = "forecast:" + std::to_string(agent_id) + ":" + metric_name;
        std::string cachedJson = getCached(cacheKey);
        if (!cachedJson.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody(cachedJson);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            callback(resp);
            return;
        }
        
        // Нет в кэше → идём в БД
        globalDbClient->execSqlAsync(
            "SELECT forecast_value, EXTRACT(EPOCH FROM forecast_timestamp) * 1000 as ts "
            "FROM forecasts WHERE agent_id = $1 AND metric_name = $2 "
            "ORDER BY forecast_timestamp ASC",
            [cacheKey, callback](const orm::Result& result) {
                Json::Value json;
                Json::Value timestamps(Json::arrayValue);
                Json::Value values(Json::arrayValue);
                
                for (const auto& row : result) {
                    timestamps.append(row["ts"].as<double>());
                    values.append(row["forecast_value"].as<double>());
                }
                
                json["timestamps"] = timestamps;
                json["values"] = values;
                
                Json::FastWriter writer;
                setCached(cacheKey, writer.write(json), 120);
                callback(HttpResponse::newHttpJsonResponse(json));
            },
            [callback](const orm::DrogonDbException& e) {
                callback(HttpResponse::newHttpResponse(k500InternalServerError, CT_TEXT_PLAIN));
            },
            agent_id, metric_name
        );
    }

    void getAlerts(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 

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
        // Проверка сессии
        std::string session_id;
        if (!checkSession(req, std::move(callback), session_id)) {
            return; 
        } 

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
        
        // 3. Отправка email-уведомления
        std::string subject = "⚠️ Monitoring Alert: " + metric_name;
        sendEmailNotification(subject, message);
    }

    void sendEmailNotification(const std::string& subject, const std::string& body) {
        std::cout << "📧 [DEBUG] sendEmailNotification called. Subject: " << subject << std::endl;
        std::cout.flush();
        
        if (serverConfig.smtp_password.empty() || serverConfig.smtp_to.empty()) {
            std::cerr << "⚠️ SMTP not configured (password or recipient missing), email not sent" << std::endl;
            return;
        }
        
        std::string to = serverConfig.smtp_to;
        
        // Формируем письмо
        std::string mail_content = 
            "From: " + serverConfig.smtp_from + "\r\n"
            "To: " + to + "\r\n"
            "Subject: " + subject + "\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            + body + "\r\n";
        
        // Создаём временный файл
        std::string tmp_file = "/tmp/email_" + std::to_string(std::time(nullptr)) + ".txt";
        std::ofstream file(tmp_file);
        if (!file.is_open()) {
            std::cerr << "❌ Failed to create temporary email file" << std::endl;
            return;
        }
        file << mail_content;
        file.close();
        
        // Отправляем через curl
        std::string cmd = "curl -s --ssl-reqd "
                        "--url 'smtps://" + serverConfig.smtp_host + ":" + serverConfig.smtp_port + "' "
                        "--user '" + serverConfig.smtp_from + ":" + serverConfig.smtp_password + "' "
                        "--mail-from '" + serverConfig.smtp_from + "' "
                        "--mail-rcpt '" + to + "' "
                        "--upload-file " + tmp_file + " 2>&1";
        
        std::cout << "📧 [DEBUG] Command: " << cmd << std::endl;
        std::cout.flush();
        
        int ret = system(cmd.c_str());
        
        // Удаляем временный файл
        std::remove(tmp_file.c_str());
        
        std::cout << "📧 [DEBUG] curl exit code: " << ret << std::endl;
        std::cout.flush();
        
        if (ret != 0) {
            std::cerr << "❌ Failed to send email notification (curl exit code: " << ret << ")" << std::endl;
        } else {
            std::cout << "✅ Email notification sent to " << to << std::endl;
        }
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

    // Чтение конфига
    serverConfig = loadServerConfig();

    // Запуск планировщика прогнозов
    runScheduledForecast();
    
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
