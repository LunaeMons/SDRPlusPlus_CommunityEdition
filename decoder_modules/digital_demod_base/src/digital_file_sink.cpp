#include "digital_file_sink.h"

namespace digital_demod {

    DigitalFileSink::DigitalFileSink(ProtocolType protocolType) 
        : protocolType(protocolType) {
    }

    DigitalFileSink::~DigitalFileSink() {
        stopRecording();
    }

    bool DigitalFileSink::startRecording(const std::string& filePath, const std::string& description) {
        std::lock_guard<std::mutex> lck(fileMtx);
        
        if (recording) {
            stopRecording();
        }

        try {
            // Ensure directory exists
            std::filesystem::path path(filePath);
            std::filesystem::create_directories(path.parent_path());

            // Open file for binary writing
            file.open(filePath, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!file.is_open()) {
                std::lock_guard<std::mutex> errLck(errorMtx);
                lastError = "Failed to open file: " + filePath;
                flog::error("Digital file recording failed: {}", lastError);
                return false;
            }

            currentFilePath = filePath;
            recordingStartTime = std::chrono::high_resolution_clock::now();
            
            // Initialize file header
            fileHeader = DigitalFileHeader{};
            fileHeader.protocol_id = static_cast<uint16_t>(protocolType);
            fileHeader.start_timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                recordingStartTime.time_since_epoch()).count();
            
            const auto* config = getProtocolConfig(protocolType);
            if (config) {
                fileHeader.symbol_rate = static_cast<uint16_t>(config->symbolRate);
                fileHeader.bits_per_symbol = config->bitsPerSymbol;
            }
            
            // Copy description (truncate if too long)
            strncpy(fileHeader.description, description.c_str(), sizeof(fileHeader.description) - 1);
            fileHeader.description[sizeof(fileHeader.description) - 1] = '\0';

            // Write initial header (sample_count will be updated later)
            headerPosition = file.tellp();
            writeHeader();

            // Reset statistics
            bytesWritten = 0;
            samplesWritten = 0;
            recording = true;

            flog::info("Digital file recording started: {} ({})", filePath, description);
            return true;

        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> errLck(errorMtx);
            lastError = std::string("Recording start exception: ") + e.what();
            flog::error("Digital file recording exception: {}", lastError);
            return false;
        }
    }

    void DigitalFileSink::stopRecording() {
        std::lock_guard<std::mutex> lck(fileMtx);
        
        if (!recording) return;

        try {
            // Update header with final sample count
            updateHeader();
            
            // Close file
            if (file.is_open()) {
                file.close();
            }

            recording = false;
            
            flog::info("Digital file recording stopped: {} ({} samples, {} bytes)", 
                      currentFilePath, samplesWritten.load(), bytesWritten.load());
            
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> errLck(errorMtx);
            lastError = std::string("Recording stop exception: ") + e.what();
            flog::error("Digital file recording stop error: {}", lastError);
        }
    }

    void DigitalFileSink::writeData(const uint8_t* data, int count) {
        if (!recording || count <= 0) return;

        std::lock_guard<std::mutex> lck(fileMtx);
        
        try {
            if (file.is_open()) {
                file.write(reinterpret_cast<const char*>(data), count);
                
                if (autoFlush) {
                    file.flush();
                }
                
                bytesWritten += count;
                samplesWritten += count;
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> errLck(errorMtx);
            lastError = std::string("Write error: ") + e.what();
            flog::error("Digital file write error: {}", lastError);
            
            // Stop recording on write error
            recording = false;
            if (file.is_open()) {
                file.close();
            }
        }
    }

    void DigitalFileSink::setProtocolType(ProtocolType type) {
        protocolType = type;
        fileHeader.protocol_id = static_cast<uint16_t>(type);
        
        const auto* config = getProtocolConfig(type);
        if (config) {
            fileHeader.symbol_rate = static_cast<uint16_t>(config->symbolRate);
            fileHeader.bits_per_symbol = config->bitsPerSymbol;
        }
    }

    std::string DigitalFileSink::getLastError() const {
        std::lock_guard<std::mutex> lck(errorMtx);
        return lastError;
    }

    void DigitalFileSink::writeHeader() {
        if (!file.is_open()) return;
        
        file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(DigitalFileHeader));
        if (autoFlush) {
            file.flush();
        }
    }

    void DigitalFileSink::updateHeader() {
        if (!file.is_open()) return;
        
        try {
            // Save current position
            auto currentPos = file.tellp();
            
            // Seek to header and update sample count
            file.seekp(headerPosition);
            fileHeader.sample_count = samplesWritten;
            file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(DigitalFileHeader));
            
            // Restore position
            file.seekp(currentPos);
            
            if (autoFlush) {
                file.flush();
            }
        } catch (const std::exception& e) {
            flog::error("Failed to update file header: {}", e.what());
        }
    }

    std::string DigitalFileSink::generateDefaultPath() const {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        char timestamp[64];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm);
        
        std::string protocolName = getProtocolName(protocolType);
        std::replace(protocolName.begin(), protocolName.end(), ' ', '_');
        
        return std::string("/tmp") + "/recordings/" + 
               protocolName + "_" + timestamp + ".digi";
    }

    // DigitalFileSinkAdapter implementation
    DigitalFileSinkAdapter::DigitalFileSinkAdapter(ProtocolType protocolType) 
        : fileSink(protocolType) {
    }

    DigitalFileSinkAdapter::~DigitalFileSinkAdapter() {
        stop();
    }

    void DigitalFileSinkAdapter::init(dsp::stream<uint8_t>* input) {
        if (initialized) return;
        
        sinkHandler.init(input, dataHandler, this);
        initialized = true;
    }

    void DigitalFileSinkAdapter::start() {
        if (!initialized) return;
        sinkHandler.start();
    }

    void DigitalFileSinkAdapter::stop() {
        if (!initialized) return;
        
        sinkHandler.stop();
        fileSink.stopRecording();
    }

    bool DigitalFileSinkAdapter::startRecording(const std::string& filePath, const std::string& description) {
        return fileSink.startRecording(filePath, description);
    }

    void DigitalFileSinkAdapter::stopRecording() {
        fileSink.stopRecording();
    }

    void DigitalFileSinkAdapter::dataHandler(uint8_t* data, int count, void* ctx) {
        DigitalFileSinkAdapter* _this = (DigitalFileSinkAdapter*)ctx;
        _this->fileSink.writeData(data, count);
    }
}
