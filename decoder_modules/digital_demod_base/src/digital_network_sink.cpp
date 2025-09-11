#include "digital_network_sink.h"

namespace digital_demod {

    DigitalNetworkSink::DigitalNetworkSink(ProtocolType protocolType) 
        : protocolType(protocolType) {
        
        // Initialize stream header
        streamHeader.protocol_id = static_cast<uint16_t>(protocolType);
        const auto* config = getProtocolConfig(protocolType);
        if (config) {
            streamHeader.symbol_rate = static_cast<uint16_t>(config->symbolRate);
            streamHeader.bits_per_symbol = config->bitsPerSymbol;
        }
    }

    DigitalNetworkSink::~DigitalNetworkSink() {
        stop();
    }

    void DigitalNetworkSink::setNetworkConfig(const std::string& hostname, int port, bool useUDP) {
        std::lock_guard<std::mutex> lck(connMtx);
        this->hostname = hostname;
        this->port = port;
        this->useUDP = useUDP;
    }

    void DigitalNetworkSink::setProtocolType(ProtocolType type) {
        protocolType = type;
        streamHeader.protocol_id = static_cast<uint16_t>(type);
        const auto* config = getProtocolConfig(type);
        if (config) {
            streamHeader.symbol_rate = static_cast<uint16_t>(config->symbolRate);
            streamHeader.bits_per_symbol = config->bitsPerSymbol;
        }
        headerSent = false; // Force header resend
    }

    bool DigitalNetworkSink::start() {
        if (running) return true;

        shouldStop = false;
        headerSent = false;
        
        try {
            worker = std::thread(&DigitalNetworkSink::workerThread, this);
            running = true;
            flog::info("Digital network sink started: {}:{} ({})", hostname, port, useUDP ? "UDP" : "TCP");
            return true;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lck(errorMtx);
            lastError = std::string("Failed to start worker thread: ") + e.what();
            flog::error("Digital network sink start failed: {}", lastError);
            return false;
        }
    }

    void DigitalNetworkSink::stop() {
        if (!running) return;
        
        shouldStop = true;
        queueCV.notify_all();
        
        if (worker.joinable()) {
            worker.join();
        }
        
        {
            std::lock_guard<std::mutex> lck(connMtx);
            if (connection) connection->close();
            if (listener) listener->close();
        }
        
        running = false;
        flog::info("Digital network sink stopped");
    }

    bool DigitalNetworkSink::isConnected() const {
        std::lock_guard<std::mutex> lck(connMtx);
        return connection && connection->isOpen();
    }

    void DigitalNetworkSink::sendData(const uint8_t* data, int count) {
        if (!running || count <= 0) return;

        // Create data packet with timestamp
        DataPacket packet;
        packet.data.assign(data, data + count);
        packet.timestamp = std::chrono::high_resolution_clock::now();

        // Add to queue (thread-safe)
        {
            std::lock_guard<std::mutex> lck(queueMtx);
            if (dataQueue.size() >= MAX_QUEUE_SIZE) {
                // Drop oldest packet to prevent memory overflow
                dataQueue.pop();
                flog::warn("Digital network sink queue overflow, dropping packet");
            }
            dataQueue.push(std::move(packet));
        }
        
        queueCV.notify_one();
    }

    void DigitalNetworkSink::sendHeader() {
        // For UDP, always allow header resending; for TCP, only send once
        if (headerSent && !useUDP) return;
        
        streamHeader.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        sendHeaderInternal();
        if (!useUDP) headerSent = true;  // Only mark as sent for TCP
    }

    void DigitalNetworkSink::workerThread() {
        flog::info("Digital network sink worker thread started");
        
        while (!shouldStop) {
            try {
                // Establish connection
                {
                    std::lock_guard<std::mutex> lck(connMtx);
                    if (!connection || !connection->isOpen()) {
                        if (useUDP) {
                            connection = net::openUDP("0.0.0.0", port, hostname, port, false);
                        } else {
                            listener = net::listen("0.0.0.0", port);
                            if (listener) {
                                flog::info("Waiting for TCP connection on port {}", port);
                                connection = listener->accept(); // Blocking call
                            }
                        }
                        
                        if (connection && connection->isOpen()) {
                            flog::info("Digital network connection established: {}:{}", hostname, port);
                            headerSent = false; // Send header on new connection
                        }
                    }
                }

                // Process data queue
                std::unique_lock<std::mutex> queueLock(queueMtx);
                queueCV.wait(queueLock, [this] { return !dataQueue.empty() || shouldStop; });
                
                while (!dataQueue.empty() && !shouldStop) {
                    DataPacket packet = std::move(dataQueue.front());
                    dataQueue.pop();
                    queueLock.unlock();

                    // Send header if needed (for TCP) or periodically (for UDP)
                    if (!headerSent || (useUDP && packetsSent % 100 == 0)) {
                        sendHeader();
                        if (!useUDP) headerSent = true;  // Only mark as sent for TCP
                    }

                    // Send data
                    {
                        std::lock_guard<std::mutex> lck(connMtx);
                        if (connection && connection->isOpen()) {
                            try {
                                connection->write(packet.data.size(), packet.data.data());
                                bytesSent += packet.data.size();
                                packetsSent++;
                            } catch (const std::exception& e) {
                                std::lock_guard<std::mutex> errLck(errorMtx);
                                lastError = std::string("Send failed: ") + e.what();
                                flog::error("Digital network send error: {}", lastError);
                                connection->close(); // Force reconnection
                            }
                        }
                    }

                    queueLock.lock();
                }
                
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> errLck(errorMtx);
                lastError = std::string("Worker thread error: ") + e.what();
                flog::error("Digital network worker error: {}", lastError);
                
                // Wait before retrying
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        
        flog::info("Digital network sink worker thread stopped");
    }

    void DigitalNetworkSink::sendHeaderInternal() {
        std::lock_guard<std::mutex> lck(connMtx);
        if (connection && connection->isOpen()) {
            try {
                // Debug: Log header bytes
                uint8_t* headerBytes = reinterpret_cast<uint8_t*>(&streamHeader);
                std::string headerHex;
                for (int i = 0; i < sizeof(DigitalStreamHeader); i++) {
                    char hex_byte[4];
                    snprintf(hex_byte, sizeof(hex_byte), "%02X ", headerBytes[i]);
                    headerHex += hex_byte;
                }
                flog::debug("Sending header bytes: {}", headerHex);
                
                connection->write(sizeof(DigitalStreamHeader), const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&streamHeader)));
                flog::info("Digital stream header sent: protocol={}, symbol_rate={}, bits_per_symbol={}", 
                          streamHeader.protocol_id, streamHeader.symbol_rate, streamHeader.bits_per_symbol);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> errLck(errorMtx);
                lastError = std::string("Header send failed: ") + e.what();
                flog::error("Digital network header send error: {}", lastError);
            }
        }
    }

    // DigitalNetworkSinkAdapter implementation
    DigitalNetworkSinkAdapter::DigitalNetworkSinkAdapter(ProtocolType protocolType) 
        : networkSink(protocolType) {
    }

    DigitalNetworkSinkAdapter::~DigitalNetworkSinkAdapter() {
        stop();
    }

    void DigitalNetworkSinkAdapter::init(dsp::stream<uint8_t>* input) {
        if (initialized) return;
        
        sinkHandler.init(input, dataHandler, this);
        initialized = true;
    }

    void DigitalNetworkSinkAdapter::start() {
        if (!initialized) return;
        
        networkSink.start();
        sinkHandler.start();
    }

    void DigitalNetworkSinkAdapter::stop() {
        if (!initialized) return;
        
        sinkHandler.stop();
        networkSink.stop();
    }

    void DigitalNetworkSinkAdapter::setNetworkConfig(const std::string& hostname, int port, bool useUDP) {
        networkSink.setNetworkConfig(hostname, port, useUDP);
    }

    void DigitalNetworkSinkAdapter::dataHandler(uint8_t* data, int count, void* ctx) {
        DigitalNetworkSinkAdapter* _this = (DigitalNetworkSinkAdapter*)ctx;
        _this->networkSink.sendData(data, count);
    }
}
