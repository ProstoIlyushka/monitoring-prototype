#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <cstdio>
#include <vector>
#include <mutex>
#include <queue>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <algorithm>

// ============================================================
// Конфигурация (значения по умолчанию)
// ============================================================
const std::string DEFAULT_SERVER_URL = "https://localhost:8443/api/metrics";
const std::string BUFFER_FILE = "/var/lib/monitoring-agent/agent_buffer.dat";
const int MAX_BUFFER_SIZE = 10000;
const int RETRY_INTERVAL_SEC = 30;

// ============================================================
// Глобальные переменные
// ============================================================
std::string SERVER_URL = DEFAULT_SERVER_URL;
std::queue<std::string> bufferQueue;
std::mutex bufferMutex;
std::atomic<bool> isSending{false};
std::atomic<bool> running{true};

// ============================================================
// Обработчик сигналов
// ============================================================
void signalHandler(int signum) {
    running = false;
}

// ============================================================
// Создание директории
// ============================================================
void ensureDirectoryExists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        std::string dir = path.substr(0, path.find_last_of('/'));
        mkdir(dir.c_str(), 0755);
    }
}

// ============================================================
// Сохранение буфера на диск
// ============================================================
void saveBufferToFile() {
    if (bufferQueue.empty()) {
        std::remove(BUFFER_FILE.c_str());
        return;
    }
    
    ensureDirectoryExists(BUFFER_FILE);
    
    std::ofstream file(BUFFER_FILE, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Failed to open buffer file: " << BUFFER_FILE << std::endl;
        return;
    }
    
    size_t size = bufferQueue.size();
    file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    
    std::queue<std::string> temp = bufferQueue;
    while (!temp.empty()) {
        const std::string& record = temp.front();
        size_t len = record.size();
        file.write(reinterpret_cast<const char*>(&len), sizeof(len));
        file.write(record.c_str(), len);
        temp.pop();
    }
    file.close();
}

// ============================================================
// Загрузка буфера с диска
// ============================================================
void loadBufferFromFile() {
    std::ifstream file(BUFFER_FILE, std::ios::binary);
    if (!file.is_open()) return;
    
    size_t size = 0;
    file.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (size == 0 || size > MAX_BUFFER_SIZE * 2) {
        file.close();
        std::remove(BUFFER_FILE.c_str());
        return;
    }
    
    for (size_t i = 0; i < size; ++i) {
        size_t len = 0;
        file.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (len == 0 || len > 4096) break;
        
        std::string record(len, '\0');
        file.read(&record[0], len);
        bufferQueue.push(record);
    }
    file.close();
    
    if (!bufferQueue.empty()) {
        std::cout << "📂 Loaded " << bufferQueue.size() << " buffered metrics from disk" << std::endl;
    }
}

// ============================================================
// Чтение конфига
// ============================================================
std::string readConfigValue(const std::string& key, const std::string& defaultValue = "") {
    std::ifstream file("/etc/monitoring-agent/config");
    if (!file.is_open()) return defaultValue;
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.find(key + "=") == 0) {
            std::string value = line.substr(key.length() + 1);
            value.erase(remove(value.begin(), value.end(), ' '), value.end());
            value.erase(remove(value.begin(), value.end(), '"'), value.end());
            return value;
        }
    }
    return defaultValue;
}

// ============================================================
// Сбор метрик CPU
// ============================================================
double getCpuUsage() {
    static unsigned long long prevTotal = 0;
    static unsigned long long prevIdle = 0;
    
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return 0;
    
    std::string line;
    std::getline(file, line);
    file.close();
    
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;
    unsigned long long idleTotal = idle + iowait;
    
    double usage = 0;
    if (prevTotal != 0) {
        usage = 100.0 * (1.0 - (double)(idleTotal - prevIdle) / (total - prevTotal));
    }
    
    prevTotal = total;
    prevIdle = idleTotal;
    return usage;
}

double getMemoryUsage() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return 0;
    
    std::string line;
    unsigned long long total = 0, available = 0;
    
    while (std::getline(file, line)) {
        if (line.find("MemTotal:") == 0) {
            sscanf(line.c_str(), "MemTotal: %llu kB", &total);
        } else if (line.find("MemAvailable:") == 0) {
            sscanf(line.c_str(), "MemAvailable: %llu kB", &available);
        }
    }
    file.close();
    
    if (total == 0) return 0;
    return 100.0 * (1.0 - (double)available / total);
}

double getSwapUsage() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return 0;
    
    std::string line;
    unsigned long long total = 0, free = 0;
    
    while (std::getline(file, line)) {
        if (line.find("SwapTotal:") == 0) {
            sscanf(line.c_str(), "SwapTotal: %llu kB", &total);
        } else if (line.find("SwapFree:") == 0) {
            sscanf(line.c_str(), "SwapFree: %llu kB", &free);
        }
    }
    file.close();
    
    if (total == 0) return 0;
    return 100.0 * (double)(total - free) / total;
}

double getLoadAverage() {
    std::ifstream file("/proc/loadavg");
    if (!file.is_open()) return 0;
    
    double load1, load5, load15;
    file >> load1 >> load5 >> load15;
    file.close();
    
    return load1;
}

int getProcessesRunning() {
    std::ifstream file("/proc/loadavg");
    if (!file.is_open()) return 0;
    
    double load1, load5, load15;
    int running, total;
    file >> load1 >> load5 >> load15 >> running >> total;
    file.close();
    
    return running;
}

long long getNetworkRx() {
    static long long prev = 0;
    static long long prev_time = 0;
    
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return 0;
    
    std::string line;
    long long rx = 0;
    
    while (std::getline(file, line)) {
        if (line.find(":") == std::string::npos) continue;
        
        size_t colon = line.find(':');
        std::string iface = line.substr(0, colon);
        if (iface.size() > 0 && (iface[0] == 'e' || iface == "eth0" || iface.find("ens") == 0 || iface.find("enp") == 0)) {
            std::istringstream iss(line.substr(colon + 1));
            iss >> rx;
            break;
        }
    }
    file.close();
    
    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    long long diff = 0;
    if (prev != 0 && prev_time != 0 && now - prev_time > 0) {
        diff = (rx - prev) / (now - prev_time);
        if (diff < 0) diff = 0;
    }
    
    prev = rx;
    prev_time = now;
    return diff;
}

long long getNetworkTx() {
    static long long prev = 0;
    static long long prev_time = 0;
    
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return 0;
    
    std::string line;
    long long tx = 0;
    
    while (std::getline(file, line)) {
        if (line.find(":") == std::string::npos) continue;
        
        size_t colon = line.find(':');
        std::string iface = line.substr(0, colon);
        if (iface.size() > 0 && (iface[0] == 'e' || iface == "eth0" || iface.find("ens") == 0 || iface.find("enp") == 0)) {
            std::istringstream iss(line.substr(colon + 1));
            long long rx, rx_packets, rx_errs, rx_drop, rx_fifo, rx_frame, rx_compressed, rx_multicast;
            iss >> rx >> rx_packets >> rx_errs >> rx_drop >> rx_fifo >> rx_frame >> rx_compressed >> rx_multicast >> tx;
            break;
        }
    }
    file.close();
    
    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    long long diff = 0;
    if (prev != 0 && prev_time != 0 && now - prev_time > 0) {
        diff = (tx - prev) / (now - prev_time);
        if (diff < 0) diff = 0;
    }
    
    prev = tx;
    prev_time = now;
    return diff;
}

// ============================================================
// Отправка метрик на сервер
// ============================================================
bool sendMetricsToServer(const std::string& json) {
    int retries = 3;
    int delay = 2;
    
    // Читаем API_KEY из конфига при каждом вызове (или можно передавать)
    std::string api_key = readConfigValue("API_KEY");
    if (api_key.empty()) {
        std::cerr << "API_KEY not configured" << std::endl;
        return false;
    }
    
    for (int attempt = 0; attempt < retries; ++attempt) {
        if (!running) return false;
        
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("X-API-Key: " + api_key).c_str());
        
        curl_easy_setopt(curl, CURLOPT_URL, SERVER_URL.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        
        CURLcode res = curl_easy_perform(curl);
        
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res == CURLE_OK && (http_code == 200 || http_code == 201)) {
            return true;
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        delay *= 2;
    }
    
    return false;
}

// ============================================================
// Добавление в буфер
// ============================================================
void addToBuffer(const std::string& json) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    if (bufferQueue.size() < MAX_BUFFER_SIZE) {
        bufferQueue.push(json);
    } else {
        bufferQueue.pop();
        bufferQueue.push(json);
    }
    saveBufferToFile();
}

// ============================================================
// Отправка метрики (основной поток)
// ============================================================
void sendMetrics(double cpu, double memory, double swap, double load_avg, 
                 int processes, long long net_rx, long long net_tx) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    char json[512];
    snprintf(json, sizeof(json), 
        "{"
        "\"cpu\": %.2f, "
        "\"memory\": %.2f, "
        "\"ram\": %.2f, "
        "\"swap\": %.2f, "
        "\"load_avg\": %.2f, "
        "\"processes_running\": %d, "
        "\"network_rx\": %lld, "
        "\"network_tx\": %lld, "
        "\"timestamp\": %ld"
        "}",
        cpu, memory, memory, swap, load_avg, processes, net_rx, net_tx, ms);
    
    if (!sendMetricsToServer(json)) {
        addToBuffer(json);
    }
}

// ============================================================
// Фоновый поток досылки буфера
// ============================================================
void flushBufferThread() {
    while (running) {
        for (int i = 0; i < RETRY_INTERVAL_SEC && running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!running) break;
        
        bool hasData = false;
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            hasData = !bufferQueue.empty();
        }
        
        if (!hasData) continue;
        if (isSending.exchange(true)) continue;
        
        std::queue<std::string> sendQueue;
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            std::swap(sendQueue, bufferQueue);
            std::remove(BUFFER_FILE.c_str());
        }
        
        std::queue<std::string> failedQueue;
        size_t successCount = 0;
        
        while (!sendQueue.empty() && running) {
            const std::string& json = sendQueue.front();
            if (sendMetricsToServer(json)) {
                successCount++;
            } else {
                failedQueue.push(json);
            }
            sendQueue.pop();
        }
        
        if (!running && !sendQueue.empty()) {
            std::lock_guard<std::mutex> lock(bufferMutex);
            while (!sendQueue.empty()) {
                bufferQueue.push(sendQueue.front());
                sendQueue.pop();
            }
            while (!failedQueue.empty()) {
                bufferQueue.push(failedQueue.front());
                failedQueue.pop();
            }
            saveBufferToFile();
            isSending = false;
            break;
        }
        
        if (!failedQueue.empty()) {
            std::lock_guard<std::mutex> lock(bufferMutex);
            while (!failedQueue.empty()) {
                bufferQueue.push(failedQueue.front());
                failedQueue.pop();
            }
            saveBufferToFile();
        }
        
        isSending = false;
    }
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Читаем SERVER_URL из конфига
    std::string server_url = readConfigValue("SERVER_URL", DEFAULT_SERVER_URL);
    SERVER_URL = server_url;
    
    // Читаем API_KEY
    std::string api_key = readConfigValue("API_KEY");
    if (argc >= 2) {
        api_key = argv[1];
    }
    
    if (api_key.empty()) {
        std::cerr << "Error: API_KEY not found." << std::endl;
        std::cerr << "Usage: " << argv[0] << " <API_KEY>" << std::endl;
        std::cerr << "Or set API_KEY in /etc/monitoring-agent/config" << std::endl;
        return 1;
    }
    
    // Загружаем буфер с диска
    loadBufferFromFile();
    
    // Запускаем фоновый поток
    std::thread flushThread(flushBufferThread);
    flushThread.detach();
    
    std::cout << "========================================" << std::endl;
    std::cout << "🔄 Monitoring Agent Started" << std::endl;
    std::cout << "Server: " << SERVER_URL << std::endl;
    std::cout << "API Key: " << api_key.substr(0, 8) << "..." << std::endl;
    std::cout << "Buffer size: " << bufferQueue.size() << " metrics" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int count = 0;
    while (running) {
        double cpu = getCpuUsage();
        double memory = getMemoryUsage();
        double swap = getSwapUsage();
        double load_avg = getLoadAverage();
        int processes = getProcessesRunning();
        long long net_rx = getNetworkRx();
        long long net_tx = getNetworkTx();
        
        if (count % 6 == 0) {
            std::cout << "CPU: " << cpu << "% | RAM: " << memory << "% | Swap: " << swap << "%" << std::endl;
            std::cout << "Load: " << load_avg << " | Processes: " << processes << std::endl;
            if (!bufferQueue.empty()) {
                std::cout << "📦 Buffer: " << bufferQueue.size() << " metrics waiting" << std::endl;
            }
        }
        
        sendMetrics(cpu, memory, swap, load_avg, processes, net_rx, net_tx);
        
        std::this_thread::sleep_for(std::chrono::seconds(10));
        count++;
    }
    
    saveBufferToFile();
    std::cout << "🛑 Agent stopped" << std::endl;
    
    return 0;
}