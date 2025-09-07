#include "Logger.hpp"
#include <utils/flog.h>
#include <iomanip>
ScannerLogger::ScannerLogger() {}
ScannerLogger::~ScannerLogger(){ stop(); }
void ScannerLogger::start(const std::string& path){
    if(running) return;
    
    // Check if file exists to determine if we need to write headers
    bool fileExists = std::filesystem::exists(path);
    
    file.open(path, std::ios::out | std::ios::app);
    if(!file.is_open()){
        flog::error("Failed to open scan log {}", path);
        return;
    }
    
    // Write CSV header if this is a new file
    if (!fileExists) {
        file << "Frequency_Hz,Signal_dBFS,Start_Timestamp,End_Timestamp,Duration_Seconds,Frequency_MHz\n";
        file.flush();
    }
    
    running = true;
    th = std::thread(&ScannerLogger::worker, this);
    flog::info("Scanner logger started, writing to: {}", path);
}
void ScannerLogger::stop(){
    if(!running) return;
    {
        std::lock_guard<std::mutex> lk(mtx);
        running = false;
    }
    cv.notify_one();
    if(th.joinable()) th.join();
    if(file.is_open()) file.close();
}
void ScannerLogger::log(const ScanRecord& rec){
    {
        std::lock_guard<std::mutex> lk(mtx);
        q.push(rec);
    }
    cv.notify_one();
}
void ScannerLogger::worker(){
    while(true){
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this]{ return !q.empty() || !running; });
        if(!running && q.empty()) break;
        auto rec = q.front(); q.pop();
        lk.unlock();
        if(file.is_open()){
            auto startTime = std::chrono::system_clock::to_time_t(rec.timestamp);
            auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                rec.timestamp.time_since_epoch()) % 1000;
            
            auto endTime = std::chrono::system_clock::to_time_t(rec.endTimestamp);
            auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                rec.endTimestamp.time_since_epoch()) % 1000;
            
            // Format: Frequency_Hz, Signal_dBFS, Start_Timestamp, End_Timestamp, Duration_Seconds, Frequency_MHz
            file << std::fixed << std::setprecision(0) << rec.frequency << ","
                 << std::setprecision(1) << rec.dBm << ","
                 << std::put_time(std::localtime(&startTime), "%Y-%m-%d %H:%M:%S")
                 << "." << std::setfill('0') << std::setw(3) << startMs.count() << ","
                 << std::put_time(std::localtime(&endTime), "%Y-%m-%d %H:%M:%S")
                 << "." << std::setfill('0') << std::setw(3) << endMs.count() << ","
                 << std::setprecision(3) << rec.durationSeconds << ","
                 << std::setprecision(6) << (rec.frequency / 1e6) << "\n";
            file.flush(); // Ensure immediate write for real-time monitoring
        }
    }
}
