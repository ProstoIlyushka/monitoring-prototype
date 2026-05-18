#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <cstdio>

// Отправляем на HTTPS-порт nginx (8443), а не напрямую на Drogon
const std::string SERVER_URL = "https://localhost:8443/api/metrics";

// ============================================================
// Сбор метрик CPU (как было, работает)
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

// ============================================================
// Сбор метрик RAM (как было, работает)
// ============================================================
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

// ============================================================
// НОВОЕ: Swap usage
// ============================================================
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

// ============================================================
// НОВОЕ: Load Average (1 min) и количество процессов в очереди
// ============================================================
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

// ============================================================
// НОВОЕ: Сетевой трафик (байт/сек)
// ============================================================
long long getNetworkRx() {
    static long long prev = 0;
    static long long prev_time = 0;
    
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return 0;
    
    std::string line;
    long long rx = 0;
    
    // Ищем интерфейс (eth0, ens33, enp0s3 и т.д.)
    while (std::getline(file, line)) {
        // Пропускаем заголовки
        if (line.find(":") == std::string::npos) continue;
        
        // Ищем интерфейс, начинающийся с e (eth/enp/ens)
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
// Отправка метрик (расширенный JSON)
// ============================================================
void sendMetrics(double cpu, double memory, double swap, double load_avg, 
                 int processes, long long net_rx, long long net_tx,
                 const std::string& api_key) {
    CURL* curl = curl_easy_init();
    if (!curl) return;
    
    // Формируем JSON со всеми метриками
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
        "\"network_tx\": %lld"
        "}",
        cpu, memory, memory, swap, load_avg, processes, net_rx, net_tx);
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("X-API-Key: " + api_key).c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, SERVER_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    
    // Для самоподписанного сертификата nginx
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Failed to send: " << curl_easy_strerror(res) << std::endl;
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <API_KEY>" << std::endl;
        std::cerr << "Example: " << argv[0] << " a1b2c3d4e5f6..." << std::endl;
        std::cerr << "\nGet API_KEY: curl -X POST http://localhost:8080/api/agents/register -H 'Content-Type: application/json' -d '{\"name\": \"my-server\"}'" << std::endl;
        return 1;
    }
    
    std::string api_key = argv[1];
    
    std::cout << "========================================" << std::endl;
    std::cout << "Monitoring Agent Started" << std::endl;
    std::cout << "Server: " << SERVER_URL << std::endl;
    std::cout << "API Key: " << api_key.substr(0, 8) << "..." << std::endl;
    std::cout << "Sending metrics every 10 seconds" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int count = 0;
    while (true) {
        double cpu = getCpuUsage();
        double memory = getMemoryUsage();
        double swap = getSwapUsage();
        double load_avg = getLoadAverage();
        int processes = getProcessesRunning();
        long long net_rx = getNetworkRx();
        long long net_tx = getNetworkTx();
        
        if (count % 6 == 0) {  // Каждую минуту (10 сек * 6)
            std::cout << "CPU: " << cpu << "% | RAM: " << memory << "% | Swap: " << swap << "%" << std::endl;
            std::cout << "Load: " << load_avg << " | Processes: " << processes << std::endl;
            std::cout << "Net RX: " << net_rx << " B/s | TX: " << net_tx << " B/s" << std::endl;
        }
        
        sendMetrics(cpu, memory, swap, load_avg, processes, net_rx, net_tx, api_key);
        
        std::this_thread::sleep_for(std::chrono::seconds(10));
        count++;
    }
    
    return 0;
}