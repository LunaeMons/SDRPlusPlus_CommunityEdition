#include "digital_demod_base.h"
#include <regex>
#include <filesystem>

namespace digital_demod {

    DigitalDemodulatorBase::DigitalDemodulatorBase(std::string name, ProtocolType protocolType, ConfigManager* config) 
        : symbolDiag(0.6, 480), name(name), protocolType(protocolType), configManager(config), folderSelect("%ROOT%/recordings") {
        
        // Get protocol configuration
        protocolConfig = getProtocolConfig(protocolType);
        if (!protocolConfig) {
            flog::error("Unknown protocol type: {}", (int)protocolType);
            return;
        }

        flog::info("Initializing digital demodulator: {} ({})", name, protocolConfig->name);

        // Load configuration
        loadConfig();

        // Initialize digital output sinks
        netSink = std::make_unique<DigitalNetworkSinkAdapter>(protocolType);
        fileSink = std::make_unique<DigitalFileSinkAdapter>(protocolType);

        // VFO will be created in enable() method

        // Initialize diagnostics
        diagReshape.init(nullptr, 480, 0);
        diagSink.init(&diagReshape.out, diagHandler, this);

        // Register menu
        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    DigitalDemodulatorBase::~DigitalDemodulatorBase() {
        // Signal that object is being destroyed
        destroying = true;
        
        // Ensure all operations are stopped before destruction
        if (enabled) {
            disable();
        }
        
        // Stop and cleanup sinks to prevent callbacks during destruction
        if (netSink) {
            netSink->stop();
            netSink.reset();
        }
        
        if (fileSink) {
            fileSink->getSink()->stopRecording();
            fileSink.reset();
        }
        
        gui::menu.removeEntry(name);
    }

    void DigitalDemodulatorBase::enable() {
        if (enabled) return;
        
        double bw = gui::waterfall.getBandwidth();
        double reqBw = getRequiredBandwidth();
        double reqSr = getRequiredSampleRate();
        
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 
                                           std::clamp<double>(0, -bw / 2.0, bw / 2.0), 
                                           reqBw, reqSr, reqBw, reqBw, true);

        // Initialize and start DSP chain
        flog::debug("Initializing DSP for {}", name);
        if (!initDSP()) {
            flog::error("Failed to initialize DSP for {}, disabling module", name);
            // Clean up VFO if initialization failed
            if (vfo) {
                sigpath::vfoManager.deleteVFO(vfo);
                vfo = nullptr;
            }
            return;
        }
        
        // Set VFO snap interval after protocolConfig is loaded
        if (protocolConfig) {
            vfo->setSnapInterval(protocolConfig->symbolRate / 10);
            flog::debug("Set VFO snap interval for {}", name);
        }
        
        flog::debug("Starting DSP for {}", name);
        startDSP();
        diagReshape.start();
        diagSink.start();

        enabled = true;
        flog::info("Digital demodulator enabled: {} ({})", name, protocolConfig ? protocolConfig->name : "unknown");
        
        // Auto-start network sink if enabled in config
        if (networkEnabled && netSink) {
            netSink->setNetworkConfig(networkHost, networkPort, useUDP);
            bool started = netSink->getSink()->start();
            if (!started) {
                flog::error("Failed to auto-start digital network sink: {}:{}", networkHost, networkPort);
                networkEnabled = false;  // Reset if auto-start failed
            } else {
                flog::info("Auto-started digital network output: {}:{} ({})", 
                          networkHost, networkPort, useUDP ? "UDP" : "TCP");
            }
        }
    }

    void DigitalDemodulatorBase::disable() {
        if (!enabled) return;
        
        stopDSP();
        diagReshape.stop();
        diagSink.stop();
        
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = nullptr;
        
        enabled = false;
        flog::info("Digital demodulator disabled: {}", name);
    }

    void DigitalDemodulatorBase::menuHandler(void* ctx) {
        DigitalDemodulatorBase* _this = (DigitalDemodulatorBase*)ctx;
        _this->showBaseMenu();
        _this->showProtocolMenu();
    }

    void DigitalDemodulatorBase::showBaseMenu() {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!enabled) { style::beginDisabled(); }

        // Protocol information
        ImGui::Text("Protocol: %s", protocolConfig->name.c_str());
        ImGui::Text("Symbol Rate: %.0f sym/s", protocolConfig->symbolRate);
        ImGui::Text("Bits/Symbol: %d", protocolConfig->bitsPerSymbol);
        
        ImGui::Separator();

        // Signal status
        {
            std::lock_guard<std::mutex> lck(statusMtx);
            ImGui::Text("Status: %s", isReceiving ? "Receiving" : "Idle");
            if (isReceiving) {
                ImGui::Text("Signal: %.1f dB", 20.0f * log10f(signalLevel + 1e-10f));
                ImGui::Text("Bits Received: %llu", bitsReceived);
            }
        }

        ImGui::Separator();

        // Symbol diagram
        ImGui::SetNextItemWidth(menuWidth);
        symbolDiag.draw();

        ImGui::Separator();

        // Network output configuration
        ImGui::Text("Digital Output");
        
        if (ImGui::Checkbox(CONCAT("Network Output##", name), &networkEnabled)) {
            flog::debug("Network output checkbox clicked: enabled={}, netSink valid={}", networkEnabled, netSink != nullptr);
            if (networkEnabled && netSink) {
                netSink->setNetworkConfig(networkHost, networkPort, useUDP);
                flog::debug("Setting network config: {}:{} ({})", networkHost, networkPort, useUDP ? "UDP" : "TCP");
                bool started = netSink->getSink()->start();
                if (!started) {
                    flog::error("Failed to start digital network sink: {}:{}", networkHost, networkPort);
                    networkEnabled = false;  // Reset checkbox if start failed
                }
                flog::info("Digital network output {}: {}:{} ({})", started ? "started" : "failed", 
                          networkHost, networkPort, useUDP ? "UDP" : "TCP");
            } else if (netSink) {
                netSink->getSink()->stop();
                flog::info("Digital network output stopped");
            } else {
                flog::error("Network output enabled but netSink is null!");
            }
            saveConfig();
        }

        if (networkEnabled) {
            ImGui::SetNextItemWidth(menuWidth * 0.7f);
            if (ImGui::InputText(CONCAT("Host##", name), networkHost, sizeof(networkHost))) {
                saveConfig();
            }
            
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth * 0.25f);
            if (ImGui::InputInt(CONCAT("Port##", name), &networkPort)) {
                networkPort = std::clamp(networkPort, 1024, 65535);
                saveConfig();
            }
            
            if (ImGui::Checkbox(CONCAT("UDP##", name), &useUDP)) {
                saveConfig();
            }
            ImGui::SameLine();
            ImGui::Text("(%s)", useUDP ? "UDP" : "TCP");
            
            // Connection status
            if (netSink && netSink->getSink()->isRunning()) {
                if (netSink->getSink()->isConnected()) {
                    ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
                } else {
                    ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Listening");
                }
                ImGui::SameLine();
                ImGui::Text("(Sent: %llu bytes)", netSink->getSink()->getBytesSent());
            } else {
                ImGui::TextUnformatted("Disconnected");
            }
        }

        // File recording section
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("File Recording");
        
        // Enable/disable file recording
        if (ImGui::Checkbox(CONCAT("Enable File Recording##", name), &fileRecordingEnabled)) {
            if (!fileRecordingEnabled && fileSink && fileSink->getSink()->isRecording()) {
                // Stop recording if disabled
                fileSink->getSink()->stopRecording();
                flog::info("File recording disabled and stopped");
            }
            saveConfig();
        }
        
        // Only show recording controls if enabled
        if (!fileRecordingEnabled) { style::beginDisabled(); }
        
        // Recording path selection
        ImGui::LeftLabel("Recording Path");
        if (folderSelect.render(CONCAT("##_digital_rec_path_", name))) {
            if (folderSelect.pathIsValid()) {
                saveConfig();
            }
        }
        
        // Name template
        ImGui::LeftLabel("Name Template");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_digital_name_template_", name), nameTemplate, sizeof(nameTemplate))) {
            saveConfig();
        }
        
        // Template help text
        ImGui::TextWrapped("Variables: $p=protocol, $t=module, $y=year, $M=month, $d=day, $h=hour, $m=minute, $s=second");
        
        // Preview filename
        if (protocolConfig) {
            std::string previewName = generateFileName(std::string(nameTemplate), protocolConfig->name) + ".digi";
            ImGui::Text("Preview: %s", previewName.c_str());
        }
        
        // Recording controls
        bool canRecord = folderSelect.pathIsValid();
        if (!fileSink || !fileSink->getSink()) { canRecord = false; }
        
        if (!fileSink->getSink()->isRecording()) {
            if (!canRecord) { style::beginDisabled(); }
            if (ImGui::Button(CONCAT("Start Recording##", name), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                if (protocolConfig) {
                    std::string fileName = generateFileName(std::string(nameTemplate), protocolConfig->name) + ".digi";
                    std::string fullPath = expandString(folderSelect.path + "/" + fileName);
                    
                    // Create directory if it doesn't exist
                    std::string dirPath = expandString(folderSelect.path);
                    if (!std::filesystem::exists(dirPath)) {
                        std::filesystem::create_directories(dirPath);
                    }
                    
                    if (fileSink->getSink()->startRecording(fullPath, protocolConfig->name)) {
                        flog::info("Started digital recording: {}", fullPath);
                        fileRecordingEnabled = true;
                        saveConfig();
                    } else {
                        flog::error("Failed to start digital recording: {}", fullPath);
                    }
                }
            }
            if (!canRecord) { style::endDisabled(); }
            
            if (fileSink->getSink()->isRecording()) {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Idle");
            }
        } else {
            if (ImGui::Button(CONCAT("Stop Recording##", name), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                fileSink->getSink()->stopRecording();
                fileRecordingEnabled = false;
                saveConfig();
                flog::info("Stopped digital recording");
            }
            
            // Show recording status
            uint64_t samples = fileSink->getSink()->getSamplesWritten();
            uint64_t bytes = fileSink->getSink()->getBytesWritten();
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Recording: %llu samples (%llu bytes)", samples, bytes);
            
            // Show current file
            std::string currentFile = fileSink->getSink()->getCurrentFilePath();
            if (!currentFile.empty()) {
                // Show just the filename, not the full path
                std::string fileName = std::filesystem::path(currentFile).filename().string();
                ImGui::Text("File: %s", fileName.c_str());
            }
        }
        
        // End file recording disabled style
        if (!fileRecordingEnabled) { style::endDisabled(); }

        if (!enabled) { style::endDisabled(); }
    }

    void DigitalDemodulatorBase::digitalStreamHandler(uint8_t* data, int count, void* ctx) {
        DigitalDemodulatorBase* _this = (DigitalDemodulatorBase*)ctx;
        
        // Safety check: ensure object is still valid and not being destroyed
        if (!_this || !_this->enabled || _this->destroying) {
            return;
        }
        
        // Debug: Log first few symbols to see what we're getting
        if (count > 0) {
            std::string symbols_hex;
            for (int i = 0; i < std::min(8, count); i++) {
                char hex_byte[4];
                sprintf(hex_byte, "%02X ", data[i]);
                symbols_hex += hex_byte;
            }
            flog::debug("Digital symbols received: {} bytes, first symbols: {}", count, symbols_hex);
        }
        
        // Set a reasonable signal level (TODO: implement proper signal level calculation)
        float currentSignalLevel = 0.1f;  // Corresponds to about -10 dB
        
        // Update statistics
        {
            std::lock_guard<std::mutex> lck(_this->statusMtx);
            _this->bitsReceived += count;
            _this->lastActivity = std::chrono::high_resolution_clock::now();
            _this->isReceiving = true;
            _this->signalLevel = currentSignalLevel;  // Update signal level
        }

        // Send to network sink if enabled
        if (_this->networkEnabled && _this->netSink) {
            _this->netSink->getSink()->sendData(data, count);
        }

        // Send to file sink if enabled  
        if (_this->fileRecordingEnabled && _this->fileSink) {
            _this->fileSink->getSink()->writeData(data, count);
        }
    }

    void DigitalDemodulatorBase::diagHandler(float* data, int count, void* ctx) {
        DigitalDemodulatorBase* _this = (DigitalDemodulatorBase*)ctx;
        float* buf = _this->symbolDiag.acquireBuffer();
        memcpy(buf, data, count * sizeof(float));
        _this->symbolDiag.releaseBuffer();
    }

    void DigitalDemodulatorBase::loadConfig() {
        if (!configManager) return;
        
        configManager->acquire();
        if (!configManager->conf.contains(name)) {
            // Set defaults
            configManager->conf[name]["networkEnabled"] = false;
            configManager->conf[name]["fileRecordingEnabled"] = false;
            configManager->conf[name]["networkHost"] = "localhost";
            configManager->conf[name]["networkPort"] = 7356;  // Different port from audio network sink
            configManager->conf[name]["useUDP"] = true;
            configManager->conf[name]["recordingPath"] = "%ROOT%/recordings";
            configManager->conf[name]["nameTemplate"] = "$p_$t_$d-$M-$y_$h-$m-$s";
        }
        
        networkEnabled = configManager->conf[name]["networkEnabled"];
        fileRecordingEnabled = configManager->conf[name]["fileRecordingEnabled"];
        std::string host = configManager->conf[name]["networkHost"];
        strncpy(networkHost, host.c_str(), sizeof(networkHost) - 1);
        networkHost[sizeof(networkHost) - 1] = '\0';
        networkPort = configManager->conf[name]["networkPort"];
        useUDP = configManager->conf[name]["useUDP"];
        
        // Load file recording configuration
        if (configManager->conf[name].contains("recordingPath")) {
            folderSelect.setPath(configManager->conf[name]["recordingPath"]);
        }
        if (configManager->conf[name].contains("nameTemplate")) {
            std::string nameTemplateStr = configManager->conf[name]["nameTemplate"];
            strncpy(nameTemplate, nameTemplateStr.c_str(), sizeof(nameTemplate) - 1);
            nameTemplate[sizeof(nameTemplate) - 1] = '\0';
        }
        
        // Note: Network auto-start will happen in enable() method if networkEnabled is true
        
        configManager->release();
    }

    void DigitalDemodulatorBase::saveConfig() {
        if (!configManager) return;
        
        configManager->acquire();
        configManager->conf[name]["networkEnabled"] = networkEnabled;
        configManager->conf[name]["fileRecordingEnabled"] = fileRecordingEnabled;
        configManager->conf[name]["networkHost"] = std::string(networkHost);
        configManager->conf[name]["networkPort"] = networkPort;
        configManager->conf[name]["useUDP"] = useUDP;
        configManager->conf[name]["recordingPath"] = folderSelect.path;
        configManager->conf[name]["nameTemplate"] = std::string(nameTemplate);
        configManager->release(true);
    }

    void DigitalDemodulatorBase::updateStatus(bool receiving, float sigLevel, float noiseLevel) {
        std::lock_guard<std::mutex> lck(statusMtx);
        isReceiving = receiving;
        signalLevel = sigLevel;
        this->noiseLevel = noiseLevel;
        if (receiving) {
            lastActivity = std::chrono::high_resolution_clock::now();
        }
    }
    
    std::string DigitalDemodulatorBase::generateFileName(const std::string& nameTemplate, const std::string& protocolName) const {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        std::string result = nameTemplate;
        
        // Replace template variables
        result = std::regex_replace(result, std::regex("\\$p"), protocolName);  // Protocol name
        result = std::regex_replace(result, std::regex("\\$t"), name);         // Module instance name
        result = std::regex_replace(result, std::regex("\\$y"), std::to_string(tm.tm_year + 1900)); // Year
        result = std::regex_replace(result, std::regex("\\$M"), std::to_string(tm.tm_mon + 1));     // Month
        result = std::regex_replace(result, std::regex("\\$d"), std::to_string(tm.tm_mday));        // Day
        result = std::regex_replace(result, std::regex("\\$h"), std::to_string(tm.tm_hour));        // Hour
        result = std::regex_replace(result, std::regex("\\$m"), std::to_string(tm.tm_min));         // Minute
        result = std::regex_replace(result, std::regex("\\$s"), std::to_string(tm.tm_sec));         // Second
        
        return result;
    }
    
    std::string DigitalDemodulatorBase::expandString(const std::string& input) const {
        std::string result = std::regex_replace(input, std::regex("%ROOT%"), (std::string)core::args["root"]);
        return std::regex_replace(result, std::regex("//"), "/");
    }
}
