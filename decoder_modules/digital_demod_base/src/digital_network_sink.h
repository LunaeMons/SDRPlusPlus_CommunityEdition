#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <utils/networking.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <chrono>
#include <condition_variable>
#include "protocol_types.h"

namespace digital_demod {

    class DigitalNetworkSink {
    public:
        DigitalNetworkSink(ProtocolType protocolType);
        ~DigitalNetworkSink();

        // Configuration
        void setNetworkConfig(const std::string& hostname, int port, bool useUDP);
        void setProtocolType(ProtocolType type);
        
        // Control
        bool start();
        void stop();
        bool isRunning() const { return running; }
        bool isConnected() const;
        
        // Data input
        void sendData(const uint8_t* data, int count);
        void sendHeader(); // Send protocol header
        
        // Statistics
        uint64_t getBytesSent() const { return bytesSent; }
        uint64_t getPacketsSent() const { return packetsSent; }
        std::string getLastError() const { return lastError; }

    private:
        void workerThread();
        void sendHeaderInternal();
        
        // Configuration
        ProtocolType protocolType;
        std::string hostname = "localhost";
        int port = 7355;
        bool useUDP = true;
        
        // Network connection
        net::Conn connection;
        net::Listener listener;
        mutable std::mutex connMtx;
        
        // Threading
        std::atomic<bool> running{false};
        std::atomic<bool> shouldStop{false};
        std::thread worker;
        
        // Data queue for thread-safe operation
        struct DataPacket {
            std::vector<uint8_t> data;
            std::chrono::time_point<std::chrono::high_resolution_clock> timestamp;
        };
        std::queue<DataPacket> dataQueue;
        std::mutex queueMtx;
        std::condition_variable queueCV;
        
        // Statistics
        std::atomic<uint64_t> bytesSent{0};
        std::atomic<uint64_t> packetsSent{0};
        std::string lastError;
        std::mutex errorMtx;
        
        // Header management
        bool headerSent = false;
        DigitalStreamHeader streamHeader;
        
        static constexpr size_t MAX_QUEUE_SIZE = 1000; // Prevent memory overflow
    };

    // Helper class to integrate with DSP sink system
    class DigitalNetworkSinkAdapter {
    public:
        DigitalNetworkSinkAdapter(ProtocolType protocolType);
        ~DigitalNetworkSinkAdapter();
        
        void init(dsp::stream<uint8_t>* input);
        void start();
        void stop();
        
        void setNetworkConfig(const std::string& hostname, int port, bool useUDP);
        
        DigitalNetworkSink* getSink() { return &networkSink; }
        
    private:
        static void dataHandler(uint8_t* data, int count, void* ctx);
        
        DigitalNetworkSink networkSink;
        dsp::sink::Handler<uint8_t> sinkHandler;
        bool initialized = false;
    };
}
