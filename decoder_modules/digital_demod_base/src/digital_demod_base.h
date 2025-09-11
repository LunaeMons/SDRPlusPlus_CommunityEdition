#pragma once
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <dsp/stream.h>
#include <dsp/demod/gfsk.h>
#include <dsp/demod/psk.h>
#include <dsp/digital/binary_slicer.h>
#include <dsp/digital/quaternary_slicer.h>
#include <dsp/digital/differential_decoder.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/buffer/reshaper.h>
#include <gui/widgets/symbol_diagram.h>
#include <gui/widgets/folder_select.h>
#include <utils/flog.h>
#include <chrono>
#include <mutex>
#include <atomic>
#include <fstream>
#include "protocol_types.h"
#include "digital_network_sink.h"
#include "digital_file_sink.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// Forward declaration for config
extern ConfigManager config;

namespace digital_demod {

    // Base class for all digital demodulator modules
    class DigitalDemodulatorBase : public ModuleManager::Instance {
    public:
        DigitalDemodulatorBase(std::string name, ProtocolType protocolType, ConfigManager* config);
        virtual ~DigitalDemodulatorBase();

        // ModuleManager::Instance interface
        void postInit() override {}
        void enable() override;
        void disable() override;
        bool isEnabled() override { return enabled; }

    protected:
        // Virtual methods for protocol-specific implementation
        virtual bool initDSP() = 0;  // Returns true on success, false on failure
        virtual void startDSP() = 0;
        virtual void stopDSP() = 0;
        virtual void showProtocolMenu() = 0;
        virtual double getRequiredBandwidth() = 0;
        virtual double getRequiredSampleRate() = 0;

        // Common UI and configuration
        static void menuHandler(void* ctx);
        void showBaseMenu();
        
        // Digital stream handlers
        static void digitalStreamHandler(uint8_t* data, int count, void* ctx);
        static void diagHandler(float* data, int count, void* ctx);

        // Configuration management
        void loadConfig();
        void saveConfig();

        // Common DSP components
        VFOManager::VFO* vfo = nullptr;
        dsp::buffer::Reshaper<float> diagReshape;
        dsp::sink::Handler<float> diagSink;
        dsp::sink::Handler<uint8_t> digitalSink;

        // UI components
        ImGui::SymbolDiagram symbolDiag;
        
        // Configuration
        std::string name;
        ProtocolType protocolType;
        const ProtocolConfig* protocolConfig;
        ConfigManager* configManager;
        bool enabled = false;
        std::atomic<bool> destroying{false};
        
        // Digital output configuration
        bool networkEnabled = false;
        bool fileRecordingEnabled = false;
        char networkHost[256] = "localhost";
        int networkPort = 7356;  // Different port from audio network sink
        bool useUDP = true;
        
        // File recording configuration
        FolderSelect folderSelect;
        char nameTemplate[1024] = "$p_$t_$d-$M-$y_$h-$m-$s";  // protocol_name_DD-MM-YYYY_HH-MM-SS
        
        // Status tracking
        bool isReceiving = false;
        uint64_t bitsReceived = 0;
        float signalLevel = 0.0f;
        float noiseLevel = 0.0f;
        std::chrono::time_point<std::chrono::high_resolution_clock> lastActivity;
        
        // Thread safety
        std::mutex statusMtx;
        
        // Digital output sinks
        std::unique_ptr<DigitalNetworkSinkAdapter> netSink;
        std::unique_ptr<DigitalFileSinkAdapter> fileSink;

    private:
        void updateStatus(bool receiving, float sigLevel = 0.0f, float noiseLevel = 0.0f);
        std::string generateFileName(const std::string& nameTemplate, const std::string& protocolName) const;
        std::string expandString(const std::string& input) const;
    };


}
