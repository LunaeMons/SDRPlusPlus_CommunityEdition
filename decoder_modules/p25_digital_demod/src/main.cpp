#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/flog.h>
#include "digital_demod_base.h"
#include "protocol_types.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "p25_digital_demod",
    /* Description:     */ "P25 Digital Demodulator for SDR++",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// P25 protocol variants
enum P25Mode {
    P25_MODE_FSK4 = 0,
    P25_MODE_CQPSK_4800 = 1,
    P25_MODE_CQPSK_6000 = 2,
    P25_MODE_H_DQPSK = 3,
    P25_MODE_H_CPM = 4
};

const char* p25ModeNames[] = {
    "P25 FSK4",
    "P25 CQPSK 4800",
    "P25 CQPSK 6000", 
    "P25 H-DQPSK",
    "P25 H-CPM"
};

const digital_demod::ProtocolType p25ProtocolTypes[] = {
    digital_demod::ProtocolType::P25_FSK4,
    digital_demod::ProtocolType::P25_CQPSK_4800,
    digital_demod::ProtocolType::P25_CQPSK_6000,
    digital_demod::ProtocolType::P25_H_DQPSK,
    digital_demod::ProtocolType::P25_H_CPM
};

class P25DigitalDemodulator : public digital_demod::DigitalDemodulatorBase {
public:
    P25DigitalDemodulator(std::string name) 
        : DigitalDemodulatorBase(name, digital_demod::ProtocolType::P25_FSK4, &config) {
        
        this->instanceName = name;
        
        // Initialize P25-specific configuration with defaults (no config reading in constructor)
        p25Mode = P25_MODE_FSK4;
        showConstellation = true;
        adaptiveThreshold = true;

        // Note: updateProtocolType() will be called in initDSP() after full initialization
        
        flog::info("P25 Digital Demodulator initialized: {} (Mode: {})", name, p25ModeNames[p25Mode]);
    }

    ~P25DigitalDemodulator() {
        flog::info("P25 Digital Demodulator destroyed: {}", instanceName);
    }

protected:
    void loadP25Config() {
        // Load P25-specific configuration safely
        config.acquire();
        try {
            if (!config.conf.contains(instanceName)) {
                config.conf[instanceName] = json::object();
            }
            
            // Set defaults if not present
            if (!config.conf[instanceName].contains("p25Mode")) {
                config.conf[instanceName]["p25Mode"] = P25_MODE_FSK4;
            }
            if (!config.conf[instanceName].contains("showConstellation")) {
                config.conf[instanceName]["showConstellation"] = true;
            }
            if (!config.conf[instanceName].contains("adaptiveThreshold")) {
                config.conf[instanceName]["adaptiveThreshold"] = true;
            }
            
            // Read values safely
            if (config.conf[instanceName]["p25Mode"].is_number_integer()) {
                p25Mode = config.conf[instanceName]["p25Mode"];
            }
            if (config.conf[instanceName]["showConstellation"].is_boolean()) {
                showConstellation = config.conf[instanceName]["showConstellation"];
            }
            if (config.conf[instanceName]["adaptiveThreshold"].is_boolean()) {
                adaptiveThreshold = config.conf[instanceName]["adaptiveThreshold"];
            }
        } catch (const std::exception& e) {
            flog::error("P25 config error: {}, using defaults", e.what());
        }
        config.release(true);
        // Note: updateProtocolType() will be called in initDSP()
    }

    bool initDSP() override {
        // Load configuration before initializing DSP
        loadP25Config();
        
        // Update protocol type based on loaded configuration
        updateProtocolType();
        
        // Verify protocol config is valid
        if (!protocolConfig) {
            flog::error("P25 protocol config is null, cannot initialize DSP");
            return false;
        }
        
        double sampleRate = getRequiredSampleRate();
        double symbolRate = protocolConfig->symbolRate;
        
        // Check if this is a supported mode for current implementation
        if (protocolConfig->deviation == 0) {
            flog::error("CQPSK/PSK modes not yet implemented, only FSK modes supported");
            return false;
        }
        
        // Initialize GFSK demodulator for P25 FSK4
        gfskDemod.init(vfo->output, symbolRate, sampleRate, protocolConfig->deviation, 
                      protocolConfig->rrcTaps, protocolConfig->rrcBeta, 1e-6f, 0.01f, 0.01f);
        
        // Initialize quaternary slicer for FSK4
        quaternarySlicer.init(&gfskDemod.out, threshold1, threshold2);
        
        // Initialize digital sink
        digitalSink.init(&quaternarySlicer.out, digitalStreamHandler, this);
        
        // Initialize diagnostics
        diagReshape.setInput(&gfskDemod.out);
        
        // Output adapters are handled by digitalStreamHandler in the base class
        // No direct initialization needed here
        
        return true;  // Initialization successful
    }
    
    void startDSP() override {
        gfskDemod.start();
        quaternarySlicer.start();
        digitalSink.start();
    }
    
    void stopDSP() override {
        digitalSink.stop();
        quaternarySlicer.stop();
        gfskDemod.stop();
    }
    
    double getRequiredBandwidth() override {
        return protocolConfig ? protocolConfig->bandwidth : 9600.0;
    }
    
    double getRequiredSampleRate() override {
        return protocolConfig ? (protocolConfig->symbolRate * 3.0) : 14400.0;
    }

    void showProtocolMenu() override {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // P25 Mode selection
        ImGui::Text("P25 Configuration");
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "Note: Only FSK modes supported currently");
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(CONCAT("Mode##", instanceName), &p25Mode, p25ModeNames, IM_ARRAYSIZE(p25ModeNames))) {
            // Save configuration first
            config.acquire();
            config.conf[instanceName]["p25Mode"] = p25Mode;
            config.release(true);
            
            // Update protocol type
            updateProtocolType();
            
            // Restart DSP with new parameters if enabled
            if (enabled) {
                try {
                    stopDSP();
                    
                    // Update VFO parameters for new mode
                    if (vfo && protocolConfig) {
                        vfo->setBandwidth(protocolConfig->bandwidth);
                        vfo->setSampleRate(protocolConfig->symbolRate * 3.0, protocolConfig->bandwidth);
                        vfo->setSnapInterval(protocolConfig->symbolRate / 10);
                    }
                    
                    initDSP();
                    startDSP();
                    flog::info("P25 mode changed to: {}", p25ModeNames[p25Mode]);
                } catch (const std::exception& e) {
                    flog::error("Error restarting DSP after mode change: {}", e.what());
                    // Try to disable and re-enable to recover
                    disable();
                    enable();
                }
            }
        }

        ImGui::Separator();

        // Display current protocol parameters
        if (protocolConfig) {
            ImGui::Text("Symbol Rate: %.0f sym/s", protocolConfig->symbolRate);
            ImGui::Text("Deviation: %.0f Hz", protocolConfig->deviation);
            ImGui::Text("Bandwidth: %.0f Hz", protocolConfig->bandwidth);
            ImGui::Text("Bits/Symbol: %d", protocolConfig->bitsPerSymbol);
        }

        ImGui::Separator();

        // Advanced options
        if (ImGui::Checkbox(CONCAT("Show Constellation##", instanceName), &showConstellation)) {
            config.acquire();
            config.conf[instanceName]["showConstellation"] = showConstellation;
            config.release(true);
        }

        if (ImGui::Checkbox(CONCAT("Adaptive Threshold##", instanceName), &adaptiveThreshold)) {
            config.acquire();
            config.conf[instanceName]["adaptiveThreshold"] = adaptiveThreshold;
            config.release(true);
        }

        // Signal quality indicators
        ImGui::Separator();
        ImGui::Text("Signal Quality");
        
        {
            std::lock_guard<std::mutex> lck(statusMtx);
            if (isReceiving) {
                float snr = signalLevel / (noiseLevel + 1e-10f);
                ImGui::Text("SNR: %.1f dB", 10.0f * log10f(snr + 1e-10f));
                
                // Simple signal quality bar
                float quality = std::clamp(snr / 100.0f, 0.0f, 1.0f); // Normalize to 0-1
                ImVec4 color = (quality > 0.7f) ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) :
                              (quality > 0.3f) ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) :
                                                 ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                
                ImGui::ProgressBar(quality, ImVec2(menuWidth, 0), "");
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::TextColored(color, "%.0f%%", quality * 100.0f);
            } else {
                ImGui::Text("SNR: -- dB");
                ImGui::ProgressBar(0.0f, ImVec2(menuWidth, 0), "No Signal");
            }
        }
    }

private:
    // P25-specific DSP chain
    dsp::demod::GFSK gfskDemod;
    dsp::digital::QuaternarySlicer quaternarySlicer;
    
    void updateProtocolType() {
        try {
            // Bounds check for p25Mode
            if (p25Mode < 0 || p25Mode >= static_cast<int>(sizeof(p25ProtocolTypes) / sizeof(p25ProtocolTypes[0]))) {
                flog::error("Invalid P25 mode index: {}, resetting to FSK4", p25Mode);
                p25Mode = P25_MODE_FSK4;
            }
            
            protocolType = p25ProtocolTypes[p25Mode];
            protocolConfig = digital_demod::getProtocolConfig(protocolType);
            
            // Update network and file sinks safely
            if (netSink && netSink->getSink()) {
                netSink->getSink()->setProtocolType(protocolType);
            }
            if (fileSink && fileSink->getSink()) {
                fileSink->getSink()->setProtocolType(protocolType);
            }
            
            flog::info("Protocol type updated to: {}", protocolConfig ? protocolConfig->name : "Unknown");
        } catch (const std::exception& e) {
            flog::error("Error updating protocol type: {}", e.what());
        }
    }

    std::string instanceName;
    int p25Mode = P25_MODE_FSK4;
    bool showConstellation = true;
    bool adaptiveThreshold = true;
    
    // FSK4 slicer thresholds
    float threshold1 = -0.5f;
    float threshold2 = 0.5f;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/p25_digital_demod_config.json");
    config.load(def);
    config.enableAutoSave();
    
    flog::info("P25 Digital Demodulator module initialized");
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new P25DigitalDemodulator(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (P25DigitalDemodulator*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
    flog::info("P25 Digital Demodulator module terminated");
}
