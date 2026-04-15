#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <cstdio>

// Отправляем на HTTPS-порт nginx (8443), а не напрямую на Drogon
const std::string SERVER_URL = "https://localhost:8443/api/metrics";

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

void sendMetrics(double cpu, double memory, const std::string& api_key) {
    CURL* curl = curl_easy_init();
    if (!curl) return;
    
    char json[256];
    snprintf(json, sizeof(json), "{\"cpu\": %.1f, \"memory\": %.1f}", cpu, memory);
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("X-API-Key: " + api_key).c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, SERVER_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <API_KEY>" << std::endl;
        std::cerr << "Example: " << argv[0] << " a1b2c3d4e5f6..." << std::endl;
        return 1;
    }
    
    std::string api_key = argv[1];
    
    std::cout << "========================================" << std::endl;
    std::cout << "Monitoring Agent Started" << std::endl;
    std::cout << "Server: " << SERVER_URL << std::endl;
    std::cout << "API Key: " << api_key.substr(0, 8) << "..." << std::endl;
    std::cout << "Sending metrics every 5 seconds" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int count = 0;
    while (true) {
        double cpu = getCpuUsage();
        double memory = getMemoryUsage();
        
        if (count % 10 == 0) {
            std::cout << "CPU: " << cpu << "% | Memory: " << memory << "%" << std::endl;
        }
        
        sendMetrics(cpu, memory, api_key);
        
        std::this_thread::sleep_for(std::chrono::seconds(5));
        count++;
    }
    
    return 0;
}
