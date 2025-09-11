#pragma once
#include <fstream>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include "protocol_types.h"

namespace digital_demod {

    class DigitalFileSink {
    public:
        DigitalFileSink(ProtocolType protocolType);
        ~DigitalFileSink();

        // File operations
        bool startRecording(const std::string& filePath, const std::string& description = "");
        void stopRecording();
        bool isRecording() const { return recording; }
        
        // Data writing
        void writeData(const uint8_t* data, int count);
        
        // Configuration
        void setProtocolType(ProtocolType type);
        void setAutoFlush(bool autoFlush) { this->autoFlush = autoFlush; }
        
        // Statistics
        uint64_t getBytesWritten() const { return bytesWritten; }
        uint64_t getSamplesWritten() const { return samplesWritten; }
        std::string getCurrentFilePath() const { return currentFilePath; }
        std::string getLastError() const;

    private:
        void writeHeader();
        void updateHeader(); // Update sample count in header
        std::string generateDefaultPath() const;
        
        // Configuration
        ProtocolType protocolType;
        bool autoFlush = true;
        
        // File handling
        std::ofstream file;
        std::string currentFilePath;
        std::atomic<bool> recording{false};
        std::mutex fileMtx;
        
        // Statistics
        std::atomic<uint64_t> bytesWritten{0};
        std::atomic<uint64_t> samplesWritten{0};
        std::chrono::time_point<std::chrono::high_resolution_clock> recordingStartTime;
        
        // Error handling
        mutable std::mutex errorMtx;
        std::string lastError;
        
        // File format
        DigitalFileHeader fileHeader;
        std::streampos headerPosition;
    };

    // Helper class to integrate with DSP sink system
    class DigitalFileSinkAdapter {
    public:
        DigitalFileSinkAdapter(ProtocolType protocolType);
        ~DigitalFileSinkAdapter();
        
        void init(dsp::stream<uint8_t>* input);
        void start();
        void stop();
        
        bool startRecording(const std::string& filePath, const std::string& description = "");
        void stopRecording();
        
        DigitalFileSink* getSink() { return &fileSink; }
        
    private:
        static void dataHandler(uint8_t* data, int count, void* ctx);
        
        DigitalFileSink fileSink;
        dsp::sink::Handler<uint8_t> sinkHandler;
        bool initialized = false;
    };
}
