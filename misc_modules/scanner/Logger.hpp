#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <chrono>
#include <filesystem>
struct ScanRecord {
    double frequency;
    float dBm;
    std::chrono::system_clock::time_point timestamp;
    std::chrono::system_clock::time_point endTimestamp;
    float durationSeconds;
    bool isEndOfTransmission;  // true when logging the end of a transmission
    // optional GPS fields could be added later
};
class ScannerLogger {
public:
    ScannerLogger();
    ~ScannerLogger();
    void start(const std::string& path);
    void stop();
    void log(const ScanRecord& rec);
private:
    void worker();
    std::atomic<bool> running{false};
    std::thread th;
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<ScanRecord> q;
    std::ofstream file;
};