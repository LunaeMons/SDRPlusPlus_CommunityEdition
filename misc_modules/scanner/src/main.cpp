#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <fstream> // Added for file operations
#include <core.h>
#include <radio_interface.h>
#include <sstream>
#include <cstring>  // For memcpy
#include <set>      // For std::set in profile diagnostics
#include <cstdint>  // For uintptr_t
#include "scanner_log.h" // Custom logging macros
#include "../Logger.hpp"
#include <gui/widgets/precision_slider.h>
#include <gui/widgets/folder_select.h>
#include <gui/file_dialogs.h>
#include <filesystem>
#include <regex>
#include "../../recorder/src/recorder_interface.h"

// Windows MSVC compatibility 
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif



// Frequency range structure for multiple scanning ranges
struct FrequencyRange {
    std::string name;
    double startFreq;
    double stopFreq;
    bool enabled;
    float gain;  // Gain setting for this frequency range (in dB)
    
    FrequencyRange() : name("New Range"), startFreq(88000000.0), stopFreq(108000000.0), enabled(true), gain(20.0f) {}
    FrequencyRange(const std::string& n, double start, double stop, bool en = true, float g = 20.0f) 
        : name(n), startFreq(start), stopFreq(stop), enabled(en), gain(g) {}
};

SDRPP_MOD_INFO{
    /* Name:            */ "scanner",
    /* Description:     */ "Frequency scanner for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

// Forward declarations for frequency manager integration
class FrequencyManagerModule;

// CRITICAL: Use frequency manager's real FrequencyBookmark struct (must match exactly!)
struct FrequencyBookmark {
    // Original fields (backward compatibility)
    double frequency;
    double bandwidth;
    int mode;
    bool selected;
    
    // NEW: Band support (performance-optimized layout)
    bool isBand = false;                    // Flag: true = band, false = single frequency
    double startFreq = 0.0;                 // For bands: start frequency  
    double endFreq = 0.0;                   // For bands: end frequency
    double stepFreq = 100000.0;             // For bands: scanning step (default 100kHz)
    std::string notes;                      // User notes/description
    std::vector<std::string> tags;          // Tags for categorization (pre-allocated for performance)
    
    // NOTE: Other fields may exist but are not accessed by scanner
    // This struct MUST match frequency_manager's FrequencyBookmark exactly
};

// CRITICAL: Use frequency manager's real TuningProfile struct (no local copy!)
// Forward declaration only - real definition in frequency_manager module
struct TuningProfile {
    // INTERFACE CONTRACT: This must match frequency_manager's TuningProfile exactly
    // Fields are accessed via interface, not direct member access
    int demodMode;
    float bandwidth;
    bool squelchEnabled;
    float squelchLevel;
    int deemphasisMode;
    bool agcEnabled;
    float rfGain;
    double centerOffset;
    std::string name;
    bool autoApply;
    
    // SAFETY: Only access this struct through frequency manager interface
    // Direct field access is UNSAFE due to potential ABI differences
};

// Scanner module interface commands
#define SCANNER_IFACE_CMD_GET_RUNNING   0
#define SCANNER_IFACE_CMD_START         1
#define SCANNER_IFACE_CMD_STOP          2
#define SCANNER_IFACE_CMD_RESET         3
#define SCANNER_IFACE_CMD_GET_STATUS    4
#define SCANNER_IFACE_CMD_PREV_FREQ     5
#define SCANNER_IFACE_CMD_NEXT_FREQ     6
#define SCANNER_IFACE_CMD_BLACKLIST     7

class ScannerModule : public ModuleManager::Instance {
public:
    ScannerModule(std::string name) : autoRecordFolderSelect("%ROOT%/scanner_recordings") {
        this->name = name;
        
        // Initialize time points to current time to prevent crashes
        auto now = std::chrono::high_resolution_clock::now();
        lastSignalTime = now;
        lastTuneTime = now;
        
        // Ensure scanner starts in a safe state
        running = false;
        tuning = false;
        receiving = false;
        
        flog::info("Scanner: Initializing scanner module '{}'", name);
        
        // Set up FFT redraw handler for trigger level visualization
        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        loadConfig();
        
        // Bind FFT redraw handler for trigger level line
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        
        // Check for midnight reset on module initialization
        checkMidnightReset();
        
        // Register scanner interface for external communication
        core::modComManager.registerInterface("scanner", name, scannerInterfaceHandler, this);
        
        flog::info("Scanner: Scanner module '{}' initialized successfully", name);
    }

    ~ScannerModule() {
        flog::info("Scanner: Destructor called, cleaning up");
        
        // Unbind FFT redraw handler
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        
        // Stop scanner and set running to false
        running = false;
        
        // Clean up worker thread with timeout
        if (workerThread.joinable()) {
            flog::info("Scanner: Cleaning up worker thread in destructor");
            try {
                auto future = std::async(std::launch::async, [this]() {
                    workerThread.join();
                });
                if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                    flog::warn("Scanner: Worker thread cleanup timed out in destructor, detaching");
                    workerThread.detach();
                }
            } catch (const std::exception& e) {
                flog::error("Scanner: Exception during thread cleanup in destructor: {}", e.what());
                workerThread.detach();
            }
        }
        
        // Clean up other resources
        saveConfig();
        gui::menu.removeEntry(name);
        core::modComManager.unregisterInterface(name);
        stop();
        
        // Clean up file dialog
        if (logFileDialog) {
            delete logFileDialog;
            logFileDialog = nullptr;
        }
        
        flog::info("Scanner: Destructor completed");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }
    
    // Range management methods
    void addFrequencyRange(const std::string& name, double start, double stop, bool enabled = true, float gain = 20.0f) {
        frequencyRanges.emplace_back(name, start, stop, enabled, gain);
        saveConfig();
    }
    
    void removeFrequencyRange(int index) {
        if (index >= 0 && index < frequencyRanges.size()) {
            frequencyRanges.erase(frequencyRanges.begin() + index);
            if (currentRangeIndex >= frequencyRanges.size() && !frequencyRanges.empty()) {
                currentRangeIndex = frequencyRanges.size() - 1;
            }
            saveConfig();
        }
    }
    
    void toggleFrequencyRange(int index) {
        if (index >= 0 && index < frequencyRanges.size()) {
            frequencyRanges[index].enabled = !frequencyRanges[index].enabled;
            saveConfig();
        }
    }
    
    void updateFrequencyRange(int index, const std::string& name, double start, double stop, float gain) {
        if (index >= 0 && index < frequencyRanges.size()) {
            frequencyRanges[index].name = name;
            frequencyRanges[index].startFreq = start;
            frequencyRanges[index].stopFreq = stop;
            frequencyRanges[index].gain = gain;
            saveConfig();
            flog::info("Scanner: Updated range '{}' - gain set to {:.1f} dB", name, gain);
        }
    }
    
    // Get current active ranges for scanning
    std::vector<int> getActiveRangeIndices() {
        std::vector<int> activeRanges;
        for (int i = 0; i < frequencyRanges.size(); i++) {
            if (frequencyRanges[i].enabled) {
                activeRanges.push_back(i);
            }
        }
        return activeRanges;
    }
    
    // Get current scanning bounds (supports both single range and multi-range)
    bool getCurrentScanBounds(double& currentStart, double& currentStop) {
        // FREQUENCY MANAGER MODE: Get bounds from current band entry
        if (useFrequencyManager && currentBookmark) {
            const FrequencyBookmark* bookmark = static_cast<const FrequencyBookmark*>(currentBookmark);
            if (bookmark && bookmark->isBand) {
                // Use band boundaries from frequency manager
                currentStart = bookmark->startFreq;
                currentStop = bookmark->endFreq;
                return true;
            }
            // For single frequencies, use 1.5x the bookmark's bandwidth as scanning window
            if (bookmark && !bookmark->isBand) {
                double centerFreq = bookmark->frequency;
                double halfWindow = (bookmark->bandwidth * 1.5) / 2.0; // 1.5x bandwidth, split around center
                currentStart = centerFreq - halfWindow;
                currentStop = centerFreq + halfWindow;
                return true;
            }
        }
        
        if (frequencyRanges.empty()) {
            // Fall back to legacy single range
            currentStart = startFreq;
            currentStop = stopFreq;
            return true;
        }
        
        // Multi-range mode: get current active range
            auto activeRanges = getActiveRangeIndices();
            if (activeRanges.empty()) {
                return false; // No active ranges
            }
            
            // Ensure current range index is valid
            if (currentRangeIndex >= activeRanges.size()) {
                currentRangeIndex = 0;
            }
            
            int rangeIdx = activeRanges[currentRangeIndex];
            // Critical bounds check to prevent crash
            if (rangeIdx >= frequencyRanges.size()) return false;
            
            currentStart = frequencyRanges[rangeIdx].startFreq;
            currentStop = frequencyRanges[rangeIdx].stopFreq;
            return true;
        }
    
    // Get recommended gain for current range
    float getCurrentRangeGain() {
        if (frequencyRanges.empty()) return 20.0f;
        
        auto activeRanges = getActiveRangeIndices();
        if (activeRanges.empty() || currentRangeIndex >= activeRanges.size()) return 20.0f;
        
        int rangeIdx = activeRanges[currentRangeIndex];
        // Critical bounds check to prevent crash
        if (rangeIdx >= frequencyRanges.size()) return 20.0f;
        
        return frequencyRanges[rangeIdx].gain;
    }
    
    // Apply or recommend gain setting for current range
    void applyCurrentRangeGain() {
        if (frequencyRanges.empty()) return;
        
        auto activeRanges = getActiveRangeIndices();
        if (activeRanges.empty() || currentRangeIndex >= activeRanges.size()) return;
        
        int rangeIdx = activeRanges[currentRangeIndex];
        // Critical bounds check to prevent crash
        if (rangeIdx >= frequencyRanges.size()) return;
        
        float targetGain = frequencyRanges[rangeIdx].gain;
        
        try {
            std::string sourceName = sigpath::sourceManager.getSelectedName();
            if (!sourceName.empty()) {
                // Use the new SourceManager::setGain() method
                sigpath::sourceManager.setGain(targetGain);
                flog::info("Scanner: Applied gain {:.1f} dB for range '{}' (source: {})",
                          targetGain, frequencyRanges[rangeIdx].name, sourceName);
            } else {
                SCAN_DEBUG("Scanner: No source selected, cannot apply gain for range '{}'",
                          frequencyRanges[rangeIdx].name);
            }
        } catch (const std::exception& e) {
            flog::error("Scanner: Exception in applyCurrentRangeGain: {}", e.what());
        } catch (...) {
            flog::error("Scanner: Unknown exception in applyCurrentRangeGain");
        }
    }



private:
    // Scanner logging
    bool enableScanLogging = false;
    std::string scanLogPath;
    int scanLogMinDurationMs = 100;  // Minimum transmission duration to log (milliseconds)
    ScannerLogger scannerLogger;
    pfd::save_file* logFileDialog = nullptr;
    
    // Transmission duration tracking
    std::chrono::system_clock::time_point currentTransmissionStart;
    double currentTransmissionFreq = 0.0;
    float currentTransmissionLevel = 0.0f;
    bool isTrackingTransmission = false;
    
    // Helper function to log transmission end
    void logTransmissionEnd() {
        if (enableScanLogging && isTrackingTransmission) {
            auto now = std::chrono::system_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - currentTransmissionStart);
            float durationSeconds = duration.count() / 1000.0f;
            
            // Apply minimum duration filter
            if (duration.count() >= scanLogMinDurationMs) {
                ScanRecord rec{
                    currentTransmissionFreq, 
                    currentTransmissionLevel, 
                    currentTransmissionStart,
                    now,
                    durationSeconds,
                    true  // isEndOfTransmission
                };
                scannerLogger.log(rec);
                flog::debug("Scanner: Logged transmission {:.6f} MHz, duration: {:.3f}s", 
                           currentTransmissionFreq / 1e6, durationSeconds);
            } else {
                flog::debug("Scanner: Skipped short transmission {:.6f} MHz, duration: {}ms (< {}ms threshold)", 
                           currentTransmissionFreq / 1e6, (int)duration.count(), (int)scanLogMinDurationMs);
            }
            isTrackingTransmission = false;
        }
    }
    // Coverage Analysis Functions for Band Scanning Optimization
    struct CoverageAnalysis {
        double bandWidth = 0.0;           // Total band width (Hz)
        double effectiveStep = 0.0;       // Average step size (Hz) 
        double radioBandwidth = 0.0;      // Radio VFO bandwidth (Hz)
        double effectiveBandwidth = 0.0;  // Radio bandwidth * passband ratio (Hz)
        double coveragePerStep = 0.0;     // Coverage provided by each step (Hz)
        double totalCoverage = 0.0;       // Total spectrum covered (Hz)
        double coveragePercent = 0.0;     // Coverage percentage (0-100%)
        double gapSize = 0.0;             // Size of gaps between steps (Hz)
        double overlapSize = 0.0;         // Size of overlaps between steps (Hz)
        int numSteps = 0;                 // Number of scan points
        bool hasGaps = false;             // True if there are coverage gaps
        bool hasOverlaps = false;         // True if there are overlaps
        std::string recommendation = "";  // Optimization recommendation
        
        // FFT Analysis Parameters
        int fftSize = 0;                  // Current FFT size (bins)
        double sampleRate = 0.0;          // Effective sample rate (Hz)
        double fftResolution = 0.0;       // FFT frequency resolution (Hz/bin)
        double analysisSpan = 0.0;        // Total FFT analysis span (Hz)
        bool intervalTooSmall = false;    // True if interval < FFT resolution
        bool stepOptimal = false;         // True if step matches FFT analysis span
        std::string fftWarning = "";      // FFT-related warnings
    };
    
    // Realistic coverage calculation using actual system parameters
    CoverageAnalysis calculateBasicCoverage() {
        CoverageAnalysis analysis;
        
        // Get actual band bounds
        double currentStart, currentStop;
        if (!getCurrentScanBounds(currentStart, currentStop)) {
            analysis.recommendation = "No active scanning band selected";
            return analysis;
        }
        
        analysis.bandWidth = currentStop - currentStart;
        
        // Get REAL FFT and sample rate parameters from the system
        try {
            // Acquire raw FFT to get actual size used by scanner
            int actualRawFFTSize;
            float* rawData = gui::waterfall.acquireRawFFT(actualRawFFTSize);
            if (rawData) {
                analysis.fftSize = actualRawFFTSize;
                gui::waterfall.releaseRawFFT();
                flog::info("Scanner: Got FFT size from waterfall: {}", actualRawFFTSize);
            } else {
                analysis.fftSize = 524288; // Fallback based on typical SDR++ configuration
                flog::warn("Scanner: Failed to acquire FFT data, using fallback FFT size: {}", analysis.fftSize);
            }
            
            // Get actual sample rate from signal path - this should work!
            flog::debug("Scanner: About to call iq_frontend.getEffectiveSamplerate()");
            analysis.sampleRate = sigpath::iqFrontEnd.getEffectiveSamplerate();
            flog::info("Scanner: Effective sample rate from iq_frontend: {:.0f} Hz ({:.1f} MHz)", 
                          (double)analysis.sampleRate, (double)(analysis.sampleRate/1e6));
            
            // Note: The effective sample rate is the actual sample rate after decimation
            // This is the correct rate to use for FFT analysis since that's what the waterfall sees
            
            // Validate the sample rate
            if (analysis.sampleRate <= 0) {
                flog::error("Scanner: iq_frontend returned invalid sample rate: {:.0f} Hz", (double)analysis.sampleRate);
                throw std::runtime_error("Invalid sample rate from iq_frontend");
            }
            
            // Calculate real FFT resolution
            if (analysis.fftSize > 0 && analysis.sampleRate > 0) {
                analysis.fftResolution = analysis.sampleRate / analysis.fftSize;
                analysis.analysisSpan = analysis.sampleRate;
                flog::info("Scanner: Calculated FFT resolution: {:.2f} Hz/bin", (double)analysis.fftResolution);
            }
            
        } catch (const std::exception& e) {
            flog::error("Scanner: Exception in coverage calculation: {}", e.what());
        } catch (...) {
            // Fallback to reasonable defaults if system access fails
            analysis.fftSize = 524288;
            
            // Try to get sample rate from signal path one more time with better error handling
            try {
                analysis.sampleRate = sigpath::iqFrontEnd.getEffectiveSamplerate();
                flog::warn("Scanner: Fallback - got sample rate from signal path: {:.0f} Hz", (double)analysis.sampleRate);
                if (analysis.sampleRate <= 0) {
                    analysis.sampleRate = 10000000.0; // 10MHz default for modern SDRs
                    flog::warn("Scanner: Sample rate was <= 0, using 10MHz default");
                }
            } catch (...) {
                analysis.sampleRate = 10000000.0; // 10MHz default for modern SDRs (not RTL-SDR specific)
                flog::warn("Scanner: Exception getting sample rate, using 10MHz default");
            }
            
            analysis.fftResolution = analysis.sampleRate / analysis.fftSize;
            analysis.analysisSpan = analysis.sampleRate;
            analysis.fftWarning = "Using fallback FFT parameters - system access failed";
        }
        
        // Get REAL VFO bandwidth from radio module
        double actualVfoBandwidth = 0.0;
        try {
            // Access the radio's actual bandwidth setting
            if (!gui::waterfall.selectedVFO.empty() && 
                core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                
                // Try to get actual VFO bandwidth from radio module interface
                // This requires the radio module to be loaded and active
                actualVfoBandwidth = gui::waterfall.getBandwidth();
            }
        } catch (...) {
            // Fallback if radio access fails
        }
        
        // Use profile bandwidth if available, otherwise use VFO bandwidth
        analysis.radioBandwidth = actualVfoBandwidth;
        if (useFrequencyManager && currentTuningProfile) {
            const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
            if (profile && profile->bandwidth > 0) {
                analysis.radioBandwidth = profile->bandwidth;
            }
        }
        
        // Calculate REALISTIC coverage metrics
        if (useFrequencyManager) {
            // Calculate effective coverage based on actual system parameters
            analysis.effectiveStep = interval;
            analysis.numSteps = (int)std::ceil(analysis.bandWidth / analysis.effectiveStep);
            
            // Real coverage calculation: each step covers the VFO bandwidth
            if (analysis.radioBandwidth > 0) {
                analysis.coveragePerStep = analysis.radioBandwidth;
                analysis.totalCoverage = std::min(analysis.numSteps * analysis.coveragePerStep, analysis.bandWidth);
                analysis.coveragePercent = (analysis.totalCoverage / analysis.bandWidth) * 100.0;
                
                // Calculate gaps and overlaps based on actual parameters
                if (analysis.effectiveStep > analysis.radioBandwidth) {
                    // Steps are larger than bandwidth = gaps
                    analysis.hasGaps = true;
                    analysis.gapSize = analysis.effectiveStep - analysis.radioBandwidth;
                } else if (analysis.effectiveStep < analysis.radioBandwidth) {
                    // Steps are smaller than bandwidth = overlaps  
                    analysis.hasOverlaps = true;
                    analysis.overlapSize = analysis.radioBandwidth - analysis.effectiveStep;
                }
                
                // Generate intelligent recommendations
                if (analysis.coveragePercent < 80.0) {
                    analysis.recommendation = "Large gaps detected - reduce interval for better coverage";
                } else if (analysis.coveragePercent > 150.0) {
                    analysis.recommendation = "Excessive overlap - increase interval for faster scanning";
                } else {
                    analysis.recommendation = "Coverage is well optimized";
                }
            } else {
                // Fallback calculation if VFO bandwidth unknown
                analysis.coveragePercent = 90.0; // Assume reasonable coverage
                analysis.recommendation = "VFO bandwidth unknown - using estimated coverage";
            }
            
            // Check FFT resolution vs interval
            if (analysis.fftResolution > 0 && interval < analysis.fftResolution) {
                analysis.intervalTooSmall = true;
                analysis.fftWarning = "Interval smaller than FFT resolution (" + 
                                    std::to_string((int)(analysis.fftResolution/1000.0)) + " kHz)";
            }
            
        } else {
            // Legacy interval scanning - use actual VFO bandwidth 
            analysis.effectiveStep = interval;
            analysis.numSteps = (int)std::ceil(analysis.bandWidth / analysis.effectiveStep);
            
            // Use actual VFO bandwidth or fallback
            if (analysis.radioBandwidth <= 0) {
                // Try to get current VFO bandwidth from waterfall
                try {
                    analysis.radioBandwidth = gui::waterfall.getBandwidth();
                } catch (...) {
                    analysis.radioBandwidth = 200000.0; // 200 kHz conservative fallback for wide FM
                }
            }
            
            // Calculate realistic coverage for legacy mode
            analysis.coveragePerStep = analysis.radioBandwidth;
            analysis.totalCoverage = std::min(analysis.numSteps * analysis.coveragePerStep, analysis.bandWidth);
            analysis.coveragePercent = (analysis.totalCoverage / analysis.bandWidth) * 100.0;
            
            // Calculate gaps and overlaps for legacy mode
            if (analysis.effectiveStep > analysis.radioBandwidth) {
                analysis.hasGaps = true;
                analysis.gapSize = analysis.effectiveStep - analysis.radioBandwidth;
            } else if (analysis.effectiveStep < analysis.radioBandwidth) {
                analysis.hasOverlaps = true;
                analysis.overlapSize = analysis.radioBandwidth - analysis.effectiveStep;
            }
        }
        
        // Simple recommendations
        if (analysis.coveragePercent < 50.0) {
            analysis.recommendation = "Low coverage - consider smaller steps or larger radio bandwidth";
        } else if (analysis.coveragePercent > 150.0) {
            analysis.recommendation = "High overlap - consider larger steps for faster scanning";
        } else if (analysis.hasGaps) {
            analysis.recommendation = "Coverage gaps detected - reduce step size for better coverage";
        } else {
            analysis.recommendation = "Good coverage with current settings";
        }
        
        return analysis;
    }

    static void menuHandler(void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;
        
        // === SCANNER READY STATUS ===
        // Scanner now uses Frequency Manager exclusively for simplified operation
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Scanner uses Frequency Manager entries");
        ImGui::TextWrapped("Enable scanning for specific entries in Frequency Manager to include them in scan list.");
        ImGui::Separator();
        
        // REMOVED: Legacy range manager - scanner now uses Frequency Manager exclusively
        if (false) {  // Legacy code removed for cleaner UI
            ImGui::Begin("Scanner Range Manager", &_this->showRangeManager);
            
            // Add new range section
            ImGui::Text("Add New Range");
            ImGui::Separator();
            ImGui::InputText("Name", _this->newRangeName, sizeof(_this->newRangeName));
            ImGui::InputDouble("Start (Hz)", &_this->newRangeStart, 100000.0, 1000000.0, "%.0f");
            ImGui::InputDouble("Stop (Hz)", &_this->newRangeStop, 100000.0, 1000000.0, "%.0f");
            ImGui::InputFloat("Gain (dB)", &_this->newRangeGain, 1.0f, 10.0f, "%.1f");
            
            if (ImGui::Button("Add Range")) {
                _this->addFrequencyRange(std::string(_this->newRangeName), _this->newRangeStart, _this->newRangeStop, true, _this->newRangeGain);
                strcpy(_this->newRangeName, "New Range");
                _this->newRangeStart = 88000000.0;
                _this->newRangeStop = 108000000.0;
                _this->newRangeGain = 20.0f;
            }
            
            ImGui::Spacing();
            ImGui::Text("Existing Ranges");
            ImGui::Separator();
            
            // List existing ranges
            for (int i = 0; i < _this->frequencyRanges.size(); i++) {
                auto& range = _this->frequencyRanges[i];
                
                ImGui::PushID(i);
                
                // Enabled checkbox
                bool enabled = range.enabled;
                if (ImGui::Checkbox("##enabled", &enabled)) {
                    _this->toggleFrequencyRange(i);
                }
                ImGui::SameLine();
                
                // Range info with edit capability
                static char editName[256];
                static double editStart, editStop;
                static float editGain;
                static int editingIndex = -1;
                
                if (editingIndex == i) {
                    // Editing mode
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputText("##edit_name", editName, sizeof(editName));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputDouble("##edit_start", &editStart, 1000000.0, 10000000.0, "%.0f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputDouble("##edit_stop", &editStop, 1000000.0, 10000000.0, "%.0f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputFloat("##edit_gain", &editGain, 1.0f, 10.0f, "%.1f");
                    ImGui::SameLine();
                    
                    if (ImGui::Button("Save")) {
                        _this->updateFrequencyRange(i, std::string(editName), editStart, editStop, editGain);
                        editingIndex = -1;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        editingIndex = -1;
                    }
            } else {
                    // Display mode
                    ImGui::Text("%s: %.1f - %.1f MHz (%.1f dB)", 
                        range.name.c_str(), 
                        range.startFreq / 1e6, 
                        range.stopFreq / 1e6,
                        range.gain);
                    ImGui::SameLine();
                    
                    if (ImGui::Button("Edit")) {
                        editingIndex = i;
                        strcpy(editName, range.name.c_str());
                        editStart = range.startFreq;
                        editStop = range.stopFreq;
                        editGain = range.gain;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) {
                        _this->removeFrequencyRange(i);
                        break; // Break to avoid iterator invalidation
                    }
                }
                
                ImGui::PopID();
            }
            
            // Quick presets section
            if (ImGui::CollapsingHeader("Quick Presets")) {
                if (ImGui::Button("FM Broadcast (88-108 MHz)")) {
                    _this->addFrequencyRange("FM Broadcast", 88000000.0, 108000000.0, true, 15.0f);
                }
                if (ImGui::Button("Airband (118-137 MHz)")) {
                    _this->addFrequencyRange("Airband", 118000000.0, 137000000.0, true, 25.0f);
                }
                if (ImGui::Button("2m Ham (144-148 MHz)")) {
                    _this->addFrequencyRange("2m Ham", 144000000.0, 148000000.0, true, 30.0f);
                }
                if (ImGui::Button("PMR446 (446.0-446.2 MHz)")) {
                    _this->addFrequencyRange("PMR446", 446000000.0, 446200000.0, true, 35.0f);
                }
                if (ImGui::Button("70cm Ham (420-450 MHz)")) {
                    _this->addFrequencyRange("70cm Ham", 420000000.0, 450000000.0, true, 35.0f);
                }
            }
            
            ImGui::End();
        }
        
        // REMOVED: Legacy single range controls - scanner now uses Frequency Manager exclusively
        
        // === COMMON SCANNER PARAMETERS ===
        ImGui::Spacing();
        ImGui::Text("Scanner Parameters");
        ImGui::Separator();

        // LIVE PARAMETERS: Can be changed while scanning for immediate effect!
        ImGui::LeftLabel("Interval (Hz)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("In-memory frequency analysis step size for spectrum search (Hz)\n"
                             "Analyzes captured spectrum data WITHOUT using the hardware tuner\n"
                             "Works with Frequency Manager: Step=hardware tuner jumps, Interval=frequency analysis\n"
                             "Common values: 5000 Hz (precise), 25000 Hz (balanced), 100000 Hz (fast)\n"
                             "TIP: Small intervals find more signals (limited by radio bandwidth)");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        {
            float intervalFloat = (float)_this->interval;
            if (ImGui::PrecisionSliderFloat("##scanner_interval", &intervalFloat, 1000.0f, 500000.0f, "%.0f Hz", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
                _this->interval = std::clamp((double)intervalFloat, 1000.0, 500000.0); // Enforce limits
                _this->saveConfig();
            }
        }
        
        // PERFORMANCE: Configurable scan rate (consistent across all modes)
        ImGui::LeftLabel("Scan Rate (Hz)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The rate at which to check for signals during scanning\n"
                             "\n"
                             "Controls the rate of in-memory signal detection using FFT analysis.\n"
                             "High rates are possible because most work is digital spectrum analysis,\n"
                             "not physical hardware tuner steps.\n"
                             "\n"
                             "COMMON VALUES:\n"
                             "10 Hz = conservative, very stable\n"
                             "25 Hz = balanced (recommended starting point)\n"
                             "50 Hz = fast scanning\n"
                             "100-500 Hz = very fast scanning\n"
                             "\n"
                             "Higher rates consume more CPU but find signals faster");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        
        float maxScanRate = _this->unlockHighSpeed ? (float)MAX_SCAN_RATE : (float)NORMAL_MAX_SCAN_RATE;
        {
            float scanRateFloat = (float)_this->scanRateHz;
            if (ImGui::PrecisionSliderFloat("##scanner_scan_rate", &scanRateFloat, 1.0f, maxScanRate, "%.0f Hz", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
                _this->scanRateHz = std::clamp((int)scanRateFloat, 1, (int)maxScanRate);
                _this->saveConfig();
            }
        }
        
        // Add unlock higher speed toggle with parameterized max rate
        char unlockLabel[64];
        snprintf(unlockLabel, sizeof(unlockLabel), "Unlock high-speed scanning (up to %d Hz)", MAX_SCAN_RATE);
        if (ImGui::Checkbox(unlockLabel, &_this->unlockHighSpeed)) {
            // Clamp scan rate if unlocking was disabled
            if (!_this->unlockHighSpeed && _this->scanRateHz > NORMAL_MAX_SCAN_RATE) {
                _this->scanRateHz = NORMAL_MAX_SCAN_RATE;
            }
            _this->saveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enable scan rates up to %d Hz (default max is %d Hz)\n"
                             "\n"
                             "FFT-based scanning (Frequency Manager mode) can handle much higher\n"
                             "rates since most work is in-memory spectrum analysis.\n"
                             "\n"
                             "WARNING: Very high scan rates (>500 Hz) may consume significant CPU\n"
                             "and could impact system responsiveness",
                             MAX_SCAN_RATE, NORMAL_MAX_SCAN_RATE);
        }
        ImGui::LeftLabel("Passband Ratio");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Signal detection bandwidth as percentage of VFO width\n"
                             "TIP: Start at 100%% for best signal detection\n"
                             "Lower if catching too many false positives");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        
        // DISCRETE SLIDER: Show actual values with units instead of indices
        if (ImGui::SliderInt("##passband_ratio_discrete", &_this->passbandIndex, 0, _this->PASSBAND_VALUES_COUNT - 1, _this->PASSBAND_FORMATS[_this->passbandIndex])) {
            // Only update passband ratio - don't affect interval or scan rate!
            _this->passbandRatio = _this->PASSBAND_VALUES[_this->passbandIndex] / 100.0; // Convert percentage to ratio
            _this->saveConfig();
            SCAN_DEBUG("Scanner: Passband slider changed to index {} ({}%)", _this->passbandIndex, _this->PASSBAND_VALUES[_this->passbandIndex]);
        }
        ImGui::LeftLabel("Tuning Time (ms)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Time to wait after tuning before checking for signals (ms)\n"
                             "Allows hardware and DSP to settle after frequency change\n"
                             "TIP: Increase if missing signals (slow hardware)\n"
                             "Decrease for faster scanning (stable hardware)\n"
                             "Range: %dms - 10000ms, default: 250ms%s",
                             _this->unlockHighSpeed ? MIN_TUNING_TIME : 100,
                             _this->unlockHighSpeed ? "\nFor high-speed scanning (>50Hz), use 10-50ms" : "");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        // Convert to float for precision slider, use appropriate range for unlockHighSpeed mode
        float tuningTimeFloat = (float)_this->tuningTime;
        float minTime = _this->unlockHighSpeed ? (float)MIN_TUNING_TIME : 100.0f;
        if (ImGui::PrecisionSliderFloat("##tuning_time_scanner", &tuningTimeFloat, minTime, 10000.0f, "%.0f ms", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
            _this->tuningTime = std::clamp<int>((int)tuningTimeFloat, (int)minTime, 10000);
            // If user manually adjusts tuning time, turn off auto mode
            if (_this->tuningTimeAuto) {
                _this->tuningTimeAuto = false;
                flog::info("Scanner: Auto tuning time adjustment disabled due to manual edit");
            }
            _this->saveConfig();
        }
        
        // Auto-adjust tuning time based on scan rate (available at all scan rates)
        ImGui::SameLine();
        if (ImGui::Button(_this->tuningTimeAuto ? "Auto-Adjust (ON)" : "Auto-Adjust")) {
            // Toggle auto-adjust mode
            _this->tuningTimeAuto = !_this->tuningTimeAuto;
            
            // If turning on, immediately apply the scaling formula
            if (_this->tuningTimeAuto) {
                // Use the same scaling formula as in the worker thread
                
                // Calculate optimal tuning time that scales with scan rate
                const int optimalTime = std::max(
                    MIN_TUNING_TIME, 
                    static_cast<int>((BASE_TUNING_TIME * BASE_SCAN_RATE) / static_cast<int>(_this->scanRateHz))
                );
                
                _this->tuningTime = optimalTime;
                flog::info("Scanner: Auto-adjusted tuning time to {}ms for {}Hz scan rate", 
                          _this->tuningTime, _this->scanRateHz);
            }
            
            _this->saveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Toggle automatic tuning time adjustment based on scan rate\n"
                             "When ON: Tuning time will automatically scale with scan rate\n"
                             "Formula: tuningTime = %dms * (%dHz / currentRate)\n"
                             "Examples:\n"
                             "- %dHz scan rate: ~%dms tuning time\n"
                             "- %dHz scan rate: ~%dms tuning time\n"
                             "- %dHz scan rate: %dms tuning time\n"
                             "- %dHz scan rate: %dms tuning time",
                             BASE_TUNING_TIME, BASE_SCAN_RATE,
                             MAX_SCAN_RATE, BASE_TUNING_TIME * BASE_SCAN_RATE / MAX_SCAN_RATE,
                             100, BASE_TUNING_TIME * BASE_SCAN_RATE / 100,
                             BASE_SCAN_RATE, BASE_TUNING_TIME,
                             25, BASE_TUNING_TIME * BASE_SCAN_RATE / 25);
        }
        ImGui::LeftLabel("Linger Time (ms)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Time to stay on frequency when signal is detected (ms)\n"
                             "Scanner pauses to let you listen to the signal\n"
                             "TIP: Longer times for voice communications (2000+ ms)\n"
                             "Shorter times for quick signal identification (500-1000 ms)\n"
                             "Range: %dms - 10000ms, default: %dms\n"
                             "For high scan rates (>%dHz), consider using %d-%dms",
                             _this->unlockHighSpeed ? MIN_LINGER_TIME : 100,
                             BASE_LINGER_TIME,
                             NORMAL_MAX_SCAN_RATE,
                             MIN_LINGER_TIME,
                             BASE_LINGER_TIME / 2);
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        // Convert to float for precision slider, use appropriate range for unlockHighSpeed mode
        float lingerTimeFloat = (float)_this->lingerTime;
        float minLinger = _this->unlockHighSpeed ? (float)MIN_LINGER_TIME : 100.0f;
        if (ImGui::PrecisionSliderFloat("##linger_time_scanner", &lingerTimeFloat, minLinger, 10000.0f, "%.0f ms", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
            _this->lingerTime = std::clamp<int>((int)lingerTimeFloat, (int)minLinger, 10000);
            _this->saveConfig();
        }
        
        // Add linger time auto-adjust button when auto-adjust is enabled for tuning time
        if (_this->tuningTimeAuto) {
            ImGui::SameLine();
            if (ImGui::Button("Scale Linger")) {
                // Use a similar scaling formula as tuning time, but with different base values
                // Linger time should be longer than tuning time
                
                // Calculate optimal linger time that scales with scan rate
                const int optimalTime = std::max(
                    MIN_LINGER_TIME, 
                    static_cast<int>((BASE_LINGER_TIME * BASE_SCAN_RATE) / static_cast<int>(_this->scanRateHz))
                );
                
                _this->lingerTime = optimalTime;
                _this->saveConfig();
                flog::info("Scanner: Scaled linger time to {}ms for {}Hz scan rate", 
                          _this->lingerTime, _this->scanRateHz);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scale linger time based on scan rate (one-time adjustment)\n"
                                 "Formula: lingerTime = %dms * (%dHz / currentRate)\n"
                                 "Examples:\n"
                                 "- %dHz scan rate: ~%dms linger time\n"
                                 "- %dHz scan rate: ~%dms linger time\n"
                                 "- %dHz scan rate: %dms linger time\n"
                                 "- %dHz scan rate: %dms linger time",
                                 BASE_LINGER_TIME, BASE_SCAN_RATE,
                                 MAX_SCAN_RATE, BASE_LINGER_TIME * BASE_SCAN_RATE / MAX_SCAN_RATE,
                                 100, BASE_LINGER_TIME * BASE_SCAN_RATE / 100,
                                 BASE_SCAN_RATE, BASE_LINGER_TIME,
                                 25, BASE_LINGER_TIME * BASE_SCAN_RATE / 25);
            }
        }
        // LIVE PARAMETERS: No more disabling - all can be changed during scanning!

        ImGui::LeftLabel("Trigger Level");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Signal strength threshold for stopping scanner (dBFS)\n"
                             "Scanner stops when signal exceeds this level\n"
                             "Lower values = more sensitive, higher values = less sensitive");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::PrecisionSliderFloat("##scanner_trigger_level", &_this->level, -150.0, 0.0, "%.1f dBFS", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
            _this->saveConfig();
        }
        
        // Squelch Delta Control with improved labeling
        ImGui::LeftLabel("Delta (dB)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Close threshold = Squelch - Delta\n"
                             "Higher values reduce unnecessary squelch closures\n"
                             "Creates hysteresis effect to maintain reception");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::PrecisionSliderFloat("##scanner_squelch_delta", &_this->squelchDelta, 0.0f, 10.0f, "%.1f dB", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
            _this->saveConfig();
        }
        
        ImGui::LeftLabel("Auto Delta");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically calculate squelch delta based on noise floor\n"
                             "Places squelch closing level closer to noise floor\n"
                             "Updates every 250ms when not receiving");
        }
        if (ImGui::Checkbox(("##scanner_squelch_delta_auto_" + _this->name).c_str(), &_this->squelchDeltaAuto)) {
            _this->saveConfig();
        }
        
        // MUTE WHILE SCANNING: Prevent noise bursts during frequency sweeps
        ImGui::LeftLabel("Mute Scanning");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically mute audio while scanning frequencies\n"
                             "Prevents noise bursts and audio artifacts during sweeps\n"
                             "Audio is restored when a signal is detected and locked");
        }
        if (ImGui::Checkbox(("##scanner_mute_scanning_" + _this->name).c_str(), &_this->muteWhileScanning)) {
            _this->saveConfig();
        }

        // Enhanced mute sub-settings (only show if mute while scanning is enabled)
        if (_this->muteWhileScanning) {
            ImGui::Indent();
            
            // Aggressive mute checkbox
            ImGui::LeftLabel("Aggressive Mute");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Enhanced noise protection during frequency/demod changes\n"
                                 "Applies extra muting during critical operations\n"
                                 "Disable for minimal scanning interference");
            }
            if (ImGui::Checkbox(("##scanner_aggressive_mute_" + _this->name).c_str(), &_this->aggressiveMute)) {
                _this->saveConfig();
            }
            
            // Aggressive mute level slider (only show if aggressive mute is enabled)
            if (_this->aggressiveMute) {
                ImGui::LeftLabel("Mute Level (dB)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Emergency mute squelch level during operations\n"
                                     "Higher values (closer to 0) = more aggressive muting\n"
                                     "Range: -10.0 dB to 0.0 dB");
                }
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::SliderFloat(("##scanner_aggressive_level_" + _this->name).c_str(), &_this->aggressiveMuteLevel, -10.0f, 0.0f, "%.1f dB")) {
                    _this->saveConfig();
                }
            }
            
            ImGui::Unindent();
        }

        // Signal analysis display
        ImGui::LeftLabel("Show Signal Info");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically display signal strength and SNR when a signal is detected\n"
                             "Shows the same information as Ctrl+click on VFO in waterfall\n"
                             "Useful for analyzing signal quality during scanning");
        }
        if (ImGui::Checkbox(("##scanner_show_signal_info_" + _this->name).c_str(), &_this->showSignalInfo)) {
            _this->saveConfig();
        }
        
        ImGui::LeftLabel("Show Trigger Level");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show horizontal line on FFT display indicating trigger level\n"
                             "Visual indicator shows where scanner threshold is set\n"
                             "Helps with adjusting trigger level for optimal scanning");
        }
        if (ImGui::Checkbox(("##scanner_show_trigger_level_" + _this->name).c_str(), &_this->showTriggerLevel)) {
            _this->saveConfig();
        }

        // Blacklist controls
        ImGui::Separator();
        ImGui::Text("Frequency Blacklist");
        
        // Add frequency to blacklist
        static double newBlacklistFreq = 0.0;
        ImGui::LeftLabel("Add Frequency (Hz)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##new_blacklist_freq", &newBlacklistFreq, 1000.0, 100000.0, "%0.0f")) {
            newBlacklistFreq = round(newBlacklistFreq);
        }
        if (ImGui::Button("Add to Blacklist##scanner_add_blacklist", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            if (newBlacklistFreq > 0) {
                _this->blacklistedFreqs.push_back(newBlacklistFreq);
                _this->frequencyNameCache.clear(); // Clear cache when blacklist changes
                _this->frequencyNameCacheDirty = true;
                newBlacklistFreq = 0.0;
                _this->saveConfig();
                
                // UX FIX: Automatically resume scanning after blacklisting (same as "Blacklist Current")
                {
                    std::lock_guard<std::mutex> lck(_this->scanMtx);
                    
                    // Log any ongoing transmission before manual stop
                    _this->logTransmissionEnd();
                    
                    _this->receiving = false;
                }
                _this->applyMuteWhileScanning(); // Mute while resuming scanning
                SCAN_DEBUG("Scanner: Auto-resuming scanning after adding frequency to blacklist");
            }
        }
        
        // Show current frequency for reference
        if (!gui::waterfall.selectedVFO.empty()) {
            double currentFreq = gui::waterfall.getCenterFrequency();
            if (gui::waterfall.vfos.find(gui::waterfall.selectedVFO) != gui::waterfall.vfos.end()) {
                currentFreq += gui::waterfall.vfos[gui::waterfall.selectedVFO]->centerOffset;
            }
            ImGui::Text("Current Frequency: %.0f Hz (%.3f MHz)", currentFreq, currentFreq / 1e6);
        } else {
            ImGui::TextDisabled("Current Frequency: No VFO selected");
        }
        
        // Add current tuned frequency to blacklist
        bool hasValidFreq = !gui::waterfall.selectedVFO.empty();
        if (!hasValidFreq) { ImGui::BeginDisabled(); }
        if (ImGui::Button("Blacklist Current Frequency##scanner_blacklist_current", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            if (!gui::waterfall.selectedVFO.empty()) {
                // Get current center frequency + VFO offset
                double currentFreq = gui::waterfall.getCenterFrequency();
                if (gui::waterfall.vfos.find(gui::waterfall.selectedVFO) != gui::waterfall.vfos.end()) {
                    currentFreq += gui::waterfall.vfos[gui::waterfall.selectedVFO]->centerOffset;
                }
                
                // Check if frequency is already blacklisted (avoid duplicates)
                bool alreadyBlacklisted = false;
                for (const double& blacklisted : _this->blacklistedFreqs) {
                    if (std::abs(currentFreq - blacklisted) < _this->blacklistTolerance) {
                        alreadyBlacklisted = true;
                        break;
                    }
                }
                
                if (!alreadyBlacklisted) {
                    _this->blacklistedFreqs.push_back(currentFreq);
                    _this->frequencyNameCache.clear(); // Clear cache when blacklist changes
                    _this->frequencyNameCacheDirty = true;
                    _this->saveConfig();
                    // Blacklist addition logging removed - action is already visible in UI
                    
                    // UX FIX: Automatically resume scanning after blacklisting
                    // (Same mechanism as directional arrow buttons)
                    {
                        std::lock_guard<std::mutex> lck(_this->scanMtx);
                        _this->receiving = false;
                    }
                    _this->applyMuteWhileScanning(); // Mute while resuming scanning
                    SCAN_DEBUG("Scanner: Auto-resuming scanning after blacklisting frequency");
                    
                } else {
                    flog::warn("Scanner: Frequency {:.0f} Hz already blacklisted (within tolerance)", (double)currentFreq);
                }
            } else {
                flog::warn("Scanner: No VFO selected, cannot blacklist current frequency");
            }
        }
        if (!hasValidFreq) { ImGui::EndDisabled(); }
        
        // Blacklist tolerance
        ImGui::LeftLabel("Blacklist Tolerance (Hz)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Frequency matching tolerance for blacklist entries (Hz)\n"
                             "Two frequencies within this range are considered the same\n"
                             "TIP: Lower values (100-500 Hz) for precise frequency control\n"
                             "Higher values (1-5 kHz) for tolerance against frequency drift\n"
                             "Default: 1000 Hz, Range: 100 Hz - 100 kHz");
        }
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        // Convert to float for precision slider
        float toleranceFloat = (float)_this->blacklistTolerance;
        if (ImGui::PrecisionSliderFloat("##blacklist_tolerance", &toleranceFloat, 100.0f, 100000.0f, "%.0f Hz", ImGuiSliderFlags_AlwaysClamp, ImGui::PRECISION_SLIDER_MODE_HYBRID)) {
            _this->blacklistTolerance = std::clamp<double>(round(toleranceFloat), 100.0, 100000.0);
            _this->saveConfig();
        }
        
        // List of blacklisted frequencies
        if (!_this->blacklistedFreqs.empty()) {
            ImGui::Text("Blacklisted Frequencies:");
            ImGui::Separator();
            
            // Create a scrollable region for the blacklist if there are many entries
            if (_this->blacklistedFreqs.size() > 5) {
                ImGui::BeginChild("##blacklist_scroll", ImVec2(0, 150), true);
            }
            
            for (size_t i = 0; i < _this->blacklistedFreqs.size(); i++) {
                // ENHANCED UX: Show frequency manager entry name if available
                std::string entryName = _this->lookupFrequencyManagerName(_this->blacklistedFreqs[i]);
                
                if (!entryName.empty()) {
                    // Show name + frequency for better UX
                    ImGui::Text("%s (%.3f MHz)", entryName.c_str(), _this->blacklistedFreqs[i] / 1e6);
                } else {
                    // Fallback to frequency only
                ImGui::Text("%.0f Hz (%.3f MHz)", _this->blacklistedFreqs[i], _this->blacklistedFreqs[i] / 1e6);
                }
                ImGui::SameLine();
                
                // Right-align the remove button
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 80);
                if (ImGui::Button(("Remove##scanner_remove_blacklist_" + std::to_string(i)).c_str())) {
                    _this->blacklistedFreqs.erase(_this->blacklistedFreqs.begin() + i);
                    _this->frequencyNameCache.clear(); // Clear cache when blacklist changes
                    _this->frequencyNameCacheDirty = true;
                    _this->saveConfig();
                    break;
                }
            }
            
            if (_this->blacklistedFreqs.size() > 5) {
                ImGui::EndChild();
            }
            
            ImGui::Spacing();
            if (ImGui::Button("Clear All Blacklisted##scanner_clear_blacklist", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                _this->blacklistedFreqs.clear();
                _this->frequencyNameCache.clear(); // Clear cache when blacklist changes
                _this->frequencyNameCacheDirty = true;
                _this->saveConfig();
            }
        }

        // === COVERAGE ANALYSIS DISPLAY ===
        // SAFE IMPLEMENTATION: Only basic calculations with scanner's own data
        static bool enableCoverageAnalysis = false;
        static bool lastSdrRunning = false;
        static int stableFrames = 0;
        bool currentSdrRunning = gui::mainWindow.sdrIsRunning();
        
        // Track SDR state stability 
        if (currentSdrRunning == lastSdrRunning) {
            stableFrames++;
        } else {
            stableFrames = 0; // Reset on any state change
            enableCoverageAnalysis = false; // Disable on transitions
        }
        lastSdrRunning = currentSdrRunning;
        
        // Enable after SDR has been stable for a while
        if (currentSdrRunning && stableFrames > 120) { // 2 seconds at 60fps
            enableCoverageAnalysis = true;
        }
        
        ImGui::Spacing();
        ImGui::Text("Band Coverage Analysis");
        ImGui::Separator();
        
        if (enableCoverageAnalysis) {
            // Use safe basic coverage calculation - no external component access
            try {
                auto coverage = _this->calculateBasicCoverage();
                
                if (coverage.bandWidth > 0) {
                    // MAIN FEATURE: Coverage percentage with color coding
                    ImVec4 coverageColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f); // Green - good
                    if (coverage.coveragePercent < 80.0) {
                        coverageColor = ImVec4(0.8f, 0.8f, 0.2f, 1.0f); // Yellow - warning
                    }
                    if (coverage.coveragePercent < 50.0) {
                        coverageColor = ImVec4(0.8f, 0.2f, 0.2f, 1.0f); // Red - poor
                    }
                    
                    ImGui::TextColored(coverageColor, "Coverage: %.1f%%", coverage.coveragePercent);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("COVERAGE PERCENTAGE GUIDE:\n"
                                         "100%% = Maximum thoroughness (<=2.5 kHz interval) - catches everything\n"
                                         "98%%+ = Excellent (<=5 kHz interval) - finds weak signals\n"
                                         "85%%+ = Good (10 kHz interval) - balanced speed/thoroughness\n" 
                                         "70%%+ = Reasonable (20 kHz interval) - may miss some weak signals\n"
                                         "50%%+ = Fast (40+ kHz interval) - good for strong signals only\n"
                                         "\n"
                                         "Higher coverage = more thorough scanning but slower\n"
                                         "Lower coverage = faster scanning but may miss weak transmissions");
                    }
                    
                    ImGui::SameLine();
                    if (coverage.hasGaps) {
                        ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.2f, 1.0f), " (gaps)");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("GAPS DETECTED - some frequencies might be missed.\n"
                                             "\n"
                                             "WHAT THIS MEANS:\n"
                                             "- Interval is large relative to signal detection needs\n"
                                             "- May miss weak or intermittent transmissions\n"
                                             "- Could skip over active frequencies between scan points\n"
                                             "\n"
                                             "SOLUTIONS:\n"
                                             "- Reduce interval size for more thorough coverage\n"
                                             "- Consider if current speed vs coverage trade-off is acceptable");
                        }
                    } else if (coverage.hasOverlaps) {
                        ImGui::TextColored(ImVec4(0.2f, 0.6f, 0.8f, 1.0f), " (overlap)");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("OVERLAP DETECTED - scanning same frequencies multiple times.\n"
                                             "\n"
                                             "WHAT THIS MEANS:\n"
                                             "- Very small interval provides high precision\n"
                                             "- Multiple scan passes over the same frequency ranges\n"
                                             "\n"
                                             "BENEFITS:\n"
                                             "- Catches the weakest possible signals\n"
                                             "- High probability of detecting intermittent transmissions\n"
                                             "\n"
                                             "TRADE-OFFS:\n"
                                             "- Slower overall scanning speed\n"
                                             "- May spend too long in one area vs covering more spectrum");
                        }
                    }
                    
                    // Compact settings display with FFT analysis details
                    ImGui::Text("Interval: %.1f kHz", _this->interval / 1e3);
                    if (coverage.fftResolution > 0) {
                        ImGui::SameLine();
                        if (coverage.intervalTooSmall) {
                            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1.0f), " (< FFT res: %.1f Hz)", coverage.fftResolution);
                        } else {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), " (FFT res: %.1f Hz)", coverage.fftResolution);
                        }
                    }
                    
                    if (ImGui::IsItemHovered()) {
                        if (_this->useFrequencyManager) {
                            ImGui::SetTooltip("INTERVAL = IN-MEMORY FREQUENCY ANALYSIS STEP SIZE (%.1f kHz)\n"
                                             "\n"
                                             "REAL-TIME FFT ANALYSIS:\n"
                                             "- FFT Size: %d bins\n"
                                             "- Sample Rate: %.1f MHz\n"
                                             "- FFT Resolution: %.1f Hz per bin\n"
                                             "- VFO Bandwidth: %.1f kHz\n"
                                             "\n"
                                             "HOW IT WORKS:\n"
                                             "1. Radio hardware jumps between major frequency points in your bands\n"
                                             "2. At each stop, in-memory frequency analysis checks spectrum in %.1f kHz steps\n"
                                             "   (no hardware tuner steps needed - very fast!)\n"
                                             "\n"
                                             "INTERVAL SIZE GUIDE:\n"
                                             "- 2.5-5 kHz: Maximum sensitivity, catches weakest signals\n"
                                             "- 6.25-12.5 kHz: Good balance for most applications\n" 
                                             "- 25 kHz: Fast scanning, strong signals only\n"
                                             "- 50+ kHz: Very fast, nearby/powerful transmissions only\n"
                                             "\n"
                                             "Match interval to your target signal characteristics and band", 
                                             _this->interval / 1e3,
                                             coverage.fftSize, coverage.sampleRate / 1e6, coverage.fftResolution,
                                             coverage.radioBandwidth / 1e3, _this->interval / 1e3);
                        } else {
                            ImGui::SetTooltip("INTERVAL = FREQUENCY STEP SIZE (%.1f kHz)\n"
                                             "\n"
                                             "CURRENT SYSTEM:\n"
                                             "- FFT Size: %d bins\n"
                                             "- Sample Rate: %.1f MHz\n"
                                             "- FFT Resolution: %.1f Hz per bin\n"
                                             "\n"
                                             "NOTE: Frequency Manager mode is recommended for optimal performance.\n"
                                             "Enable scanning on frequency entries in Frequency Manager for\n"
                                             "faster, more efficient scanning with FFT-based signal detection.", 
                                             _this->interval / 1e3,
                                             coverage.fftSize, coverage.sampleRate / 1e6, coverage.fftResolution);
                        }
                    }
                    
                    if (_this->useFrequencyManager) {
                        ImGui::Text("Mode: Frequency Manager + FFT");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("FREQUENCY MANAGER + FFT MODE\n"
                                             "\n"
                                             "OPTIMIZED TWO-TIER SCANNING:\n"
                                             "- Large band steps defined in Frequency Manager (fast hardware jumps)\n"
                                             "- Small in-memory frequency analysis intervals (your current: %.1f kHz)\n"
                                             "- Result: Hardware makes big jumps between major frequencies\n"
                                             "  At each stop, FFT digitally analyzes spectrum in small steps\n"
                                             "\n"
                                             "WHY THIS IS EFFICIENT:\n"
                                             "- Hardware tuning is slow (milliseconds per step)\n"
                                             "- FFT analysis is fast (microseconds per frequency)\n"
                                             "- Combines speed of large steps + thoroughness of small intervals\n"
                                             "\n"
                                             "Perfect for covering wide frequency ranges quickly yet thoroughly", _this->interval / 1e3);
                        }
                    } else {
                        ImGui::Text("Mode: Basic scanning");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("BASIC SCANNING MODE\n"
                                             "\n"
                                             "RECOMMENDATION:\n"
                                             "Enable scanning on frequency entries in Frequency Manager\n"
                                             "for optimized two-tier scanning with FFT-based signal detection.\n"
                                             "\n"
                                             "This provides significantly faster scanning while maintaining\n"
                                             "the same thoroughness and signal detection capability.");
                        }
                    }
                    
                    // COVERAGE RECOMMENDATION: Display intelligent optimization suggestions
                    if (!coverage.recommendation.empty()) {
                        ImGui::Spacing();
                        // Color-code recommendations based on content
                        ImVec4 recommendationColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f); // Green for good/optimal
                        if (coverage.recommendation.find("gaps") != std::string::npos || 
                            coverage.recommendation.find("Low coverage") != std::string::npos) {
                            recommendationColor = ImVec4(0.8f, 0.6f, 0.2f, 1.0f); // Orange for warnings
                        } else if (coverage.recommendation.find("overlap") != std::string::npos ||
                                   coverage.recommendation.find("Excessive") != std::string::npos) {
                            recommendationColor = ImVec4(0.2f, 0.6f, 0.8f, 1.0f); // Blue for optimization
                        } else if (coverage.recommendation.find("resolution") != std::string::npos ||
                                   coverage.recommendation.find("FFT") != std::string::npos) {
                            recommendationColor = ImVec4(0.8f, 0.4f, 0.8f, 1.0f); // Purple for technical issues
                        }
                        
                        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                        ImGui::TextColored(recommendationColor, "RECOMMENDATION: %s", coverage.recommendation.c_str());
                        ImGui::PopTextWrapPos();
                        
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("COVERAGE OPTIMIZATION RECOMMENDATIONS\n"
                                             "\n"
                                             "These suggestions are based on real-time analysis of your\n"
                                             "current scanning configuration, including:\n"
                                             "- Actual FFT size and sample rate from your SDR\n"
                                             "- Real VFO bandwidth from radio settings\n"
                                             "- Current frequency manager entries and profiles\n"
                                             "- Calculated gaps, overlaps, and coverage percentages\n"
                                             "\n"
                                             "Follow these recommendations to optimize your scanning\n"
                                             "for the best balance of speed vs signal detection.");
                        }
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", coverage.recommendation.c_str());
                }
            } catch (...) {
                ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Coverage analysis error - disabling");
                enableCoverageAnalysis = false;
            }
        } else {
            // Show safe status messages without any component access
            if (!currentSdrRunning) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Start SDR to enable coverage analysis");
            } else {
                int remaining = 300 - stableFrames;
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "SDR stabilizing... (%d frames remaining)", remaining);
            }
        }

        ImGui::BeginTable(("scanner_bottom_btn_table" + _this->name).c_str(), 2);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        
        // INSTANT VISUAL FEEDBACK: Use immediate state for responsiveness
        
        // Left button (decreasing frequency)
        bool leftSelected = !_this->scanUp;
        if (leftSelected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f)); // Blue highlight
        }
        if (ImGui::Button(("<<##scanner_back_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // INSTANT: No locks, no blocking operations
            _this->reverseLock = true;
            _this->receiving = false;
            _this->scanUp = false;
            _this->configNeedsSave = true;
            _this->applyMuteWhileScanning(); // Mute while resuming scanning
        }
        if (leftSelected) {
            ImGui::PopStyleColor(); // Safe: matches the PushStyleColor above
        }
        
        ImGui::TableSetColumnIndex(1);
        
        // Right button (increasing frequency)
        bool rightSelected = _this->scanUp;
        if (rightSelected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f)); // Blue highlight
        }
        if (ImGui::Button((">>##scanner_forw_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // INSTANT: No locks, no blocking operations
            _this->reverseLock = true;
            _this->receiving = false;
            _this->scanUp = true;
            _this->configNeedsSave = true;
            _this->applyMuteWhileScanning(); // Mute while resuming scanning
        }
        if (rightSelected) {
            ImGui::PopStyleColor(); // Safe: matches the PushStyleColor above
        }
        ImGui::EndTable();

        if (!_this->running) {
            // Check if radio source is running
            bool sourceRunning = gui::mainWindow.sdrIsRunning();
            
            if (!sourceRunning) {
                // Disable button and show warning when source is stopped
                style::beginDisabled();
            }
            
            if (ImGui::Button("Start##scanner_start", ImVec2(menuWidth, 0))) {
                _this->start();
            }
            
            if (!sourceRunning) {
                style::endDisabled();
                // Show warning status
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Status: Radio source not running");
            } else {
            ImGui::Text("Status: Idle");
            }
        }
        else {
            ImGui::BeginTable(("scanner_control_table" + _this->name).c_str(), 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button("Stop##scanner_start", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                _this->stop();
            }
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button("Reset##scanner_reset", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                _this->reset();
            }
            ImGui::EndTable();
            
            if (_this->receiving) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: Receiving");
            }
            else if (_this->tuning) {
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Status: Tuning");
            }
            else {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Status: Scanning");
            }
        }
        
        // PERFORMANCE FIX: Delayed config saving for responsive UI
        if (_this->configNeedsSave) {
            _this->configNeedsSave = false;
            _this->saveConfig(); // Save in background, doesn't block UI
        }
        
        // === AUTO-RECORDING CONTROLS ===
        ImGui::Spacing();
        ImGui::Text("Auto Recording");
        ImGui::Separator();
        
        if (ImGui::Checkbox("Auto Record##scanner_auto_record", &_this->autoRecord)) {
            _this->saveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically record detected signals to separate files");
        }
        
        if (_this->autoRecord) {
            ImGui::LeftLabel("Recording Path");
            if (_this->autoRecordFolderSelect.render("##scanner_record_path")) {
                _this->saveConfig();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Directory where recording files will be saved");
            }
            
            ImGui::LeftLabel("Min Duration (s)");
            if (ImGui::PrecisionSliderFloat(("##scanner_min_duration_" + _this->name).c_str(), &_this->autoRecordMinDuration, 1, 60, "%.0f")) {
                flog::info("Scanner: Min duration changed to {}s", _this->autoRecordMinDuration);
                _this->saveConfig();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Minimum recording duration in seconds\nRecordings shorter than this will be deleted");
            }
            
            // Recording status
            const char* statusLabels[] = {"Disabled", "Waiting for Signal", "Recording", "Suspended (Manual)"};
            const ImVec4 statusColors[] = {
                ImVec4(0.5f, 0.5f, 0.5f, 1.0f), // Gray for disabled
                ImVec4(1.0f, 1.0f, 0.0f, 1.0f), // Yellow for waiting
                ImVec4(0.0f, 1.0f, 0.0f, 1.0f), // Green for recording
                ImVec4(1.0f, 0.5f, 0.0f, 1.0f)  // Orange for suspended
            };
            
            ImGui::LeftLabel("Status");
            int statusIndex = (int)_this->recordingControlState;
            if (statusIndex >= 0 && statusIndex < 4) {
                ImGui::TextColored(statusColors[statusIndex], "%s", statusLabels[statusIndex]);
                if (_this->recordingControlState == RECORDING_ACTIVE) {
                    ImGui::SameLine();
                    ImGui::Text("(%.1f MHz)", _this->recordingFrequency / 1e6);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Recording with %.0fs minimum duration\n(Captured when recording started)", _this->recordingMinDurationCapture);
                    }
                }
            }
            
            ImGui::LeftLabel("Files Today");
            ImGui::Text("%d", _this->recordingFilesCount);
            ImGui::SameLine();
            if (ImGui::Button("Reset##files_today_reset")) {
                _this->resetFilesTodayCounter();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reset the daily file counter to 0\n(Counter also resets automatically at midnight)");
            }
        }
        
        // === SCAN LOGGING CONTROLS ===
        ImGui::Spacing();
        ImGui::Text("Scan Logging");
        ImGui::Separator();
        
        if (ImGui::Checkbox("Enable Logging##scanner_enable_logging", &_this->enableScanLogging)) {
            _this->saveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Log detected frequencies to CSV file for analysis\nFormat: Frequency_Hz, Signal_dBFS, Start_Timestamp, End_Timestamp, Duration_Seconds, Frequency_MHz");
        }
        
        if (_this->enableScanLogging) {
            ImGui::LeftLabel("Log File Path");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX() - 60);
            
            // Create a buffer for the input text (ImGui needs a mutable char array)
            static char logPathBuffer[512];
            if (_this->scanLogPath.length() < sizeof(logPathBuffer) - 1) {
                strcpy(logPathBuffer, _this->scanLogPath.c_str());
            }
            
            if (ImGui::InputText("##scanner_log_path", logPathBuffer, sizeof(logPathBuffer))) {
                _this->scanLogPath = std::string(logPathBuffer);
                _this->saveConfig();
            }
            ImGui::SameLine();
            if (ImGui::Button("...##scanner_log_browse")) {
                // Open file save dialog for CSV log file
                if (_this->logFileDialog) {
                    delete _this->logFileDialog;
                }
                _this->logFileDialog = new pfd::save_file("Save Scanner Log", _this->scanLogPath, 
                    { "CSV Files (*.csv)", "*.csv", "All Files", "*" });
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Browse for log file location");
            }
            
            // Handle file dialog result
            if (_this->logFileDialog && _this->logFileDialog->ready()) {
                std::string result = _this->logFileDialog->result();
                if (!result.empty()) {
                    _this->scanLogPath = result;
                    // Update the buffer for the input text
                    if (_this->scanLogPath.length() < sizeof(logPathBuffer) - 1) {
                        strcpy(logPathBuffer, _this->scanLogPath.c_str());
                    }
                    _this->saveConfig();
                }
                delete _this->logFileDialog;
                _this->logFileDialog = nullptr;
            }
            
            // Minimum duration filter with immediate effect
            ImGui::LeftLabel("Min Duration (ms)");
            if (ImGui::SliderInt(("##scanner_min_duration_ms_" + _this->name).c_str(), &_this->scanLogMinDurationMs, 10, 10000, "%d ms")) {
                _this->saveConfig();
                flog::info("Scanner: Log duration filter changed to {}ms (immediate effect)", _this->scanLogMinDurationMs);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Minimum transmission duration to log (milliseconds)\n"
                                 "Transmissions shorter than this will be ignored\n"
                                 "Changes take immediate effect during scanning\n"
                                 "Range: 10ms - 10000ms (10 seconds)");
            }
            
            // Show current log file status
            if (_this->running && _this->enableScanLogging) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Logging Active");
                if (_this->isTrackingTransmission) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "(Tracking Signal)");
                }
            } else if (_this->enableScanLogging) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Logging Ready");
            }
            
            // Show effective log path
            std::string effectivePath = _this->scanLogPath.empty() ? "scanner_log.csv" : _this->scanLogPath;
            ImGui::LeftLabel("Effective Path");
            ImGui::TextWrapped("%s", effectivePath.c_str());
        }
        
        // Draw signal analysis tooltip near VFO if enabled and signal detected
        _this->drawSignalTooltip();
    }

    void start() {
        if (running) { flog::warn("Scanner: Already running"); return; }
        // Start logger if enabled
        if (enableScanLogging) {
            std::string path = scanLogPath.empty() ? "scanner_log.csv" : scanLogPath;
            scannerLogger.start(path);
            flog::info("Scanner: Logging enabled to {}", path);
        }
        
        // THREAD SAFETY: Clean up any existing thread before starting new one
        if (workerThread.joinable()) {
            flog::info("Scanner: Cleaning up previous worker thread");
            try {
                // Try to join with a short timeout
                auto future = std::async(std::launch::async, [this]() {
                    workerThread.join();
                });
                if (future.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout) {
                    flog::warn("Scanner: Previous thread cleanup timed out, detaching");
                    workerThread.detach();
                }
            } catch (const std::exception& e) {
                flog::error("Scanner: Exception during thread cleanup: {}", e.what());
                workerThread.detach();
            }
        }
        
        // SAFETY CHECK: Ensure radio source is running before starting scanner
        if (!gui::mainWindow.sdrIsRunning()) {
            flog::error("Scanner: Cannot start scanning - radio source is not running");
            return; 
        }
        
        // Validate scanner state before starting
        if (gui::waterfall.selectedVFO.empty()) {
            flog::error("Scanner: No VFO selected, cannot start scanning");
            return;
        }
        
        // Initialize scanning parameters
        current = startFreq;
        tuning = false;
        receiving = false;
        currentEntryIsSingleFreq = false; // Default to band-style detection
        
        // MUTE WHILE SCANNING: Apply mute when scanner starts
        applyMuteWhileScanning();
        
        flog::info("Scanner: Starting scanner from {:.3f} MHz", (double)(current / 1e6));
        
        running = true;
        
        // Apply gain setting for the initial scanning range
        if (!frequencyRanges.empty()) {
            try {
                applyCurrentRangeGain();
            } catch (const std::exception& e) {
                flog::error("Scanner: Exception applying initial gain: {}", e.what());
                // Continue anyway, gain is not critical for basic operation
            }
        }
        
        // Start worker thread
        try {
            workerThread = std::thread(&ScannerModule::worker, this);
            flog::info("Scanner: Worker thread started successfully");
        } catch (const std::exception& e) {
            flog::error("Scanner: Failed to start worker thread: {}", e.what());
            running = false;
            throw;
        }
    }

    void stop() {
        // Log any ongoing transmission before stopping
        logTransmissionEnd();
        
        // Stop logger
        if (enableScanLogging) {
            scannerLogger.stop();
            flog::info("Scanner: Logging stopped");
        }
        if (!running) { return; }
        
        flog::info("Scanner: Stop requested, setting running=false");
        running = false;
        
        // AUTO-RECORDING: Stop any active recording when scanner stops
        if (autoRecord && recordingControlState == RECORDING_ACTIVE) {
            stopAutoRecording();
        }
        
        // Restore squelch level if modified
        if (squelchDeltaActive) {
            restoreSquelchLevel();
        }
        
        // SIGNAL ANALYSIS: Clear signal info when scanner stops
        if (showSignalInfo) {
            lastSignalStrength = -100.0f;
            lastSignalSNR = 0.0f;
            lastSignalFrequency = 0.0;
            showSignalTooltip = false;  // Hide tooltip
        }
        
        // MUTE WHILE SCANNING: Restore squelch when scanner stops
        restoreMuteWhileScanning();
        
        // THREAD SAFETY: Don't join thread from UI thread to prevent deadlock
        // The worker thread will exit on its own when it sees running=false
        // We'll clean up the thread in the destructor or when starting a new scan
        flog::info("Scanner: Stop completed (thread will exit asynchronously)");
    }

    void reset() {
        std::lock_guard<std::mutex> lck(scanMtx);
        
        // Log any ongoing transmission before resetting
        logTransmissionEnd();
        
        current = startFreq;
        receiving = false;
        
        // AUTO-RECORDING: Stop any active recording when scanner resets
        if (autoRecord && recordingControlState == RECORDING_ACTIVE) {
            stopAutoRecording();
        }
        tuning = false;
        reverseLock = false;
        
        // SIGNAL ANALYSIS: Clear signal info on reset
        if (showSignalInfo) {
            lastSignalStrength = -100.0f;
            lastSignalSNR = 0.0f;
            lastSignalFrequency = 0.0;
            showSignalTooltip = false;  // Hide tooltip
        }
        
        // Reset squelch delta state
        if (squelchDeltaActive) {
            restoreSquelchLevel();
        }
        
        // MUTE WHILE SCANNING: Restore squelch when scanner is reset
        restoreMuteWhileScanning();
        
        flog::warn("Scanner: Reset to start frequency {:.0f} Hz", (double)startFreq);
    }

    void saveConfig() {
        config.acquire();
        
        // Save legacy single range (for backward compatibility)
        config.conf["startFreq"] = startFreq;
        config.conf["stopFreq"] = stopFreq;
        
        // Save common scanner parameters
        config.conf["interval"] = interval;
        config.conf["passbandRatio"] = passbandRatio;
        config.conf["tuningTime"] = tuningTime;
        config.conf["lingerTime"] = lingerTime;
        config.conf["level"] = level;
        config.conf["blacklistTolerance"] = blacklistTolerance;
        config.conf["scanUp"] = scanUp; // Save scanning direction preference
        config.conf["blacklistedFreqs"] = blacklistedFreqs;
        
        // Save squelch delta settings
        config.conf["squelchDelta"] = squelchDelta;
        config.conf["squelchDeltaAuto"] = squelchDeltaAuto;
        config.conf["muteWhileScanning"] = muteWhileScanning;
        config.conf["aggressiveMute"] = aggressiveMute;
        config.conf["aggressiveMuteLevel"] = aggressiveMuteLevel;
        config.conf["showSignalInfo"] = showSignalInfo;
        config.conf["showSignalTooltip"] = showSignalTooltip;
        config.conf["unlockHighSpeed"] = unlockHighSpeed;
        config.conf["tuningTimeAuto"] = tuningTimeAuto;
        config.conf["showTriggerLevel"] = showTriggerLevel;
        
        // Save frequency ranges
        json rangesArray = json::array();
        for (const auto& range : frequencyRanges) {
            json rangeJson;
            rangeJson["name"] = range.name;
            rangeJson["startFreq"] = range.startFreq;
            rangeJson["stopFreq"] = range.stopFreq;
            rangeJson["enabled"] = range.enabled;
            rangeJson["gain"] = range.gain;
            rangesArray.push_back(rangeJson);
        }
        config.conf["frequencyRanges"] = rangesArray;
        config.conf["currentRangeIndex"] = currentRangeIndex;
        
        // Save frequency manager integration settings
        // NOTE: useFrequencyManager and applyProfiles are now always enabled (no longer configurable)
        config.conf["scanRateHz"] = scanRateHz;
        // Scanner logging config
        config.conf["enableScanLogging"] = enableScanLogging;
        config.conf["scanLogPath"] = scanLogPath;
        config.conf["scanLogMinDurationMs"] = scanLogMinDurationMs;
        
        // Save auto-recording settings
        config.conf["autoRecord"] = autoRecord;
        config.conf["autoRecordMinDuration"] = autoRecordMinDuration;
        config.conf["recordingFilesCount"] = recordingFilesCount;
        config.conf["recordingSequenceNum"] = recordingSequenceNum;
        config.conf["lastResetDate"] = lastResetDate;
        config.conf["autoRecordPath"] = autoRecordFolderSelect.path;
        config.conf["autoRecordNameTemplate"] = std::string(autoRecordNameTemplate);
        
        config.release(true);
    }

    void loadConfig() {
        config.acquire();
        // Existing load code ...
        enableScanLogging = config.conf.value("enableScanLogging", false);
        scanLogPath = config.conf.value("scanLogPath", "");
        scanLogMinDurationMs = config.conf.value("scanLogMinDurationMs", 100);
        config.release();
        // Rest of existing loadConfig continues
        config.acquire();
        startFreq = config.conf.value("startFreq", 88000000.0);
        stopFreq = config.conf.value("stopFreq", 108000000.0);
        interval = std::clamp(config.conf.value("interval", 100000.0), 1000.0, 500000.0);  // Guardrails: 1 kHz - 500 kHz (matches UI)
        passbandRatio = config.conf.value("passbandRatio", 100.0);
        tuningTime = config.conf.value("tuningTime", 250);
        lingerTime = config.conf.value("lingerTime", 1000.0);
        level = config.conf.value("level", -50.0);
        blacklistTolerance = config.conf.value("blacklistTolerance", 1000.0);
        scanUp = config.conf.value("scanUp", true); // Load scanning direction preference
        if (config.conf.contains("blacklistedFreqs")) {
            blacklistedFreqs = config.conf["blacklistedFreqs"].get<std::vector<double>>();
        }
        
        // Load squelch delta settings
        squelchDelta = config.conf.value("squelchDelta", 2.5f);
        squelchDeltaAuto = config.conf.value("squelchDeltaAuto", false);
        muteWhileScanning = config.conf.value("muteWhileScanning", true);
        aggressiveMute = config.conf.value("aggressiveMute", true);
        aggressiveMuteLevel = config.conf.value("aggressiveMuteLevel", -3.0f);
        showSignalInfo = config.conf.value("showSignalInfo", false);
        showSignalTooltip = config.conf.value("showSignalTooltip", false);
        unlockHighSpeed = config.conf.value("unlockHighSpeed", false);
        tuningTimeAuto = config.conf.value("tuningTimeAuto", false);
        showTriggerLevel = config.conf.value("showTriggerLevel", true);
        
        // Initialize time points
        lastNoiseUpdate = std::chrono::high_resolution_clock::now();
        tuneTime = std::chrono::high_resolution_clock::now();
        lastSignalAnalysisTime = std::chrono::high_resolution_clock::now();
        
        // Load frequency ranges if they exist (BEFORE releasing config!)
        if (config.conf.contains("frequencyRanges") && config.conf["frequencyRanges"].is_array()) {
            frequencyRanges.clear();
            for (const auto& rangeJson : config.conf["frequencyRanges"]) {
                if (rangeJson.contains("name") && rangeJson.contains("startFreq") && 
                    rangeJson.contains("stopFreq") && rangeJson.contains("enabled")) {
                    float gain = 20.0f; // Default gain for backward compatibility
                    if (rangeJson.contains("gain")) {
                        gain = rangeJson["gain"].get<float>();
                    }
                    frequencyRanges.emplace_back(
                        rangeJson["name"].get<std::string>(),
                        rangeJson["startFreq"].get<double>(),
                        rangeJson["stopFreq"].get<double>(),
                        rangeJson["enabled"].get<bool>(),
                        gain
                    );
                }
            }
            if (config.conf.contains("currentRangeIndex")) {
                currentRangeIndex = config.conf["currentRangeIndex"].get<int>();
                int maxIndex = std::max(0, (int)frequencyRanges.size() - 1);
                currentRangeIndex = std::clamp<int>(currentRangeIndex, 0, maxIndex);
            }
        }
        
        // Load frequency manager integration settings
        // NOTE: useFrequencyManager and applyProfiles are now always true (simplified UI)
        scanRateHz = config.conf.value("scanRateHz", 25);
        
        // Load auto-recording settings
        autoRecord = config.conf.value("autoRecord", false);
        autoRecordMinDuration = config.conf.value("autoRecordMinDuration", 5.0f);
        flog::info("Scanner: Loaded autoRecordMinDuration = {}s", autoRecordMinDuration);
        recordingFilesCount = config.conf.value("recordingFilesCount", 0);
        recordingSequenceNum = config.conf.value("recordingSequenceNum", 1);
        lastResetDate = config.conf.value("lastResetDate", "");
        
        if (config.conf.contains("autoRecordPath")) {
            autoRecordFolderSelect.setPath(config.conf["autoRecordPath"]);
        }
        
        if (config.conf.contains("autoRecordNameTemplate")) {
            std::string nameTemplate = config.conf["autoRecordNameTemplate"];
            if (nameTemplate.length() < sizeof(autoRecordNameTemplate)) {
                strcpy(autoRecordNameTemplate, nameTemplate.c_str());
            }
        }
        
        config.release();
        
        // Ensure current frequency is within bounds
        double currentStart, currentStop;
        if (getCurrentScanBounds(currentStart, currentStop)) {
            if (current < currentStart || current > currentStop) {
                current = currentStart;
            }
        } else {
            // Fall back to legacy range
            if (current < startFreq || current > stopFreq) {
                current = startFreq;
            }
        }
        
        // Initialize passband index to match loaded ratio value
        initializePassbandIndex();
    }

    void worker() {
        flog::info("Scanner: Worker thread started");
        try {
            // Initialize timer for sleep_until to reduce drift
            auto nextWakeTime = std::chrono::steady_clock::now();
            
            // Check for midnight reset on startup
            checkMidnightReset();
            
            // Track midnight reset checks (check every ~10 minutes during scanning)
            auto lastMidnightCheck = std::chrono::steady_clock::now();
            const auto midnightCheckInterval = std::chrono::minutes(10);
            
            // PERFORMANCE-CRITICAL: Configurable scan rate (consistent across all modes)
            while (running) {
                // Periodic midnight reset check (every 10 minutes)
                auto now = std::chrono::steady_clock::now();
                if (now - lastMidnightCheck >= midnightCheckInterval) {
                    checkMidnightReset();
                    lastMidnightCheck = now;
                }
                    // Implement actual scan rate control with different max based on unlock status
                    // Safety guard against division by zero and enforce limits
                    const int maxHz = unlockHighSpeed ? MAX_SCAN_RATE : NORMAL_MAX_SCAN_RATE;
                    const int safeRate = std::clamp(scanRateHz, MIN_SCAN_RATE, maxHz);
                    const int intervalMs = std::max(1, 1000 / safeRate);
                    
                    // Use sleep_until with steady_clock to reduce drift and jitter
                    
                    // Dynamically scale tuning time based on scan rate when auto mode is enabled
                    // This ensures we don't wait too long between frequencies
                    // Formula: tuningTime = BASE_TUNING_TIME * (BASE_SCAN_RATE / currentScanRate)
                    // This creates an inverse relationship - faster scanning = shorter tuning time
                    
                    // Only recalculate when auto mode is on and scan rate changes
                    static int lastAdjustedRate = 0;
                    if (tuningTimeAuto && safeRate != lastAdjustedRate) {
                        // Calculate optimal tuning time that scales with scan rate
                        const int optimalTime = std::max(
                            MIN_TUNING_TIME, 
                            static_cast<int>((BASE_TUNING_TIME * BASE_SCAN_RATE) / static_cast<int>(safeRate))
                        );
                        
                        // Only adjust if current tuning time is significantly different
                        if (std::abs(tuningTime - optimalTime) > 10) {
                            tuningTime = optimalTime;
                            flog::info("Scanner: Auto-scaled tuning time to {}ms for {}Hz scan rate", 
                                      tuningTime, safeRate);
                        }
                        lastAdjustedRate = safeRate;
                    }
                    
                    // Add debug logging to verify the actual scan rate being used
                    // Log status every 500ms instead of counting iterations
                    static Throttle statusLogThrottle{std::chrono::milliseconds(500)};
                    if (statusLogThrottle.ready()) {
                        SCAN_DEBUG("Scanner: Current scan rate: {} Hz (interval: {} ms, tuning time: {} ms)", 
                                 safeRate, intervalMs, tuningTime);
                    }
                    
                    // Sleep until next scheduled time to reduce drift
                    // Reset nextWakeTime if we've fallen too far behind to prevent catch-up bursts
                    auto sleepNow = std::chrono::steady_clock::now();
                    if (nextWakeTime + std::chrono::milliseconds(2*intervalMs) < sleepNow) {
                        nextWakeTime = sleepNow;
                    }
                    nextWakeTime += std::chrono::milliseconds(intervalMs);
                    std::this_thread::sleep_until(nextWakeTime);
                
                try {
                    std::lock_guard<std::mutex> lck(scanMtx);
                    auto now = std::chrono::high_resolution_clock::now();

                // SAFETY CHECK: Stop scanner if radio source is stopped
                if (!gui::mainWindow.sdrIsRunning()) {
                    flog::warn("Scanner: Radio source stopped, stopping scanner");
                    running = false;
                    return;
                }

                // Enforce tuning
                if (gui::waterfall.selectedVFO.empty()) {
                    running = false;
                    return;
                }
                
                // Get current range bounds (supports multi-range and legacy single range)
                double currentStart, currentStop;
                if (!getCurrentScanBounds(currentStart, currentStop)) {
                    flog::warn("Scanner: No active frequency ranges, stopping");
                    running = false;
                    return;
                }
                
                // Ensure current frequency is within bounds
                // PERFORMANCE-CRITICAL: Ensure current frequency is within bounds (legacy mode only)
                if (!useFrequencyManager && (current < currentStart || current > currentStop)) {
                    flog::warn("Scanner: Current frequency {:.0f} Hz out of bounds, resetting to start", (double)current);
                    current = currentStart;
                }
                // Record tuning time for debounce
                tuneTime = std::chrono::high_resolution_clock::now();
                
                // Apply squelch delta preemptively when tuning to new frequency
                // This prevents the initial noise burst when jumping between bands
                // Apply only when not during startup
                if (squelchDelta > 0.0f && !squelchDeltaActive && running) {
                    applySquelchDelta();
                }
                

                // ENHANCED MUTE: Ensure silence during frequency changes
                ensureMuteDuringOperation();
                tuner::normalTuning(gui::waterfall.selectedVFO, current);

                // Check if we are waiting for a tune
                if (tuning) {
                    SCAN_DEBUG("Scanner: Tuning in progress...");
                    auto timeSinceLastTune = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime);
                    if (timeSinceLastTune.count() > tuningTime) {
                        tuning = false;
                        SCAN_DEBUG("Scanner: Tuning completed");
                    }
                    continue;
                }

                // PERFORMANCE FIX: Minimize FFT lock time by copying data immediately
                int dataWidth = 0;
                static std::vector<float> fftDataCopy; // Reusable buffer to avoid allocations
                
                // Acquire RAW FFT data (FIXED: now gets correct currentFFTLine offset)
                int rawFFTSize;
                float* rawData = gui::waterfall.acquireRawFFT(rawFFTSize);
                if (!rawData || rawFFTSize <= 0) { 
                    if (rawData) gui::waterfall.releaseRawFFT();
                    continue; // No FFT data available, try again
                }
                
                // Copy raw FFT data immediately to minimize lock time
                static std::vector<float> rawFFTCopy;
                rawFFTCopy.resize(rawFFTSize);
                memcpy(rawFFTCopy.data(), rawData, rawFFTSize * sizeof(float));
                gui::waterfall.releaseRawFFT(); // CRITICAL: Release lock immediately
                
                // ZOOM-INDEPENDENT processing: Always use FULL spectrum regardless of zoom
                double wholeBandwidth = gui::waterfall.getBandwidth();
                
                // CRITICAL: Ignore view parameters for zoom-independence
                double viewOffset = 0.0;                    // Always process full spectrum
                double viewBandwidth = wholeBandwidth;       // Always use full bandwidth
                
                double offsetRatio = viewOffset / (wholeBandwidth / 2.0);  // = 0.0
                int drawDataSize = (viewBandwidth / wholeBandwidth) * rawFFTSize;  // = rawFFTSize
                int drawDataStart = (((double)rawFFTSize / 2.0) * (offsetRatio + 1)) - (drawDataSize / 2);  // = 0
                
                // Target reasonable dataWidth for processing
                dataWidth = std::min(2048, std::max(256, rawFFTSize / 4));
                if (dataWidth > rawFFTSize) dataWidth = rawFFTSize;
                
                // ZOOM-INDEPENDENT: Simple decimation of full raw FFT
                static std::vector<float> processedFFT;
                processedFFT.resize(dataWidth);
                
                // Simple peak detection: decimate rawFFTSize to dataWidth
                float factor = (float)rawFFTSize / (float)dataWidth;
                
                for (int i = 0; i < dataWidth; i++) {
                    float maxVal = -INFINITY;
                    int startIdx = (int)(i * factor);
                    int endIdx = (int)((i + 1) * factor);
                    if (endIdx > rawFFTSize) endIdx = rawFFTSize;
                    
                    // Find peak in this group
                    for (int j = startIdx; j < endIdx; j++) {
                        if (rawFFTCopy[j] > maxVal) { 
                            maxVal = rawFFTCopy[j]; 
                        }
                    }
                    processedFFT[i] = maxVal;
                }
                

                
                float* data = processedFFT.data();
                
                // Use FULL BANDWIDTH coordinates (zoom-independent)
                double wfCenter = gui::waterfall.getCenterFrequency();
                double wfWidth = wholeBandwidth;  // ZOOM-INDEPENDENT: Use full bandwidth
                double wfStart = wfCenter - (wfWidth / 2.0);
                double wfEnd = wfCenter + (wfWidth / 2.0);

                // ADAPTIVE SIGNAL DETECTION: Use different tolerances for single freq vs bands
                double baseVfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                double effectiveVfoWidth;
                
                if (useFrequencyManager && currentEntryIsSingleFreq) {
                    // Single frequency: Use tight tolerance (5 kHz) to ignore nearby signals
                    effectiveVfoWidth = 5000.0; // 5 kHz window for single frequencies
                    static bool loggedSingleFreq = false;
                    if (!loggedSingleFreq) {
                        flog::info("Scanner: Single frequency mode - using 5 kHz tolerance (ignoring nearby signals)");
                        loggedSingleFreq = true;
                    }
                } else {
                    // Band or legacy scanning: Use full VFO bandwidth for wide signal detection 
                    effectiveVfoWidth = baseVfoWidth;
                    static bool loggedBandMode = false;
                    if (!loggedBandMode && useFrequencyManager) {
                        flog::info("Scanner: Band scanning mode - using full VFO bandwidth ({:.1f} kHz) for signal detection", (double)(baseVfoWidth / 1000.0));
                        loggedBandMode = true;
                    }
                }

                if (receiving) {
                    SCAN_DEBUG("Scanner: Receiving signal...");

                    float maxLevel = getMaxLevel(data, current, effectiveVfoWidth, dataWidth, wfStart, wfWidth);
                    if (maxLevel >= level) {
                         // Update noise floor when signal is present
                        if (squelchDeltaAuto) {
                            updateNoiseFloor(maxLevel - 15.0f); // Estimate noise floor as 15dB below signal
                        }
                        
                        // Apply squelch delta when receiving strong signal
                        if (!squelchDeltaActive && squelchDelta > 0.0f && running) {
                            applySquelchDelta();
                        }
                        
                        // CONTINUOUS CENTERING: Re-center on signal peak every 100ms while receiving
                        static auto lastCenteringTime = std::chrono::high_resolution_clock::now();
                        auto timeSinceLastCentering = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCenteringTime);
                        
                        if (timeSinceLastCentering.count() >= 100) { // Every 100ms
                            printf("timeSinceLastCentering (%lld ms)", (long long)timeSinceLastCentering.count());
                            
                            // Calculate dynamic centering threshold based on current tuning profile bandwidth
                            double centeringThreshold = 25000.0; // Default fallback
                            if (useFrequencyManager && currentTuningProfile) {
                                const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                                if (profile && profile->bandwidth > 0) {
                                    centeringThreshold = 5.0 * profile->bandwidth; // 5x the bandwidth
                                }
                            }
                            
                            // Create a SMALL centering window around current frequency (NOT the entire scan range!)
                            double centeringStart = current - centeringThreshold;
                            double centeringStop = current + centeringThreshold;

                            double peakFreq = findSignalPeakHighRes(current, maxLevel, effectiveVfoWidth, wfStart, wfWidth, centeringStart, centeringStop, level);

                            // Only update frequency if peak is within reasonable distance and adjustment is significant
                            if (std::abs(peakFreq - current) <= centeringThreshold && std::abs(peakFreq - current) > 100.0) {
                                 current = peakFreq;
                                tuner::normalTuning(gui::waterfall.selectedVFO, current);
                            } else {
                                SCAN_DEBUG("Scanner: No centering needed (drift: %.1f Hz, threshold: %.1f Hz)\n", peakFreq - current, centeringThreshold);
                            }
                            
                            lastCenteringTime = now;
                        }
                        
                        lastSignalTime = now;
                    }
                    else {
                        auto timeSinceLastSignal = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime);
                        if (timeSinceLastSignal.count() > lingerTime) {
                            // Restore original squelch level when we leave receiving state
                            if (squelchDeltaActive) {
                                restoreSquelchLevel();
                            }
                            
                            receiving = false;
                            SCAN_DEBUG("Scanner: Signal lost, resuming scanning");
                            
                            // Log transmission end for duration tracking
                            logTransmissionEnd();
                            
                            // AUTO-RECORDING: Stop recording when signal lost (linger time expired)
                            if (autoRecord && recordingControlState == RECORDING_ACTIVE) {
                                stopAutoRecording();
                            }
                            
                            // SIGNAL ANALYSIS: Clear signal info when signal is lost
                            if (showSignalInfo) {
                                lastSignalStrength = -100.0f;
                                lastSignalSNR = 0.0f;
                                lastSignalFrequency = 0.0;
                                showSignalTooltip = false;  // Hide tooltip
                            }
                            
                            // MUTE WHILE SCANNING: Apply mute when resuming scanning after signal loss
                            applyMuteWhileScanning();
                        }
                    }
                }
                else {
                    flog::warn("Seeking signal");
                    double bottomLimit = current;
                    double topLimit = current;
                    
                    // ADAPTIVE SIGNAL SEARCH: Different behavior for single freq vs band scanning
                    if (useFrequencyManager && currentEntryIsSingleFreq) {
                        // SINGLE FREQUENCY: Only check signal at exact current frequency (no scanning)
                        float maxLevel = getMaxLevel(data, current, effectiveVfoWidth, dataWidth, wfStart, wfWidth);
                        if (maxLevel >= level) {
                            // SIGNAL CENTERING: Even for single frequencies, center on the peak for optimal reception
                            double currentStart, currentStop;
                            if (getCurrentScanBounds(currentStart, currentStop)) {
                                double peakFreq = findSignalPeak(current, maxLevel, effectiveVfoWidth, data, dataWidth, wfStart, wfWidth, currentStart, currentStop, level);
                                
                                // Calculate dynamic centering threshold based on current tuning profile bandwidth
                                double centeringThreshold = 25000.0; // Default fallback
                                if (useFrequencyManager && currentTuningProfile) {
                                    const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                                    if (profile && profile->bandwidth > 0) {
                                        centeringThreshold = 5.0 * profile->bandwidth; // 5x the bandwidth
                                    }
                                }
                                
                                // Only update frequency if peak is within reasonable distance (prevents jumping to different signals)
                                if (std::abs(peakFreq - current) <= centeringThreshold) {
                                    current = peakFreq;
                                }
                            }
                            
                            receiving = true;
                
                // Track transmission start for duration logging
                if (enableScanLogging) {
                    auto now = std::chrono::system_clock::now();
                    if (!isTrackingTransmission) {
                        // Start tracking new transmission
                        currentTransmissionStart = now;
                        currentTransmissionFreq = current;
                        currentTransmissionLevel = maxLevel;
                        isTrackingTransmission = true;
                    }
                }
                            SCAN_DEBUG("Scanner: Setting receiving=true for single frequency signal at %.6f MHz (level: %.1f)\n", current / 1e6, maxLevel);
                            lastSignalTime = now;
                            flog::info("Scanner: Found signal at single frequency {:.6f} MHz (level: {:.1f})", current / 1e6, maxLevel);
                            
                            // AUTO-RECORDING: Start recording when signal detected
                            if (autoRecord) {
                                flog::info("Scanner: Signal detected at {:.3f} MHz, recording state: {}", 
                                          current / 1e6, (int)recordingControlState);
                                if (recordingControlState == RECORDING_IDLE) {
                                    startAutoRecording(current, getCurrentMode());
                                } else if (recordingControlState == RECORDING_ACTIVE) {
                                    // Extend current recording - update frequency if changed significantly
                                    if (std::abs(current - recordingFrequency) > 10000.0) { // 10kHz threshold
                                        flog::info("Scanner: Signal moved from {:.3f} to {:.3f} MHz during recording", 
                                                  recordingFrequency / 1e6, current / 1e6);
                                        recordingFrequency = current; // Update tracked frequency
                                    }
                                }
                            }
                            
                            // SIGNAL ANALYSIS: Calculate and store signal info for display
                            if (showSignalInfo) {
                                float strength, snr;
                                if (calculateCurrentSignalInfo(strength, snr)) {
                                    lastSignalStrength = strength;
                                    lastSignalSNR = snr;
                                    lastSignalFrequency = current;
                                    showSignalTooltip = true;  // Show tooltip near VFO
                                    lastSignalAnalysisTime = std::chrono::high_resolution_clock::now(); // Start 50ms update timer
                                     } else {
                                    // Clear previous data if analysis fails
                                    lastSignalStrength = -100.0f;
                                    lastSignalSNR = 0.0f;
                                    lastSignalFrequency = 0.0;
                                    showSignalTooltip = false;
                                }
                            }
                            
                            // MUTE WHILE SCANNING: Restore audio when signal is found
                            restoreMuteWhileScanning();
                            
                            // TUNING PROFILE APPLICATION: Apply profile when signal found (CRITICAL FIX)
                            if (applyProfiles && currentTuningProfile && !gui::waterfall.selectedVFO.empty()) {
                                const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                                if (profile) {
                                    // Apply profile directly - no emergency mute needed since signal is found
                                    applyTuningProfileSmart(*profile, gui::waterfall.selectedVFO, current, "SIGNAL");
                                }
                            } else {
                                if (applyProfiles && !currentTuningProfile) {
                                    SCAN_DEBUG("Scanner: No profile available for {:.6f} MHz (Index:{})", current / 1e6, (int)currentScanIndex);
                                }
                            }
                            
                            continue; // Signal found, stay on this frequency
                        }
                        // No signal at exact frequency - continue to frequency stepping
                        SCAN_DEBUG("Scanner: No signal at single frequency {:.6f} MHz (level: {:.1f} < {:.1f})", current / 1e6, maxLevel, level);
                    } else {
                        // BAND SCANNING: Search for signals across range using interval stepping
                        if (findSignal(scanUp, bottomLimit, topLimit, wfStart, wfEnd, wfWidth, effectiveVfoWidth, data, dataWidth)) {
                            continue; // Signal found using band scanning
                    }
                    
                    // Search for signal in the inverse scan direction if direction isn't enforced
                    if (!reverseLock) {
                            if (findSignal(!scanUp, bottomLimit, topLimit, wfStart, wfEnd, wfWidth, effectiveVfoWidth, data, dataWidth)) {
                                continue; // Signal found using reverse band scanning
                        }
                    }
                    else { reverseLock = false; }
                    }
                    

                    // There is no signal on the visible spectrum, tune in scan direction and retry
                    // CRITICAL FIX: Use frequency manager integration or legacy frequency stepping
                    if (useFrequencyManager) {
                        // Use frequency manager for frequency stepping
                                        if (!performFrequencyManagerScanning()) {
                            // Fall back to legacy scanning if frequency manager unavailable or has no valid data
                    flog::warn("Scanner: FrequencyManager integration failed, falling back to legacy mode");
                            useFrequencyManager = false; // Switch to legacy mode permanently until restart
                    performLegacyScanning();
                }
                    } else {
                        // Legacy frequency stepping
                        if (scanUp) {
                        current = topLimit + interval;
                            // Handle range wrapping for multi-range scanning
                            if (current > currentStop) {
                                // Move to next range or wrap to beginning of current range
                            if (!frequencyRanges.empty()) {
                                    auto activeRanges = getActiveRangeIndices();
                                    if (!activeRanges.empty()) {
                                        currentRangeIndex = (currentRangeIndex + 1) % activeRanges.size();
                                        if (!getCurrentScanBounds(currentStart, currentStop)) {
                                            current = startFreq; // Fallback
                                        } else {
                                            current = currentStart;
                                            // Apply gain setting for new range
                                            applyCurrentRangeGain();
                                        }
                                    } else {
                                        current = currentStart;
                                    }
                                } else {
                                    // Legacy single range wrapping
                                    while (current > stopFreq) {
                                    current = startFreq + (current - stopFreq - interval);
                                    }
                                    if (current < startFreq) { current = startFreq; }
                                }
                            }
                        }
                        else {
                        current = bottomLimit - interval;
                            // Handle range wrapping for multi-range scanning
                            if (current < currentStart) {
                                // Move to previous range or wrap to end of current range
                            if (!frequencyRanges.empty()) {
                                    auto activeRanges = getActiveRangeIndices();
                                    if (!activeRanges.empty()) {
                                        currentRangeIndex = (currentRangeIndex - 1 + activeRanges.size()) % activeRanges.size();
                                        if (!getCurrentScanBounds(currentStart, currentStop)) {
                                            current = stopFreq; // Fallback
                                        } else {
                                            current = currentStop;
                                            // Apply gain setting for new range
                                            applyCurrentRangeGain();
                                        }
                                    } else {
                                        current = currentStop;
                                    }
                                } else {
                                    // Legacy single range wrapping
                                    while (current < startFreq) {
                                    current = stopFreq - (startFreq - current - interval);
                                    }
                                    if (current > stopFreq) { current = stopFreq; }
                            }
                        }
                    }
                    
                    // Update current range bounds after potential range change
                    getCurrentScanBounds(currentStart, currentStop);
                    
                    // Add debug logging
                    flog::warn("Scanner: Tuned to {:.0f} Hz (range: {:.0f} - {:.0f})", current, currentStart, currentStop);

                    // If the new current frequency is outside the visible bandwidth, wait for retune
                    if (current - (effectiveVfoWidth/2.0) < wfStart || current + (effectiveVfoWidth/2.0) > wfEnd) {
                        lastTuneTime = now;
                        tuning = true;
                    }
                }
                } // End of legacy frequency stepping

                // FFT data already released above after copying
                
                } catch (const std::exception& e) {
                    flog::error("Scanner: Exception in worker loop: {}", e.what());
                    running = false;
                    break;
                } catch (...) {
                    flog::error("Scanner: Unknown exception in worker loop");
                    running = false;
                    break;
                }
            }
        } catch (const std::exception& e) {
            flog::error("Scanner: Critical exception in worker thread: {}", e.what());
            running = false;
        } catch (...) {
            flog::error("Scanner: Critical unknown exception in worker thread");
            running = false;
        }
        
        flog::info("Scanner: Worker thread ended");
    }

    bool findSignal(bool scanDir, double& bottomLimit, double& topLimit, double wfStart, double wfEnd, double wfWidth, double vfoWidth, float* data, int dataWidth) {
        bool found = false;
        double freq = current;
        int maxIterations = 1000; // Prevent infinite loops
        int iterations = 0;
        
        // Get current range bounds
        double currentStart, currentStop;
        if (!getCurrentScanBounds(currentStart, currentStop)) {
            return false; // No valid range
        }
        
        // Calculate max iterations based on range and interval to prevent infinite loops
        maxIterations = static_cast<int>((currentStop - currentStart) / interval) + 10; // Add a small buffer
        iterations = 0;
        
        for (freq += scanDir ? interval : -interval;
            scanDir ? (freq <= currentStop) : (freq >= currentStart);
            freq += scanDir ? interval : -interval) {
            
            iterations++;
            if (iterations > maxIterations) {
                flog::warn("Scanner: Max iterations reached, forcing frequency wrap");
                break;
            }

            // Check if signal is within configured band bounds (not waterfall bounds)
            if (freq < currentStart || freq > currentStop) { break; }

            // Check if frequency is blacklisted
            if (isFrequencyBlacklisted(freq)) {
                continue;
            }

            if (freq < bottomLimit) { bottomLimit = freq; }
            if (freq > topLimit) { topLimit = freq; }
            
            // Check signal level
            float maxLevel = getMaxLevel(data, freq, vfoWidth * (passbandRatio * 0.01f), dataWidth, wfStart, wfWidth);
            if (maxLevel >= level) {
                // Update noise floor estimate with weak signal values when scanning
                if (!squelchDeltaAuto && maxLevel < level - 15.0f) {
                    updateNoiseFloor(maxLevel);
                }
                
                // SIGNAL CENTERING: Find the actual peak of the signal for optimal tuning
                double peakFreq = findSignalPeakHighRes(freq, maxLevel, vfoWidth, wfStart, wfWidth, currentStart, currentStop, level);
                
                found = true;
                receiving = true;
                
                // Track transmission start for duration logging
                if (enableScanLogging) {
                    auto now = std::chrono::system_clock::now();
                    if (!isTrackingTransmission) {
                        // Start tracking new transmission
                        currentTransmissionStart = now;
                        currentTransmissionFreq = current;
                        currentTransmissionLevel = maxLevel;
                        isTrackingTransmission = true;
                    }
                }
                current = peakFreq;
                
                // AUTO-RECORDING: Start recording when signal detected
                if (autoRecord) {
                    flog::info("Scanner: Signal detected at {:.3f} MHz, recording state: {}", 
                              current / 1e6, (int)recordingControlState);
                    if (recordingControlState == RECORDING_IDLE) {
                        startAutoRecording(current, getCurrentMode());
                    } else if (recordingControlState == RECORDING_ACTIVE) {
                        // Extend current recording - update frequency if changed significantly
                        if (std::abs(current - recordingFrequency) > 10000.0) { // 10kHz threshold
                            flog::info("Scanner: Signal moved from {:.3f} to {:.3f} MHz during recording", 
                                      recordingFrequency / 1e6, current / 1e6);
                            recordingFrequency = current; // Update tracked frequency
                        }
                    }
                }
                
                // SIGNAL ANALYSIS: Calculate and store signal info for display
                if (showSignalInfo) {
                    float strength, snr;
                    if (calculateCurrentSignalInfo(strength, snr)) {
                        lastSignalStrength = strength;
                        lastSignalSNR = snr;
                        lastSignalFrequency = current;
                        showSignalTooltip = true;  // Show tooltip near VFO
                        lastSignalAnalysisTime = std::chrono::high_resolution_clock::now(); // Start 50ms update timer
                     } else {
                        // Clear previous data if analysis fails
                        lastSignalStrength = -100.0f;
                        lastSignalSNR = 0.0f;
                        lastSignalFrequency = 0.0;
                        showSignalTooltip = false;
                    }
                }
                
                // MUTE WHILE SCANNING: Restore audio when signal is found in band scanning
                restoreMuteWhileScanning();
                
                // TUNING PROFILE APPLICATION: Apply profile when signal found (CRITICAL FIX)
                if (useFrequencyManager && applyProfiles && currentTuningProfile && !gui::waterfall.selectedVFO.empty()) {
                    const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                    if (profile) {
                        // Apply profile directly - no emergency mute needed since signal is found
                        applyTuningProfileSmart(*profile, gui::waterfall.selectedVFO, freq, "BAND-SIGNAL");
                    }
                } else {
                    if (useFrequencyManager && applyProfiles && !currentTuningProfile) {
                        SCAN_DEBUG("Scanner: No profile available for {:.6f} MHz BAND (Index:{})", freq / 1e6, (int)currentScanIndex);
                    }
                }
                
                break;
            }
        }
        return found;
    }

    // PASSBAND RATIO HELPER: Sync passband index with actual ratio value
    void initializePassbandIndex() {

        // Find closest passband index (only parameter that still uses discrete values)
        passbandIndex = 6; // Default to 100% (recommended starting point)
        double minPassbandDiff = std::abs(passbandRatio - (PASSBAND_VALUES[passbandIndex] / 100.0));
        for (int i = 0; i < PASSBAND_VALUES_COUNT; i++) {
            double diff = std::abs(passbandRatio - (PASSBAND_VALUES[i] / 100.0));
            if (diff < minPassbandDiff) {
                passbandIndex = i;
                minPassbandDiff = diff;
            }
        }
     }
    
    // REMOVED: syncDiscreteValues() - interval and scan rate now use direct input controls
    // Only passband ratio needs discrete values for the percentage slider
    
    // BLACKLIST: Helper function for consistent blacklist checking
    bool isFrequencyBlacklisted(double frequency) const {
        return std::any_of(blacklistedFreqs.begin(), blacklistedFreqs.end(),
                          [frequency, this](double blacklistedFreq) {
                              return std::abs(frequency - blacklistedFreq) < blacklistTolerance;
                          });
    }
    
    // ENHANCED UX: Look up frequency manager entry name for a given frequency
    std::string lookupFrequencyManagerName(double frequency) {
        // Check cache first to avoid excessive interface calls
        auto it = frequencyNameCache.find(frequency);
        if (it != frequencyNameCache.end()) {
            return it->second;
        }
        
        // Cache miss - need to look up the name
        try {
            // Check if frequency manager interface is available
            if (!core::modComManager.interfaceExists("frequency_manager")) {
                frequencyNameCache[frequency] = ""; // Cache the empty result
                return "";
            }
            
            // Use new frequency manager interface to get bookmark name
            const int CMD_GET_BOOKMARK_NAME = 2;
            std::string bookmarkName;
            
            if (!core::modComManager.callInterface("frequency_manager", CMD_GET_BOOKMARK_NAME, 
                                                 const_cast<double*>(&frequency), &bookmarkName)) {
                SCAN_DEBUG("Scanner: Failed to call frequency manager getBookmarkName interface");
                frequencyNameCache[frequency] = ""; // Cache the empty result
                return "";
            }
            
            // Cache the result for future lookups
            frequencyNameCache[frequency] = bookmarkName;
            return bookmarkName;
            
        } catch (const std::exception& e) {
            SCAN_DEBUG("Scanner: Error looking up frequency manager name: {}", e.what());
            frequencyNameCache[frequency] = ""; // Cache the empty result
            return "";
        }
    }
    
    // PERFORMANCE-OPTIMIZED: Smart profile application with caching (prevents redundant operations)
    bool applyTuningProfileSmart(const TuningProfile& profile, const std::string& vfoName, double frequency, const char* context = "Scanner") {
        // PERFORMANCE CHECK: Skip if same profile already applied to same VFO recently
        if (lastAppliedProfile == &profile && 
            lastAppliedVFO == vfoName && 
            std::abs(lastProfileFrequency - frequency) < 1000.0) { // Within 1 kHz
            
            SCAN_DEBUG("{}: SKIPPED redundant profile '{}' for {:.6f} MHz (already applied)", 
                       context, profile.name.empty() ? "Auto" : profile.name, frequency / 1e6);
            return false; // Skipped - no change needed
        }
        
        // Apply the profile using fast method
        bool success = applyTuningProfileFast(profile, vfoName);
        
        if (success) {
            // Cache the successful application
            lastAppliedProfile = &profile;
            lastProfileFrequency = frequency;
            lastAppliedVFO = vfoName;
            
            //flog::info("{}: APPLIED PROFILE '{}' for {:.6f} MHz (Mode:{} BW:{:.1f}kHz Squelch:{}@{:.1f}dB)",
            //          context, profile.name.empty() ? "Auto" : profile.name,
            //          frequency / 1e6, profile.demodMode, profile.bandwidth / 1000.0f,
            //          profile.squelchEnabled ? "ON" : "OFF", profile.squelchLevel);
        } else {
            // CORRUPTION RECOVERY: If profile application fails, invalidate cache to prevent reuse
            flog::warn("{}: Profile application failed for {:.6f} MHz - clearing cache", context, frequency / 1e6);
            lastAppliedProfile = nullptr;
            currentTuningProfile = nullptr; // Clear corrupted pointer
        }
        
        return success;
    }
    
    // Force refresh of frequency manager scan list (fixes profile pointer corruption)
    bool refreshScanList() {
        try {
            flog::warn("Scanner: Detected corrupted profile data, refreshing scan list from frequency manager");
            
            // Clear any cached profile pointers to prevent further corruption
            currentTuningProfile = nullptr;
            
            // Get fresh scan list from frequency manager
            struct ScanEntry {
                double frequency;
                const TuningProfile* profile;
                const FrequencyBookmark* bookmark;
                bool isFromBand;
            };
            const std::vector<ScanEntry>* scanList = nullptr;
            const int CMD_GET_SCAN_LIST = 1;
            
            if (!core::modComManager.callInterface("frequency_manager", CMD_GET_SCAN_LIST, nullptr, &scanList)) {
                flog::error("Scanner: Failed to get fresh scan list from frequency manager");
                return false;
            }
            
            if (!scanList || scanList->empty()) {
                flog::warn("Scanner: Frequency manager returned empty scan list");
                return false;
            }
            
            flog::info("Scanner: Successfully refreshed scan list ({} entries)", (int)scanList->size());
            
            // Note: Profile pointers will be refreshed when worker thread processes next frequency
            return true;
            
        } catch (const std::exception& e) {
            flog::error("Scanner: Error refreshing scan list: {}", e.what());
            return false;
        }
    }
    
    // PERFORMANCE-CRITICAL: Fast tuning profile application (< 10ms target)
    bool applyTuningProfileFast(const TuningProfile& profile, const std::string& vfoName) {
        try {
            if (!core::modComManager.interfaceExists(vfoName) || 
                core::modComManager.getModuleName(vfoName) != "radio") {
                return false;
            }
            
            // CRITICAL VALIDATION: Check for corrupted profile data
            if (profile.demodMode < 0 || profile.demodMode > 7) {
                flog::error("Scanner: Invalid demodulator mode {} in profile - triggering scan list refresh", profile.demodMode);
                refreshScanList(); // Attempt to fix corruption
                return false;
            }
            
            if (profile.bandwidth <= 0 || profile.bandwidth > 10000000.0f) {
                flog::error("Scanner: Invalid bandwidth {:.1f} Hz in profile - triggering scan list refresh", profile.bandwidth);
                refreshScanList(); // Attempt to fix corruption  
                return false;
            }
            
            // Core demodulation settings (fast direct calls)
            int mode = profile.demodMode;
            float bandwidth = profile.bandwidth;
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
            
            // SQUELCH CONTROL: Use existing radio interface (available!)
            // CRITICAL: Don't apply profile squelch if mute while scanning is active
            if (!muteScanningActive) {
                if (profile.squelchEnabled) {
                    bool squelchEnabled = profile.squelchEnabled;
                    float squelchLevel = profile.squelchLevel;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &squelchEnabled, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH_LEVEL, &squelchLevel, NULL);
                } else {
                    bool squelchDisabled = false;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &squelchDisabled, NULL);
                }
            } else {
                SCAN_DEBUG("Scanner: Skipping profile squelch application - mute while scanning is active");
            }
        
        // RF GAIN CONTROL: Use universal gain API from source manager
        // Apply gain regardless of value (0 dB and negative values are valid)
        if (profile.rfGain >= 0.0f && profile.rfGain <= 100.0f) {
            sigpath::sourceManager.setGain(profile.rfGain);
        } else {
            flog::warn("Scanner: Invalid RF gain {:.1f} dB in profile, skipping", profile.rfGain);
        }
            
            // TODO: AGC settings require direct demodulator access (not exposed via radio interface yet)
            // Available in TuningProfile: agcEnabled, but no radio interface command for AGC mode
            
            return true; // Success
            
        } catch (const std::exception& e) {
            flog::error("Scanner: Error applying tuning profile: {}", e.what());
            return false; // Failure
        }
    }
    
    // PERFORMANCE-CRITICAL: Frequency manager integration (< 5ms target)  
    bool performFrequencyManagerScanning() {
        // PERFORMANCE FIX: Check interface exists only once on first call
        static bool interfaceChecked = false;
        static bool interfaceAvailable = false;
        
        if (!interfaceChecked) {
            interfaceAvailable = core::modComManager.interfaceExists("frequency_manager");
            if (!interfaceAvailable) {
                flog::info("Scanner: Frequency manager module not available, using legacy mode");
            }
            interfaceChecked = true;
        }
        
        if (!interfaceAvailable) {
            return false;
        }
        
        try {
            // REAL FREQUENCY MANAGER INTEGRATION: Get actual scan list
            // Note: ModuleComManager doesn't have getInstance, we'll use the interface directly

            // Get the actual scan list from frequency manager
            static std::vector<double> realScanList;
            static std::vector<bool> realScanTypes;
            static std::vector<const void*> realScanProfiles; // Store profile pointers
            static std::vector<const void*> realScanBookmarks; // Store bookmark pointers
            static bool scanListLoaded = false;
            static auto lastScanListUpdate = std::chrono::steady_clock::now();
            
            // Refresh scan list every 5 seconds to pick up frequency manager changes
            auto now = std::chrono::steady_clock::now();
            auto timeSinceUpdate = std::chrono::duration_cast<std::chrono::seconds>(now - lastScanListUpdate);
            if (timeSinceUpdate.count() >= 5) {
                scanListLoaded = false; // Force refresh
                lastScanListUpdate = now;
            }
            
            if (!scanListLoaded) {
                try {
                    // Scan list loading info removed - reduces console noise
                    
                    // REAL INTERFACE CALL: Get scan list from frequency manager  
                    // CRITICAL: Use frequency manager's real ScanEntry structure (must match exactly!)
                    struct ScanEntry {
                        double frequency;                           // Target frequency
                        const TuningProfile* profile;              // Real profile pointer (not void*)
                        const FrequencyBookmark* bookmark;         // Real bookmark pointer (not void*)
                        bool isFromBand;                           // true = from band, false = direct frequency
                        
                        // SAFETY: This struct MUST match frequency_manager's ScanEntry exactly
                        // Any mismatch will cause memory corruption
                    };
                    const std::vector<ScanEntry>* scanList = nullptr;
                    const int CMD_GET_SCAN_LIST = 1; // Match frequency manager command
                    
                    if (!core::modComManager.callInterface("frequency_manager", CMD_GET_SCAN_LIST, nullptr, &scanList)) {
                        flog::error("Scanner: Failed to call frequency manager getScanList interface");
                        return false;
                    }
                    
                    if (!scanList || scanList->empty()) {
                        flog::info("Scanner: No scannable entries found in frequency manager, will use legacy mode");
                        return false;
                    }
                    
                    // Convert ScanEntry list to scanner format
                    realScanList.clear();
                    realScanTypes.clear();
                    realScanProfiles.clear();
                    realScanBookmarks.clear();
            
            // CRITICAL: Comprehensive profile extraction with full diagnostics
            for (const auto& entry : *scanList) {
                realScanList.push_back(entry.frequency);
                realScanTypes.push_back(!entry.isFromBand); // Single frequency if NOT from band  
                realScanProfiles.push_back(entry.profile);   // Store profile pointer
                realScanBookmarks.push_back(entry.bookmark); // Store bookmark pointer
                
                // Detailed profile logging removed - too verbose for normal operation
            }
                    
                    scanListLoaded = true;
                    // Scan list loaded info logging removed - reduces console noise
                    
                    // VERIFICATION: Cross-check profile array integrity
                    if (realScanList.size() != realScanProfiles.size()) {
                        flog::error("Scanner: CRITICAL BUG - Array size mismatch! Frequencies:{} Profiles:{}", 
                                   (int)realScanList.size(), (int)realScanProfiles.size());
                    }
                    
                    // DIAGNOSTIC: Check for profile pointer duplication
                    std::set<const void*> uniqueProfiles;
                    int nullCount = 0;
                    for (const auto* profile : realScanProfiles) {
                        if (profile) {
                            uniqueProfiles.insert(profile);
                        } else {
                            nullCount++;
                        }
                    }
                    flog::info("Scanner: Profile Analysis - Total:{} Unique:{} Null:{}", 
                              (int)realScanProfiles.size(), (int)uniqueProfiles.size(), nullCount);
                    if (realScanList.size() > 10) {
                        flog::info("Scanner: ... and {} more entries", (int)(realScanList.size() - 10));
                    }
                    
                } catch (const std::exception& e) {
                    flog::error("Scanner: Failed to load frequency manager scan list: {}", e.what());
                    return false;
                }
            }
            
            const std::vector<double>& testScanList = realScanList;
            const std::vector<bool>& isSingleFrequency = realScanTypes;
            const std::vector<const void*>& testScanProfiles = realScanProfiles;
            const std::vector<const void*>& testScanBookmarks = realScanBookmarks;
            
            // Real frequency manager integration - no more demo mode!
            
            // Initialize current frequency if not already in scan list  
            // Also ensure starting frequency is not blacklisted
            bool currentFreqInList = false;
            for (const auto& freq : testScanList) {
                if (std::abs(current - freq) < 1000.0) { // Within 1 kHz tolerance
                    currentFreqInList = true;
                    break;
                }
            }
            
            // Count how many frequencies are blacklisted for user info
            int blacklistedCount = 0;
            for (const auto& freq : testScanList) {
                if (isFrequencyBlacklisted(freq)) {
                    blacklistedCount++;
                }
            }
            // Blacklist info logging removed - too verbose for normal operation
            
            if (!currentFreqInList || isFrequencyBlacklisted(current)) {
                // Find first non-blacklisted frequency to start with
                bool foundStartFreq = false;
                for (size_t i = 0; i < testScanList.size(); i++) {
                    if (!isFrequencyBlacklisted(testScanList[i])) {
                        current = testScanList[i];
                        currentScanIndex = i;
                        // Store the corresponding profile
                        if (i < testScanProfiles.size()) {
                            currentTuningProfile = testScanProfiles[i];
                            // DIAGNOSTIC: Log profile assignment during initialization (reduced logging)
                            if (currentTuningProfile) {
                                const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                                
                                // CRITICAL FIX: Apply profile IMMEDIATELY when selecting start frequency
                                // This ensures radio starts in the correct mode
                                if (applyProfiles && !gui::waterfall.selectedVFO.empty()) {
                                    applyTuningProfileSmart(*profile, gui::waterfall.selectedVFO, testScanList[i], "STARTUP");
                                }
                            } else {
                                SCAN_DEBUG("Scanner: INIT NULL PROFILE for start freq {:.6f} MHz (Index:{})", 
                                           testScanList[i] / 1e6, (int)i);
                            }
                        } else {
                            currentTuningProfile = nullptr;
                            flog::warn("Scanner: INIT INDEX OUT OF BOUNDS for profile! Index:{} Size:{}", 
                                      (int)i, (int)testScanProfiles.size());
                        }
                        foundStartFreq = true;
                        break;
                    }
                }
                
                if (!foundStartFreq) {
                    flog::error("Scanner: All frequencies in frequency manager are blacklisted!");
                    return false;
                }
                
                // Starting frequency info logging removed - reduces console noise
            }
            
            if (testScanList.empty()) {
                return false;
            }
            
            // Find current scan index matching current frequency and store its profile
            for (size_t i = 0; i < testScanList.size(); i++) {
                if (std::abs(current - testScanList[i]) < 1000.0) { // Within 1 kHz tolerance
                    currentScanIndex = i;
                    // Store the corresponding profile
                    if (i < testScanProfiles.size()) {
                        currentTuningProfile = testScanProfiles[i];
                        // DIAGNOSTIC: Log profile assignment during current frequency lookup (reduced logging)
                        if (currentTuningProfile) {
                            const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                            
                            // CRITICAL FIX: Apply profile IMMEDIATELY when finding current frequency
                            // This ensures radio is in correct mode from the start
                            if (applyProfiles && !gui::waterfall.selectedVFO.empty()) {
                                applyTuningProfileSmart(*profile, gui::waterfall.selectedVFO, current, "INITIAL");
                            }
                        } else {
                            SCAN_DEBUG("Scanner: LOOKUP NULL PROFILE for current freq {:.6f} MHz (Index:{})", 
                                       current / 1e6, (int)i);
                        }
                    } else {
                        currentTuningProfile = nullptr;
                        flog::warn("Scanner: LOOKUP INDEX OUT OF BOUNDS for profile! Index:{} Size:{}", 
                                  (int)i, (int)testScanProfiles.size());
                    }
                    break;
                }
            }
            
            // Ensure current index is valid
            if (currentScanIndex >= testScanList.size()) {
                currentScanIndex = 0;
                current = testScanList[0];  // Ensure current matches index
            }
            
            // FREQUENCY STEPPING WITH BLACKLIST SUPPORT: Step to next non-blacklisted frequency
            size_t originalIndex = currentScanIndex;
            int attempts = 0;
            const int maxAttempts = (int)testScanList.size(); // Avoid infinite loop
            
            do {
                // Step to next frequency in scan list
                if (scanUp) {
                    currentScanIndex = (currentScanIndex + 1) % testScanList.size();
                } else {
                    currentScanIndex = (currentScanIndex == 0) ? testScanList.size() - 1 : currentScanIndex - 1;
                }
                
                // Set current frequency to the NEW scan list entry
                current = testScanList[currentScanIndex];
                
                // Store the corresponding tuning profile and bookmark for later use
                if (currentScanIndex < testScanProfiles.size()) {
                    currentTuningProfile = testScanProfiles[currentScanIndex];
                    currentBookmark = (currentScanIndex < testScanBookmarks.size()) ? testScanBookmarks[currentScanIndex] : nullptr;
                    // DIAGNOSTIC: Log profile tracking during frequency stepping (reduced logging)
                    if (currentTuningProfile) {
                        const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                        
                        // CRITICAL FIX: Apply profile IMMEDIATELY when stepping to frequency
                        // This ensures radio is in correct mode BEFORE signal detection
                        if (applyProfiles && !gui::waterfall.selectedVFO.empty()) {
                            // ENHANCED MUTE: Ensure silence during demodulator changes
                            ensureMuteDuringOperation();
                            applyTuningProfileSmart(*profile, gui::waterfall.selectedVFO, current, "PREEMPTIVE");
                            
                            // CRITICAL: Ensure profile squelch is applied after emergency mute
                            // This prevents emergency mute from persisting during signal detection
                            if (muteScanningActive && profile->squelchEnabled) {
                                float profileSquelch = profile->squelchLevel;
                                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_LEVEL, &profileSquelch, NULL);
                                SCAN_DEBUG("Scanner: Override emergency mute with profile squelch ({:.1f} dB)", profileSquelch);
                            }
                        }
                    } else {
                        SCAN_DEBUG("Scanner: TRACKING NULL PROFILE for {:.6f} MHz (Index:{})", 
                                   current / 1e6, (int)currentScanIndex);
                    }
                } else {
                    currentTuningProfile = nullptr;
                    currentBookmark = nullptr;
                    flog::warn("Scanner: INDEX OUT OF BOUNDS for profile tracking! Index:{} Size:{}", 
                              (int)currentScanIndex, (int)testScanProfiles.size());
                }
                
                attempts++;
                
                // Check if this frequency is blacklisted
                if (!isFrequencyBlacklisted(current)) {
                    // Found a non-blacklisted frequency
                    break;
                } else {
                    SCAN_DEBUG("Scanner: Skipping blacklisted frequency {:.3f} MHz", current / 1e6);
                    // Continue to next frequency
                }
                
            } while (attempts < maxAttempts && currentScanIndex != originalIndex);
            
            // Check if we found any non-blacklisted frequency
            if (attempts >= maxAttempts || isFrequencyBlacklisted(current)) {
                flog::info("Scanner: All frequencies in frequency manager scan list are blacklisted, will use legacy mode");
                return false; // No valid frequencies to scan, fallback to legacy mode
            }
            
            // CRITICAL: Store entry type for adaptive signal detection
            bool isCurrentSingleFreq = (currentScanIndex < isSingleFrequency.size()) ? isSingleFrequency[currentScanIndex] : false;
            currentEntryIsSingleFreq = isCurrentSingleFreq; // Update global state
            
            // CRITICAL: Immediate VFO tuning (same as performLegacyScanning) 
            // This frequency is guaranteed to NOT be blacklisted
            // Record tuning time for debounce
            tuneTime = std::chrono::high_resolution_clock::now();
            
            // Apply squelch delta preemptively when tuning to new frequency
            // This prevents the initial noise burst when jumping between bands
            // Apply only when not during startup
            if (squelchDelta > 0.0f && !squelchDeltaActive && running) {
                applySquelchDelta();
            }
            
            // ENHANCED MUTE: Ensure silence during frequency changes  
            ensureMuteDuringOperation();
            tuner::normalTuning(gui::waterfall.selectedVFO, current);
            tuning = true;
            lastTuneTime = std::chrono::high_resolution_clock::now();
            
            SCAN_DEBUG("Scanner: Stepped to non-blacklisted frequency {:.6f} MHz ({})", 
                       current / 1e6, currentEntryIsSingleFreq ? "single freq" : "band");
            
            return true; // Successfully performed FM frequency stepping
            
        } catch (const std::exception& e) {
            flog::error("Scanner: Error in frequency manager scanning: {}", e.what());
            return false;
        }
    }
    
    // PERFORMANCE-OPTIMIZED: Legacy scanning fallback
    void performLegacyScanning() {
        // Legacy mode always uses band-style detection (wide tolerance)
        currentEntryIsSingleFreq = false;
        // Get current range bounds (supports multi-range and legacy single range)
        double currentStart, currentStop;
        if (!getCurrentScanBounds(currentStart, currentStop)) {
            // Use single range fallback
            currentStart = startFreq;
            currentStop = stopFreq;
        }
        
        // Ensure current frequency is within bounds
        if (current < currentStart || current > currentStop) {
            current = currentStart;
        }
        
        // Simple linear stepping
        current += (scanUp ? interval : -interval);
        if (current > currentStop) current = currentStart;
        if (current < currentStart) current = currentStop;
        
        // Standard tuning
        // Apply squelch delta preemptively when tuning to new frequency
        // This prevents the initial noise burst when jumping between bands
        // Apply only when not in UI interaction and not during startup
        if (squelchDelta > 0.0f && !squelchDeltaActive && running) {
            applySquelchDelta();
        }
        
        // ENHANCED MUTE: Ensure silence during frequency changes
        ensureMuteDuringOperation();
        tuner::normalTuning(gui::waterfall.selectedVFO, current);
        tuning = true;
        lastTuneTime = std::chrono::high_resolution_clock::now();
    }

    float getMaxLevel(float* data, double freq, double width, int dataWidth, double wfStart, double wfWidth) {
        double low = freq - (width/2.0);
        double high = freq + (width/2.0);
        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        
        float max = -INFINITY;
        for (int i = lowId; i <= highId; i++) {
            if (data[i] > max) { max = data[i]; }
        }
        
        return max;
    }
    
    // HIGH-RESOLUTION version using raw FFT data for precise signal centering
    float getMaxLevelHighRes(double freq, double width, double wfStart, double wfWidth) {
        // Get raw FFT data with full resolution
        int rawFFTSize;
        float* rawData = gui::waterfall.acquireRawFFT(rawFFTSize);
        if (!rawData || rawFFTSize <= 0) {
            if (rawData) gui::waterfall.releaseRawFFT();
            return -INFINITY;
        }
        
        // Calculate frequency range in raw FFT bins
        double low = freq - (width/2.0);
        double high = freq + (width/2.0);
        int lowId = std::clamp<int>((low - wfStart) * (double)rawFFTSize / wfWidth, 0, rawFFTSize - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)rawFFTSize / wfWidth, 0, rawFFTSize - 1);
        
        float max = -INFINITY;
        for (int i = lowId; i <= highId; i++) {
            if (rawData[i] > max) { max = rawData[i]; }
        }
        
        gui::waterfall.releaseRawFFT();
        return max;
    }

    // CONTINUOUS SIGNAL CENTERING: Periodically re-center VFO while receiving to maintain optimal tuning
    void performContinuousCentering(float* data, int dataWidth, double wfStart, double wfWidth) {
        auto now = std::chrono::high_resolution_clock::now();
        auto timeSinceLastCentering = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCenteringTime).count();
        
        // Only perform centering every CENTERING_INTERVAL_MS milliseconds
        if (timeSinceLastCentering < CENTERING_INTERVAL_MS) {
            return;
        }
        
        // Only perform continuous centering when receiving (locked on signal), not when scanning
        if (!receiving || tuning) {
            return;
        }
        
        // Get current VFO bandwidth and apply passband ratio
        double vfoWidth = 0;
        if (!gui::waterfall.selectedVFO.empty()) {
            try {
                vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            } catch (const std::exception& e) {
                return; // Safety check - exit if VFO not available
            }
        } else {
            return; // No VFO selected
        }
        
        double effectiveVfoWidth = vfoWidth * (passbandRatio * 0.01f);
        
        // Check current signal level at current frequency
        float currentLevel = getMaxLevel(data, current, effectiveVfoWidth, dataWidth, wfStart, wfWidth);
        
        // Only proceed if current signal is still above trigger threshold
        if (currentLevel < level) {
            lastCenteringTime = now;
            return;
        }
        
        // Get current scan bounds for range checking
        double currentStart, currentStop;
        if (!getCurrentScanBounds(currentStart, currentStop)) {
            lastCenteringTime = now;
            return;
        }
        
        // Apply signal centering with smaller search radius for continuous operation
        double centeredFreq = findSignalPeak(current, currentLevel, vfoWidth, data, dataWidth, 
                                           wfStart, wfWidth, currentStart, currentStop, level);
        
        // Only apply centering if the movement is reasonable (prevent jumping to different signals)
        double maxCenteringAdjustment = 10000.0; // Max 10 kHz adjustment for continuous centering
        if (std::abs(centeredFreq - current) <= maxCenteringAdjustment) {

            current = centeredFreq;
            gui::waterfall.setCenterFrequency(current);
        }
        
        lastCenteringTime = now;
    }

    // HIGH-RESOLUTION SIGNAL CENTERING: Find the peak using raw FFT data for precise tuning
    double findSignalPeakHighRes(double initialFreq, float initialLevel, double vfoWidth, 
                                double wfStart, double wfWidth, double rangeStart, double rangeStop, float triggerLevel) {
        double peakFreq = initialFreq;
        float peakLevel = initialLevel;
        
        // SMART SEARCH RADIUS: Use signal bandwidth from Frequency Manager profile
        double searchRadius;
        double signalBandwidth = 0.0;
        
        if (currentTuningProfile) {
            const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
            if (profile) {
                signalBandwidth = profile->bandwidth;
                // Search radius = 1.5x signal bandwidth (allows for frequency offset/drift)
                searchRadius = signalBandwidth * 1.5;
                // Reasonable bounds: min 5kHz (narrow signals), max 50kHz (very wide signals)
                searchRadius = std::clamp(searchRadius, 5000.0, 50000.0);
            } else {
                // Fallback: interval-based for non-Frequency Manager mode
                searchRadius = std::max(interval * 2.0, 10000.0);
                searchRadius = std::min(searchRadius, 50000.0);
            }
        } else {
            // Fallback: interval-based for non-Frequency Manager mode
            searchRadius = std::max(interval * 2.0, 10000.0);
            searchRadius = std::min(searchRadius, 50000.0);
        }
        
        // HIGH-RESOLUTION SEARCH STEP: Use raw FFT resolution for maximum precision
        // Get raw FFT size for resolution calculation
        int rawFFTSize;
        float* rawData = gui::waterfall.acquireRawFFT(rawFFTSize);
        if (!rawData || rawFFTSize <= 0) {
            if (rawData) gui::waterfall.releaseRawFFT();
            return initialFreq; // Fallback to original frequency
        }
        gui::waterfall.releaseRawFFT();
        
        double rawFFTResolution = wfWidth / (double)rawFFTSize;
        double searchStep;
        
        if (signalBandwidth > 0) {
            // Step size = signal bandwidth / 20 (good resolution for peak finding)
            searchStep = signalBandwidth / 20.0;
            // But don't go below 10x the raw FFT resolution for meaningful steps
            searchStep = std::max(searchStep, rawFFTResolution * 10.0);
            // Reasonable bounds: min 100Hz (very precise), max 2kHz (fast)
            searchStep = std::clamp(searchStep, 100.0, 2000.0);
        } else {
            // Fallback: use 500Hz steps (good balance of precision and speed)
            searchStep = 500.0;
            searchStep = std::max(searchStep, rawFFTResolution * 10.0);
        }
        
        // Use narrow analysis window for precise measurements
        double testWidth = std::min(searchStep * 0.8, signalBandwidth * 0.5);
        if (testWidth < rawFFTResolution * 5.0) testWidth = rawFFTResolution * 5.0; // At least 5 bins
        
        printf("findSignalPeakHighRes: initialFreq=%.6f MHz, signalBandwidth=%.1f Hz, searchRadius=%.1f Hz, searchStep=%.1f Hz, testWidth=%.1f Hz\n", 
               initialFreq / 1e6, signalBandwidth, searchRadius, searchStep, testWidth);
        printf("  Raw FFT resolution: %.1f Hz/bin (rawFFTSize=%d, wfWidth=%.0f Hz)\n", 
               rawFFTResolution, rawFFTSize, wfWidth);
        
        int peaksFound = 0;
        double bestFreq = initialFreq;
        float bestLevel = initialLevel;
        
        // Search around the initial frequency for the strongest signal
        std::vector<std::pair<double, float>> plateauFreqs;
        
        for (double testFreq = initialFreq - searchRadius; testFreq <= initialFreq + searchRadius; testFreq += searchStep) {
            // Stay within range bounds
            if (testFreq < rangeStart || testFreq > rangeStop) continue;
            
            // Skip if frequency is blacklisted
            if (isFrequencyBlacklisted(testFreq)) continue;
            
            // Check if test frequency is within waterfall bounds
            if (testFreq - (testWidth/2.0) < wfStart || testFreq + (testWidth/2.0) > wfWidth + wfStart) continue;
            
            // Get signal level using high-resolution raw FFT data
            float testLevel = getMaxLevelHighRes(testFreq, testWidth, wfStart, wfWidth);
            
            printf("  Testing %.6f MHz: level=%.1f dBFS (best so far: %.6f MHz at %.1f dBFS)\n", 
                   testFreq / 1e6, testLevel, bestFreq / 1e6, bestLevel);
            
            // Find the strongest frequency within search radius (or equal strength at different frequency)
            // NOTE: In dBFS, LESS NEGATIVE = STRONGER signal, so we want HIGHER values (closer to 0)
            if (testLevel > bestLevel - 0.1) { // Allow equal or slightly better signals
                if (testLevel > bestLevel + 0.1) {
                    printf("  NEW PEAK FOUND: %.6f MHz at %.1f dBFS (improvement: %.1f dB)\n", 
                           testFreq / 1e6, testLevel, testLevel - bestLevel);
                    peaksFound++;
                } else if (std::abs(testLevel - bestLevel) <= 0.1) {
                    printf("  EQUAL PEAK FOUND: %.6f MHz at %.1f dBFS (same level as best)\n", 
                           testFreq / 1e6, testLevel);
                }
                bestLevel = testLevel;
                bestFreq = testFreq;
                printf("  UPDATED BEST: %.6f MHz at %.1f dBFS\n", bestFreq / 1e6, bestLevel);
            } else {
                printf("  REJECTED: %.1f dBFS is weaker than %.1f dBFS (threshold: %.1f dBFS)\n", 
                       testLevel, bestLevel, bestLevel - 0.1);
            }
            
            // Check for plateau region (signal within 1dB of initial level)
            if (std::abs(testLevel - initialLevel) <= 1.0 && testLevel >= initialLevel - 3.0) {
                plateauFreqs.push_back({testFreq, testLevel});
            }
        }
        
        // IMPROVED DECISION LOGIC: Handle equal-level signals better
        if (bestLevel >= initialLevel - 0.1) { // Found signal at least as good as initial
            // If we found a better frequency (even at same level), use it
            if (bestFreq != initialFreq) {
                printf("  CENTERING: Moving from %.6f MHz to %.6f MHz (level: %.1f dBFS)\n", 
                       initialFreq / 1e6, bestFreq / 1e6, bestLevel);
                peakFreq = bestFreq;
                peakLevel = bestLevel;
            } else {
                peakFreq = initialFreq; // Stay at original if no better frequency found
            }
        } else if (plateauFreqs.size() >= 3) {
            // No significant peak, but we found a plateau - move to center
            std::sort(plateauFreqs.begin(), plateauFreqs.end());
            size_t centerIndex = plateauFreqs.size() / 2;
            peakFreq = plateauFreqs[centerIndex].first;
            peakLevel = plateauFreqs[centerIndex].second;
            printf("  PLATEAU CENTERING: Moving to center of plateau at %.6f MHz\n", peakFreq / 1e6);
        } else {
            peakFreq = initialFreq; // Stay at original frequency
            printf("  NO CENTERING: Staying at original frequency %.6f MHz\n", initialFreq / 1e6);
        }
        
        return peakFreq;
    }

    // SIGNAL CENTERING: Find the peak of a detected signal for optimal tuning
    double findSignalPeak(double initialFreq, float initialLevel, double vfoWidth, float* data, int dataWidth, 
                         double wfStart, double wfWidth, double rangeStart, double rangeStop, float triggerLevel) {
        double peakFreq = initialFreq;
        float peakLevel = initialLevel;
        
        // SMART SEARCH RADIUS: Use signal bandwidth from Frequency Manager profile
        double searchRadius;
        double signalBandwidth = 0.0;
        
        if (currentTuningProfile) {
            const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
            if (profile) {
                signalBandwidth = profile->bandwidth;
                // Search radius = 1.5x signal bandwidth (allows for frequency offset/drift)
                searchRadius = signalBandwidth * 1.5;
                // Reasonable bounds: min 5kHz (narrow signals), max 500kHz (very wide signals)
                searchRadius = std::clamp(searchRadius, 5000.0, 500000.0);

            } else {
                // Fallback: interval-based
                searchRadius = std::max(interval * 2.0, 10000.0);
                searchRadius = std::min(searchRadius, 50000.0);

            }
        } else {
            // Fallback: interval-based for non-Frequency Manager mode
            searchRadius = std::max(interval * 2.0, 10000.0);
            searchRadius = std::min(searchRadius, 50000.0);

        }
        
        // SMART SEARCH STEP: Align with FFT resolution for accurate measurements
        // Calculate actual FFT bin resolution to avoid quantization errors
        double fftBinResolution = wfWidth / (double)dataWidth;
        
        double searchStep;
        if (signalBandwidth > 0) {
            // Step size = signal bandwidth / 20 (good resolution for peak finding)
            searchStep = signalBandwidth / 20.0;
            // Reasonable bounds: min 500Hz (precise), max 5kHz (fast)
            searchStep = std::clamp(searchStep, 500.0, 5000.0);
        } else {
            // Fallback: interval-based
            searchStep = std::max(interval / 8.0, 1000.0);
            searchStep = std::min(searchStep, 2000.0);
        }
        
        // CRITICAL FIX: Ensure step is at least 2 FFT bins for meaningful resolution
        searchStep = std::max(searchStep, fftBinResolution * 2.0);
        
        // Round search step to nearest FFT bin boundary for consistent results
        int binsPerStep = std::max(1, (int)std::round(searchStep / fftBinResolution));
        searchStep = binsPerStep * fftBinResolution;
        
        // Use narrow analysis window to avoid overlap between test frequencies
        double testWidth = searchStep * 0.8; // Use 80% of search step to minimize overlap between adjacent tests
        
        printf("findSignalPeak: initialFreq=%.6f MHz, signalBandwidth=%.1f Hz, searchRadius=%.1f Hz, searchStep=%.1f Hz, testWidth=%.1f Hz\n", 
               initialFreq / 1e6, signalBandwidth, searchRadius, searchStep, testWidth);
        printf("  FFT resolution: %.1f Hz/bin (dataWidth=%d, wfWidth=%.0f Hz), binsPerStep=%d\n", 
               fftBinResolution, dataWidth, wfWidth, binsPerStep);
        
        int peaksFound = 0;
        double bestFreq = initialFreq;
        float bestLevel = initialLevel;
        
        // Search around the initial frequency for the strongest signal OR plateau center
        int testsPerformed = 0;
        std::vector<std::pair<double, float>> plateauFreqs; // Store frequencies in plateau region
        
        for (double testFreq = initialFreq - searchRadius; testFreq <= initialFreq + searchRadius; testFreq += searchStep) {
            testsPerformed++;
            
            // Stay within range bounds
            if (testFreq < rangeStart || testFreq > rangeStop) {
                continue;
            }
            
            // Skip if frequency is blacklisted
            if (isFrequencyBlacklisted(testFreq)) {
                continue;
            }
            
            // Check if test frequency is within waterfall bounds
            if (testFreq - (testWidth/2.0) < wfStart || testFreq + (testWidth/2.0) > wfWidth + wfStart) {
                continue;
            }
            
            // Get signal level at test frequency using consistent width
            float testLevel = getMaxLevel(data, testFreq, testWidth, dataWidth, wfStart, wfWidth);
            
            printf("  Testing %.6f MHz: level=%.1f dBFS (best so far: %.6f MHz at %.1f dBFS)\n", 
                   testFreq / 1e6, testLevel, bestFreq / 1e6, bestLevel);

            
            // PEAK OPTIMIZATION: Find the strongest frequency within search radius (ignore trigger level during search)
            // Check for traditional peak (stronger signal)
            if (testLevel > bestLevel + 0.1) {
                printf("  NEW PEAK FOUND: %.6f MHz at %.1f dBFS (improvement: %.1f dB)\n", 
                       testFreq / 1e6, testLevel, testLevel - bestLevel);
                bestLevel = testLevel;
                bestFreq = testFreq;
                peaksFound++;
            }
            
            // Check for plateau region (signal within 1dB of initial level)
            if (std::abs(testLevel - initialLevel) <= 1.0 && testLevel >= initialLevel - 3.0) {
                plateauFreqs.push_back({testFreq, testLevel});
            }
        }
        

        
        // IMPROVED DECISION LOGIC: Find center of peak region, not just first peak
        if (bestLevel > initialLevel + 0.3) {
            // Find ALL frequencies at the best level (within 0.1 dB tolerance)
            std::vector<std::pair<double, float>> peakRegion;
            
            // Re-scan the tested frequencies to find all at best level
            for (double testFreq = initialFreq - searchRadius; testFreq <= initialFreq + searchRadius; testFreq += searchStep) {
                // Skip if outside bounds or blacklisted
                if (testFreq < rangeStart || testFreq > rangeStop || isFrequencyBlacklisted(testFreq)) continue;
                if (testFreq - (testWidth/2.0) < wfStart || testFreq + (testWidth/2.0) > wfWidth + wfStart) continue;
                
                float testLevel = getMaxLevel(data, testFreq, testWidth, dataWidth, wfStart, wfWidth);
                
                // Include in peak region if level is within 0.1 dB of best level
                if (std::abs(testLevel - bestLevel) <= 0.1) {
                    peakRegion.push_back({testFreq, testLevel});
                }
            }
            
            if (peakRegion.size() >= 3) {
                // Multiple frequencies at peak level - center on the middle
                std::sort(peakRegion.begin(), peakRegion.end()); // Sort by frequency
                size_t centerIndex = peakRegion.size() / 2;
                peakFreq = peakRegion[centerIndex].first;
                peakLevel = peakRegion[centerIndex].second;

            } else {
                // Single peak frequency - use it
                peakFreq = bestFreq;
                peakLevel = bestLevel;

            }
        } else if (plateauFreqs.size() >= 3) {
            // No significant peak, but we found a plateau with multiple frequencies - move to center
            std::sort(plateauFreqs.begin(), plateauFreqs.end()); // Sort by frequency
            size_t centerIndex = plateauFreqs.size() / 2;
            peakFreq = plateauFreqs[centerIndex].first;
            peakLevel = plateauFreqs[centerIndex].second;

        } else {
            peakFreq = initialFreq; // Stay at original frequency if no improvement or plateau
        }
        
        return peakFreq;
    }
    
    // Get current squelch level from radio module
    float getRadioSquelchLevel() {
        if (gui::waterfall.selectedVFO.empty() || 
            !core::modComManager.interfaceExists(gui::waterfall.selectedVFO) ||
            core::modComManager.getModuleName(gui::waterfall.selectedVFO) != "radio") {
            // Debug logging removed for production
            return -50.0f; // Default squelch level if no radio available
        }
        
        float squelchLevel = -50.0f;
        if (!core::modComManager.callInterface(gui::waterfall.selectedVFO, 
                                           RADIO_IFACE_CMD_GET_SQUELCH_LEVEL, 
                                           NULL, &squelchLevel)) {
            SCAN_DEBUG("Scanner: Failed to get squelch level");
        }
        
        return squelchLevel;
    }
    
    // Set squelch level on radio module
    void setRadioSquelchLevel(float level) {
        if (gui::waterfall.selectedVFO.empty() || 
            !core::modComManager.interfaceExists(gui::waterfall.selectedVFO) ||
            core::modComManager.getModuleName(gui::waterfall.selectedVFO) != "radio") {
            // Debug logging removed for production
            return;
        }
        
        float newLevel = level;
        
        if (!core::modComManager.callInterface(gui::waterfall.selectedVFO, 
                                           RADIO_IFACE_CMD_SET_SQUELCH_LEVEL, 
                                           &newLevel, NULL)) {
            SCAN_DEBUG("Scanner: Failed to set squelch level");
        }
    }
    
    // Apply squelch delta when signal detected
    void applySquelchDelta() {
        // CRITICAL FIX: Don't use scanMtx here - it's causing a deadlock
        // The worker thread already holds scanMtx when this is called
        
        if (!squelchDeltaActive) {
            try {
                // Check if squelch is enabled in radio module
                bool squelchEnabled = false;
                if (!core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_SQUELCH_ENABLED, NULL, &squelchEnabled)) {
                    // Failed to get squelch state, assume disabled
                    flog::warn("Scanner: Failed to get squelch state, skipping delta application");
                    return;
                }
                
                // Don't apply delta if squelch is disabled
                if (!squelchEnabled) {
                    return;
                }
                
                // Store original squelch level
                originalSquelchLevel = getRadioSquelchLevel();
                
                // Calculate new squelch level with delta
                float deltaLevel;
                if (squelchDeltaAuto) {
                    // Auto mode: use noise floor plus delta value (with bounds)
                    float boundedDelta = std::clamp(squelchDelta, 0.0f, 20.0f);
                    deltaLevel = std::max(noiseFloor + boundedDelta, MIN_SQUELCH);
                } else {
                    // Manual mode: subtract delta from original level (with bounds)
                    deltaLevel = std::max(originalSquelchLevel - squelchDelta, MIN_SQUELCH);
                }
                
                // Apply the new squelch level
                setRadioSquelchLevel(deltaLevel);
                squelchDeltaActive = true;
                
                // Initialize last noise update time
                lastNoiseUpdate = std::chrono::high_resolution_clock::now();
            }
            catch (const std::exception& e) {
                flog::error("Scanner: Exception in applySquelchDelta: {}", e.what());
            }
            catch (...) {
                flog::error("Scanner: Unknown exception in applySquelchDelta");
            }
        }
    }
    
    // Restore original squelch level
    void restoreSquelchLevel() {
        // CRITICAL FIX: Don't use scanMtx here - it's causing a deadlock
        // The worker thread already holds scanMtx when this is called
        
        if (squelchDeltaActive) {
            try {
                // Check if squelch is enabled in radio module
                bool squelchEnabled = false;
                if (!core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_SQUELCH_ENABLED, NULL, &squelchEnabled)) {
                    // Failed to get squelch state, assume disabled
                    flog::warn("Scanner: Failed to get squelch state during restore, clearing delta state");
                    squelchDeltaActive = false;
                    return;
                }
                
                // Only restore level if squelch is enabled
                if (squelchEnabled) {
                    setRadioSquelchLevel(originalSquelchLevel);
                }
                
                squelchDeltaActive = false;
            }
            catch (const std::exception& e) {
                flog::error("Scanner: Exception in restoreSquelchLevel: {}", e.what());
                squelchDeltaActive = false;
            }
            catch (...) {
                flog::error("Scanner: Unknown exception in restoreSquelchLevel");
                squelchDeltaActive = false;
            }
        }
    }
    
    // Update noise floor estimate (for auto squelch delta mode)
    void updateNoiseFloor(float instantNoise) {
        // Stronger smoothing factor for more stable noise floor
        const float alpha = 0.95f; // Smoothing factor (0.95 = 95% old value, 5% new value)
        
        // Skip updates during active reception to avoid fighting the signal
        if (receiving) return;
        
        // Apply exponential moving average
        noiseFloor = alpha * noiseFloor + (1.0f - alpha) * instantNoise;
        
        // If in auto mode and enough time has passed since last adjustment
        auto now = std::chrono::high_resolution_clock::now();
        if (squelchDeltaAuto && 
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNoiseUpdate).count() >= 250) {
            
            // Calculate and apply closing threshold with bounds
            float deltaValue = std::clamp(squelchDelta, 0.0f, 20.0f);
            float closingThreshold = std::max(noiseFloor + deltaValue, MIN_SQUELCH);
            
            // Only apply if we're actively scanning and delta is enabled
            if (squelchDeltaActive && !receiving) {
                setRadioSquelchLevel(closingThreshold);
            }
            
            lastNoiseUpdate = now;
        }
    }
    
    // Apply mute while scanning (set squelch to maximum to prevent noise bursts)
    void applyMuteWhileScanning() {
        if (!muteWhileScanning || muteScanningActive) {
            return; // Feature disabled or already active
        }
        
        try {
            // Check if squelch is enabled in radio module
            bool squelchEnabled = false;
            if (!core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_SQUELCH_ENABLED, NULL, &squelchEnabled)) {
                return; // Can't get squelch state
            }
            
            // Enable squelch if not already enabled
            if (!squelchEnabled) {
                squelchEnabled = true;
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &squelchEnabled, NULL);
            }
            
            // Store original squelch level before muting
            originalSquelchLevelForMute = getRadioSquelchLevel();
            
            // Set squelch to maximum (close to 0 dB) to mute everything
            float muteLevel = -5.0f; // Very high squelch level to mute all scanning noise
            setRadioSquelchLevel(muteLevel);
            
            muteScanningActive = true;
            SCAN_DEBUG("Scanner: Applied mute while scanning (original: {:.1f} dB)", originalSquelchLevelForMute);
            
        } catch (const std::exception& e) {
            flog::error("Scanner: Error applying mute while scanning: {}", e.what());
        }
    }
    
    // Restore squelch level after signal detection
    void restoreMuteWhileScanning() {
        if (!muteScanningActive) {
            return; // Not currently muted
        }
        
        try {
            muteScanningActive = false; // Clear mute state first
            
            // If we have a current tuning profile, apply its squelch settings
            if (currentTuningProfile && !gui::waterfall.selectedVFO.empty()) {
                const TuningProfile* profile = static_cast<const TuningProfile*>(currentTuningProfile);
                if (profile) {
                    // Apply profile's squelch settings now that mute is off
                    if (profile->squelchEnabled) {
                        bool squelchEnabled = profile->squelchEnabled;
                        float squelchLevel = profile->squelchLevel;
                        core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &squelchEnabled, NULL);
                        core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_LEVEL, &squelchLevel, NULL);
                        SCAN_DEBUG("Scanner: Restored profile squelch after signal detection ({:.1f} dB)", squelchLevel);
                    } else {
                        bool squelchDisabled = false;
                        core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &squelchDisabled, NULL);
                        SCAN_DEBUG("Scanner: Disabled squelch per profile after signal detection");
                    }
                    return; // Profile squelch applied successfully
                }
            }
            
            // Fallback: restore original squelch level if no profile
            setRadioSquelchLevel(originalSquelchLevelForMute);
            SCAN_DEBUG("Scanner: Restored original squelch after signal detection ({:.1f} dB)", originalSquelchLevelForMute);
            
        } catch (const std::exception& e) {
            flog::error("Scanner: Error restoring squelch after mute: {}", e.what());
            muteScanningActive = false; // Ensure state is cleared even on error
        }
    }
    
    // Enhanced mute for critical operations (ensures silence during frequency/demod changes)
    void ensureMuteDuringOperation() {
        if (!muteWhileScanning) {
            SCAN_DEBUG("Scanner: Skipping aggressive mute - mute while scanning disabled");
            return; // Feature disabled
        }
        if (!aggressiveMute) {
            SCAN_DEBUG("Scanner: Skipping aggressive mute - aggressive mute disabled by user");
            return; // Aggressive mute disabled by user
        }
        if (receiving) {
            SCAN_DEBUG("Scanner: Skipping aggressive mute - locked onto signal");
            return; // We're locked onto a signal (don't interfere)
        }
        
        try {
            if (!gui::waterfall.selectedVFO.empty()) {
                // Apply immediate high squelch to prevent any noise bursts during operation
                bool squelchEnabled = true;
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &squelchEnabled, NULL);
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SQUELCH_LEVEL, &aggressiveMuteLevel, NULL);
                
                // Small delay to ensure squelch command takes effect before proceeding
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                
                SCAN_DEBUG("Scanner: Applied aggressive mute during critical operation ({:.1f} dB)", aggressiveMuteLevel);
            }
        } catch (const std::exception& e) {
            SCAN_DEBUG("Scanner: Error applying aggressive mute: {}", e.what());
        }
    }
    
    // Calculate signal strength and SNR for current VFO (similar to waterfall Ctrl+click info)
    bool calculateCurrentSignalInfo(float& strength, float& snr) {
        flog::info("Scanner: calculateCurrentSignalInfo() called");
        if (gui::waterfall.selectedVFO.empty()) {
            flog::warn("Scanner: No selected VFO");
            return false;
        }
        
        try {
            // Get current VFO and FFT data
            auto vfoIt = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
            if (vfoIt == gui::waterfall.vfos.end()) {
                flog::warn("Scanner: VFO not found in waterfall.vfos");
                return false;
            }
            
            ImGui::WaterfallVFO* vfo = vfoIt->second;
            if (!vfo) {
                flog::warn("Scanner: VFO pointer is null");
                return false;
            }
            
            flog::info("Scanner: VFO found, bandwidth={:.1f}", vfo->bandwidth);
            
            // Get raw FFT data from waterfall
            int fftWidth;
            float* fftData = gui::waterfall.acquireRawFFT(fftWidth);
            if (!fftData || fftWidth <= 0) {
                flog::warn("Scanner: Failed to acquire FFT data (null pointer or invalid width)");
                gui::waterfall.releaseRawFFT();
                return false;
            }
            
            flog::info("Scanner: FFT data acquired, width={}", fftWidth);
            
            // Implement same signal analysis algorithm as waterfall
            // Calculate FFT index data based on waterfall's bandwidth
            double wholeBandwidth = gui::waterfall.getBandwidth();
            double vfoMinSizeFreq = vfo->centerOffset - vfo->bandwidth;
            double vfoMinFreq = vfo->centerOffset - (vfo->bandwidth / 2.0);
            double vfoMaxFreq = vfo->centerOffset + (vfo->bandwidth / 2.0);
            double vfoMaxSizeFreq = vfo->centerOffset + vfo->bandwidth;
            
            int vfoMinSideOffset = std::clamp<int>(((vfoMinSizeFreq / (wholeBandwidth / 2.0)) * (double)(fftWidth / 2)) + (fftWidth / 2), 0, fftWidth);
            int vfoMinOffset = std::clamp<int>(((vfoMinFreq / (wholeBandwidth / 2.0)) * (double)(fftWidth / 2)) + (fftWidth / 2), 0, fftWidth);
            int vfoMaxOffset = std::clamp<int>(((vfoMaxFreq / (wholeBandwidth / 2.0)) * (double)(fftWidth / 2)) + (fftWidth / 2), 0, fftWidth);
            int vfoMaxSideOffset = std::clamp<int>(((vfoMaxSizeFreq / (wholeBandwidth / 2.0)) * (double)(fftWidth / 2)) + (fftWidth / 2), 0, fftWidth);
            
            double avg = 0;
            float max = -INFINITY;
            int avgCount = 0;
            
            flog::info("Scanner: Index calculations - minSide={}, min={}, max={}, maxSide={}, fftWidth={}", 
                      vfoMinSideOffset, vfoMinOffset, vfoMaxOffset, vfoMaxSideOffset, fftWidth);
            flog::info("Scanner: VFO offsets - centerOffset={:.1f}, bandwidth={:.1f}", vfo->centerOffset, vfo->bandwidth);
            flog::info("Scanner: Frequency calculations - wholeBandwidth={:.1f}", wholeBandwidth);
            
            // Calculate Left average (noise floor)
            for (int i = vfoMinSideOffset; i < vfoMinOffset; i++) {
                avg += fftData[i];
                avgCount++;
            }
            
            // Calculate Right average (noise floor)
            for (int i = vfoMaxOffset + 1; i < vfoMaxSideOffset; i++) {
                avg += fftData[i];
                avgCount++;
            }
            
            if (avgCount > 0) {
                avg /= (double)(avgCount);
            } else {
                avg = -100.0; // Fallback noise floor
            }
            
            // Calculate max (signal strength)
            for (int i = vfoMinOffset; i <= vfoMaxOffset; i++) {
                if (fftData[i] > max) { 
                    max = fftData[i]; 
                }
            }
            
            flog::info("Scanner: Signal analysis - avgCount={}, avg={:.1f}, max={:.1f}, SNR={:.1f}", 
                      avgCount, avg, max, max - avg);
            
            strength = max;
            snr = max - avg;
            
            gui::waterfall.releaseRawFFT();
            
            flog::info("Scanner: Signal analysis completed - strength={:.1f}, snr={:.1f}", strength, snr);
            return true;
            
        } catch (const std::exception& e) {
            gui::waterfall.releaseRawFFT();
            SCAN_DEBUG("Scanner: Error calculating signal info: {}", e.what());
            return false;
        }
    }

    // Update signal analysis every 50ms while signal is locked
    void updateSignalAnalysis() {
        if (!showSignalInfo || !receiving || !showSignalTooltip) {
            return;
        }
        
        auto now = std::chrono::high_resolution_clock::now();
        auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalAnalysisTime).count();
        
        // Update every 50ms for real-time monitoring
        if (timeSinceLastUpdate >= 50) {
            float strength, snr;
            if (calculateCurrentSignalInfo(strength, snr)) {
                lastSignalStrength = strength;
                lastSignalSNR = snr;
                // Keep the same frequency - we're monitoring the locked signal
                lastSignalAnalysisTime = now;
            } else {
                // If analysis fails, clear tooltip
                showSignalTooltip = false;
            }
        }
    }

    // Draw signal analysis tooltip near the VFO (like Ctrl+click behavior)
    void drawSignalTooltip() {
        if (!showSignalTooltip || !showSignalInfo || !receiving || gui::waterfall.selectedVFO.empty()) {
            return;
        }
        
        // Update signal analysis every 50ms while signal is present
        updateSignalAnalysis();
        
        try {
            // Get current VFO
            auto vfoIt = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
            if (vfoIt == gui::waterfall.vfos.end()) {
                return;
            }
            
            ImGui::WaterfallVFO* vfo = vfoIt->second;
            if (!vfo) {
                return;
            }
            
            // Position tooltip near the VFO center
            ImVec2 tooltipPos;
            tooltipPos.x = (vfo->rectMin.x + vfo->rectMax.x) / 2.0f + 10.0f; // Offset to the right
            tooltipPos.y = vfo->rectMin.y - 5.0f; // Slightly above VFO
            
            // Set tooltip position
            ImGui::SetNextWindowPos(tooltipPos, ImGuiCond_Always);
            
            // Begin tooltip window
            if (ImGui::Begin("##ScannerSignalTooltip", nullptr, 
                           ImGuiWindowFlags_Tooltip | 
                           ImGuiWindowFlags_NoTitleBar | 
                           ImGuiWindowFlags_NoResize | 
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings)) {
                
                // Display VFO name
                ImGui::TextUnformatted(gui::waterfall.selectedVFO.c_str());
                ImGui::Separator();
                
                // Display signal info with real-time updates
                char freqStr[64];
                snprintf(freqStr, sizeof(freqStr), "%.6f MHz", lastSignalFrequency / 1e6);
                ImGui::Text("Frequency: %s", freqStr);
                
                ImGui::Text("Strength: %.1f dBFS", lastSignalStrength);
                ImGui::Text("SNR: %.1f dB", lastSignalSNR);
                
                ImGui::End();
            }
            
        } catch (const std::exception& e) {
            flog::warn("Scanner: Error drawing signal tooltip: {}", e.what());
        }
    }

    std::string name;
    bool enabled = true;
    
    bool running = false;
    //std::string selectedVFO = "Radio";
    
    // Multiple frequency ranges support
    std::vector<FrequencyRange> frequencyRanges;
    int currentRangeIndex = 0;
    
    // Legacy single-range support (for backward compatibility)
    double startFreq = 88000000.0;
    double stopFreq = 108000000.0;
    
    double interval = 100000.0;
    double current = 88000000.0;
    double passbandRatio = 10.0;
    int tuningTime = 250;
    int lingerTime = 1000;
    float level = -50.0;
    bool receiving = false;  // Should start as false, not receiving initially
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    bool configNeedsSave = false; // Flag for delayed config saving
    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime = std::chrono::high_resolution_clock::now();
    std::thread workerThread;
    std::mutex scanMtx;
    
    // Blacklist functionality
    std::vector<double> blacklistedFreqs;
    double blacklistTolerance = 1000.0; // Tolerance in Hz for blacklisted frequencies
    
    // Cache for frequency manager names to avoid excessive lookups
    std::map<double, std::string> frequencyNameCache;
    bool frequencyNameCacheDirty = true; // Set to true when blacklist changes
    
    // Squelch delta functionality
    float squelchDelta = 2.5f; // Default delta of 2.5 dB between detection and closing levels
    bool squelchDeltaAuto = false; // Whether to calculate delta automatically based on noise floor
    float noiseFloor = -100.0f; // Estimated noise floor for auto delta calculation
    float originalSquelchLevel = -50.0f; // Original squelch level before applying delta
    bool squelchDeltaActive = false; // Whether squelch delta is currently active
    std::chrono::time_point<std::chrono::high_resolution_clock> lastNoiseUpdate; // Time of last noise floor update
    std::chrono::time_point<std::chrono::high_resolution_clock> tuneTime; // Time of last frequency tuning
    
    // Mute while scanning functionality (prevents noise bursts during frequency sweeps)
    bool muteWhileScanning = true; // Default to enabled - prevents noise bursts during scanning
    bool muteScanningActive = false; // Whether mute is currently applied  
    float originalSquelchLevelForMute = -50.0f; // Original squelch level before muting
    
    // Enhanced mute system settings (configurable by user)
    bool aggressiveMute = true;              // Enable enhanced mute during operations
    float aggressiveMuteLevel = -3.0f;       // Emergency mute level (dB)
    
    // Signal analysis display settings
    bool showSignalInfo = false;             // Auto-display signal info when signal found
    float lastSignalStrength = -100.0f;     // Last detected signal strength (dBFS)
    float lastSignalSNR = 0.0f;              // Last detected SNR (dB)
    double lastSignalFrequency = 0.0;       // Frequency where signal info was captured
    bool showSignalTooltip = false;         // Whether to show persistent signal tooltip
    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalAnalysisTime; // Time of last signal analysis update
    
    // Trigger level visualization settings
    bool showTriggerLevel = true;            // Show horizontal line for trigger level on FFT display
    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    
    // Continuous signal centering
    std::chrono::time_point<std::chrono::high_resolution_clock> lastCenteringTime; // Time of last signal centering
    const int CENTERING_INTERVAL_MS = 50; // Interval for continuous centering in milliseconds
    
    // High speed scanning options
    bool unlockHighSpeed = false; // Whether to allow scan rates above 50Hz (up to 200Hz)
    bool tuningTimeAuto = false; // Whether to automatically adjust tuning time based on scan rate
    
    // Constants for squelch limits
    const float MIN_SQUELCH = -100.0f;
    const float MAX_SQUELCH = 0.0f;
    
    // Constants for scan rate and timing scaling
    static constexpr int BASE_SCAN_RATE = 50;        // Reference scan rate (Hz)
    static constexpr int BASE_TUNING_TIME = 250;     // Reference tuning time (ms) at 50Hz
    static constexpr int BASE_LINGER_TIME = 1000;    // Reference linger time (ms) at 50Hz
    static constexpr int MIN_TUNING_TIME = 10;       // Absolute minimum tuning time (ms)
    static constexpr int MIN_LINGER_TIME = 50;       // Absolute minimum linger time (ms)
    static constexpr int MAX_SCAN_RATE = 2000;       // Maximum scan rate (Hz) when unlocked - much higher for FFT-based scanning
    static constexpr int MIN_SCAN_RATE = 5;          // Minimum scan rate (Hz)
    static constexpr int NORMAL_MAX_SCAN_RATE = 50;  // Maximum scan rate (Hz) in normal mode
    
    // UI state for range management
    bool showRangeManager = false;
    char newRangeName[256] = "New Range";
    double newRangeStart = 88000000.0;
    double newRangeStop = 108000000.0;
    float newRangeGain = 20.0f;
    
    // PERFORMANCE: Frequency manager integration (auto-detect based on availability and configuration)
    bool useFrequencyManager = true;     // Auto-detect: use frequency manager if available and has data
    bool applyProfiles = true;           // Always enabled - automatically apply tuning profiles
    size_t currentScanIndex = 0;         // Current position in frequency manager scan list
    bool currentEntryIsSingleFreq = false; // Track if current entry is single frequency vs band
    const void* currentTuningProfile = nullptr; // Current frequency's tuning profile (from FM)
    const void* currentBookmark = nullptr;       // Current frequency's bookmark (from FM)
    
    // PERFORMANCE: Smart profile caching to prevent redundant applications
    const void* lastAppliedProfile = nullptr; // Last successfully applied profile
    double lastProfileFrequency = 0.0;        // Frequency where last profile was applied
    std::string lastAppliedVFO = "";           // VFO name where profile was applied
    
    // PERFORMANCE: Configurable scan timing (consistent across all modes) 
    int scanRateHz = 10;                 // Default: 10 scans per second (same as legacy 100ms)
    
    // PASSBAND RATIO DISCRETE VALUES: Percentage options for signal detection bandwidth
    // NOTE: Interval and scan rate now use direct input controls for full user flexibility
    
    static constexpr int PASSBAND_VALUES[] = {5, 10, 20, 30, 50, 75, 100};
    static constexpr const char* PASSBAND_LABELS[] = {"5%", "10%", "20%", "30%", "50%", "75%", "100%"};
    static constexpr const char* PASSBAND_FORMATS[] = {"5%%", "10%%", "20%%", "30%%", "50%%", "75%%", "100%%"}; // Safe for ImGui format strings
    static constexpr int PASSBAND_VALUES_COUNT = 7;
    int passbandIndex = 6; // Default to 100% (index 6, recommended starting point)
    
    // Auto-recording functionality
    bool autoRecord = false;
    FolderSelect autoRecordFolderSelect;
    float autoRecordMinDuration = 5.0f;  // Minimum recording duration in seconds
    char autoRecordNameTemplate[256] = "$y-$M-$d_$h-$m-$s_$f_$r_$n";
    
    // Recording control state
    enum RecordingControlState {
        RECORDING_DISABLED = 0,
        RECORDING_IDLE = 1,
        RECORDING_ACTIVE = 2,
        RECORDING_SUSPENDED = 3
    };
    RecordingControlState recordingControlState = RECORDING_IDLE;
    std::chrono::high_resolution_clock::time_point recordingStartTime;
    double recordingFrequency = 0.0;
    std::string recordingMode = "Unknown";
    std::string recordingFilename = "";  // Captured filename when recording started
    float recordingMinDurationCapture = 5.0f;  // Captured min duration when recording started
    int recordingSequenceNum = 1;
    int recordingFilesCount = 0;
    std::string lastResetDate = "";
    
    // Auto-recording helper methods
    void checkMidnightReset() {
        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);
        
        // Get current date as YYYY-MM-DD string
        char dateBuffer[11];
        snprintf(dateBuffer, sizeof(dateBuffer), "%04d-%02d-%02d", 
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        std::string currentDate(dateBuffer);
        
        // Check if we've crossed midnight (date changed)
        if (lastResetDate != currentDate) {
            if (!lastResetDate.empty()) {
                flog::info("Scanner: Midnight reset - Files Today counter reset from {} to 0", recordingFilesCount);
            }
            recordingFilesCount = 0;
            lastResetDate = currentDate;
            saveConfig(); // Persist the reset
        }
    }
    
    void resetFilesTodayCounter() {
        int oldCount = recordingFilesCount;
        recordingFilesCount = 0;
        saveConfig();
        flog::info("Scanner: Manual reset - Files Today counter reset from {} to 0", oldCount);
    }
    
    std::string generateRecordingFilename(double frequency, const std::string& mode) {
        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);
        
        // No nested directories - put date in filename instead
        
        // Generate filename with template replacement
        std::string filename = autoRecordNameTemplate;
        
        // Replace template variables
        std::regex regexPatterns[] = {
            std::regex("\\$y"), std::regex("\\$M"), std::regex("\\$d"),
            std::regex("\\$h"), std::regex("\\$m"), std::regex("\\$s"),
            std::regex("\\$f"), std::regex("\\$r"), std::regex("\\$n")
        };
        
        char replacements[9][32];
        snprintf(replacements[0], sizeof(replacements[0]), "%04d", tm.tm_year + 1900);
        snprintf(replacements[1], sizeof(replacements[1]), "%02d", tm.tm_mon + 1);
        snprintf(replacements[2], sizeof(replacements[2]), "%02d", tm.tm_mday);
        snprintf(replacements[3], sizeof(replacements[3]), "%02d", tm.tm_hour);
        snprintf(replacements[4], sizeof(replacements[4]), "%02d", tm.tm_min);
        snprintf(replacements[5], sizeof(replacements[5]), "%02d", tm.tm_sec);
        snprintf(replacements[6], sizeof(replacements[6]), "%.0f", frequency);
        snprintf(replacements[7], sizeof(replacements[7]), "%s", mode.c_str());
        snprintf(replacements[8], sizeof(replacements[8]), "%03d", recordingSequenceNum);
        
        for (int i = 0; i < 9; i++) {
            filename = std::regex_replace(filename, regexPatterns[i], replacements[i]);
        }
        
        // Construct full path (single directory, date in filename)
        std::string basePath = autoRecordFolderSelect.expandString(autoRecordFolderSelect.path);
        std::string fullPath = basePath + "/" + filename + ".wav";
        return fullPath;
    }
    
    void startAutoRecording(double frequency, const std::string& mode) {
        if (recordingControlState != RECORDING_IDLE || !autoRecordFolderSelect.pathIsValid()) {
            flog::warn("Scanner: Cannot start recording - state: {}, path valid: {}", 
                      (int)recordingControlState, autoRecordFolderSelect.pathIsValid());
            return;
        }
        
        // Check if Recorder module interface exists
        if (!core::modComManager.interfaceExists("Recorder")) {
            flog::error("Scanner: Recorder module interface not found - is Recorder module loaded?");
            return;
        }
        flog::info("Scanner: Recorder module interface found");
        
        // Generate unique filename
        std::string filepath = generateRecordingFilename(frequency, mode);
        flog::info("Scanner: Generated recording filename: {}", filepath);
        
        // Create base recording directory if it doesn't exist
        std::filesystem::path dir = std::filesystem::path(filepath).parent_path();
        try {
            if (!std::filesystem::exists(dir)) {
                std::filesystem::create_directories(dir);
                flog::info("Scanner: Created recording directory: {}", dir.string());
            }
        } catch (const std::exception& e) {
            flog::error("Scanner: Failed to create recording directory: {}", e.what());
            return;
        }
        
        // Set recorder to audio mode first
        int audioMode = RECORDER_MODE_AUDIO;
        if (!core::modComManager.callInterface("Recorder", RECORDER_IFACE_CMD_SET_MODE, &audioMode, nullptr)) {
            flog::error("Scanner: Failed to set recorder to audio mode");
            return;
        }
        flog::info("Scanner: Set recorder to audio mode");
        
        // Set external control
        if (!core::modComManager.callInterface("Recorder", RECORDER_IFACE_CMD_SET_EXTERNAL_CONTROL, (void*)"Scanner", nullptr)) {
            flog::error("Scanner: Failed to set external control on Recorder module");
            return;
        }
        flog::info("Scanner: Set external control to Scanner");
        
        // Start recording with custom filename
        const char* filenamePtr = filepath.c_str();
        if (!core::modComManager.callInterface("Recorder", RECORDER_IFACE_CMD_START_WITH_FILENAME, (void*)filenamePtr, nullptr)) {
            flog::error("Scanner: Failed to start recording with filename: {}", filepath);
            return;
        }
        
        recordingControlState = RECORDING_ACTIVE;
        recordingStartTime = std::chrono::high_resolution_clock::now();
        recordingFrequency = frequency;
        recordingMode = mode;
        recordingFilename = filepath;  // Capture the actual filename being recorded
        recordingMinDurationCapture = autoRecordMinDuration;  // Capture current setting
        
        flog::info("Scanner: Started auto-recording: {} (min duration captured: {}s)", filepath, recordingMinDurationCapture);
    }
    
    void stopAutoRecording() {
        if (recordingControlState != RECORDING_ACTIVE) {
            return;
        }
        
        // Calculate recording duration
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - recordingStartTime);
        
        // Stop recorder interface
        if (!core::modComManager.callInterface("Recorder", RECORDER_IFACE_CMD_STOP, nullptr, nullptr)) {
            flog::error("Scanner: Failed to stop recording");
        } else {
            flog::info("Scanner: Successfully stopped recording");
        }
        
        // Check minimum duration and delete file if too short (use captured value from when recording started)
        flog::info("Scanner: Recording duration check: {}s vs captured minimum {}s (current slider: {}s)", 
                   (double)duration.count(), (double)recordingMinDurationCapture, (double)autoRecordMinDuration);
        if (duration.count() < recordingMinDurationCapture) {
            flog::info("Scanner: Recording too short ({}s < {}s), deleting file", (double)duration.count(), (double)recordingMinDurationCapture);
            // Delete the short file using captured filename
            try {
                std::filesystem::remove(recordingFilename);
                flog::info("Scanner: Deleted short recording file: {}", recordingFilename);
            } catch (const std::exception& e) {
                flog::warn("Scanner: Failed to delete short recording file: {}", e.what());
            }
        } else {
            recordingFilesCount++;
            recordingSequenceNum++;
            flog::info("Scanner: Completed auto-recording ({}s), saved as file #{}", (double)duration.count(), recordingFilesCount);
        }
        
        recordingControlState = RECORDING_IDLE;
        saveConfig(); // Save updated counters
    }
    
    std::string getCurrentMode() {
        if (gui::waterfall.selectedVFO.empty()) {
            return "Unknown";
        }
        
        std::string vfoName = gui::waterfall.selectedVFO;
        if (core::modComManager.getModuleName(vfoName) == "radio") {
            int mode = -1;
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
            if (mode >= 0) {
                // Map radio modes to strings (from recorder module)
                const char* radioModeStrings[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
                if (mode < 8) return radioModeStrings[mode];
            }
        }
        return "Unknown";
    }
    
    // FFT redraw handler for trigger level visualization
    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        
        // Only draw if trigger level visualization is enabled
        if (!_this->showTriggerLevel) { return; }
        
        // Calculate the Y position of the trigger level line
        // Convert trigger level (dBFS) to pixel position on FFT display
        float fftMin = gui::waterfall.getFFTMin();
        float fftMax = gui::waterfall.getFFTMax();
        float vertRange = fftMax - fftMin;
        
        // Clamp trigger level to visible range
        float clampedLevel = std::clamp(_this->level, fftMin, fftMax);
        
        // Calculate Y position (note: FFT display is inverted, max is at top)
        float scaleFactor = (args.max.y - args.min.y) / vertRange;
        float yPos = args.max.y - ((clampedLevel - fftMin) * scaleFactor);
        
        // Draw horizontal line across the entire FFT width
        ImU32 lineColor = IM_COL32(255, 165, 0, 200); // Orange color with transparency
        args.window->DrawList->AddLine(
            ImVec2(args.min.x, yPos), 
            ImVec2(args.max.x, yPos), 
            lineColor, 
            2.0f * style::uiScale  // Line thickness scaled with UI
        );
        
        // Optional: Add text label showing the trigger level value
        char levelText[32];
        snprintf(levelText, sizeof(levelText), "%.1f dBFS", _this->level);
        ImVec2 textSize = ImGui::CalcTextSize(levelText);
        
        // Position text at right edge of FFT display
        ImVec2 textPos = ImVec2(args.max.x - textSize.x - 5, yPos - textSize.y - 2);
        
        // Draw text background for better visibility
        ImVec2 bgMin = ImVec2(textPos.x - 2, textPos.y - 1);
        ImVec2 bgMax = ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1);
        args.window->DrawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 128));
        
        // Draw the text
        args.window->DrawList->AddText(textPos, IM_COL32(255, 165, 0, 255), levelText);
    }
    
    // Module interface handler for external communication
    static void scannerInterfaceHandler(int code, void* in, void* out, void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        switch (code) {
            case SCANNER_IFACE_CMD_GET_RUNNING:
                if (out != NULL) {
                    *(bool*)out = _this->running;
                }
                break;
            case SCANNER_IFACE_CMD_START:
                if (_this->enabled) {
                    _this->start();
                }
                break;
            case SCANNER_IFACE_CMD_STOP:
                if (_this->enabled) {
                    _this->stop();
                }
                break;
            case SCANNER_IFACE_CMD_RESET:
                if (_this->enabled) {
                    _this->reset();
                }
                break;
            case SCANNER_IFACE_CMD_GET_STATUS:
                if (out != NULL) {
                    // Return status as integer: 0=idle, 1=scanning, 2=tuning, 3=receiving
                    int status = 0;
                    if (_this->running) {
                        if (_this->receiving) status = 3;
                        else if (_this->tuning) status = 2;
                        else status = 1;
                    }
                    *(int*)out = status;
                }
                break;
            case SCANNER_IFACE_CMD_PREV_FREQ:
                if (_this->enabled && _this->running) {
                    // Same logic as << button
                    _this->reverseLock = true;
                    _this->receiving = false;
                    _this->scanUp = false;
                    _this->configNeedsSave = true;
                    _this->applyMuteWhileScanning();
                }
                break;
            case SCANNER_IFACE_CMD_NEXT_FREQ:
                if (_this->enabled && _this->running) {
                    // Same logic as >> button
                    _this->reverseLock = true;
                    _this->receiving = false;
                    _this->scanUp = true;
                    _this->configNeedsSave = true;
                    _this->applyMuteWhileScanning();
                }
                break;
            case SCANNER_IFACE_CMD_BLACKLIST:
                if (_this->enabled && !gui::waterfall.selectedVFO.empty()) {
                    // Same logic as "Blacklist Current Frequency" button
                    double currentFreq = gui::waterfall.getCenterFrequency();
                    if (gui::waterfall.vfos.find(gui::waterfall.selectedVFO) != gui::waterfall.vfos.end()) {
                        currentFreq += gui::waterfall.vfos[gui::waterfall.selectedVFO]->centerOffset;
                    }
                    
                    // Check if frequency is already blacklisted (avoid duplicates)
                    bool alreadyBlacklisted = false;
                    for (const double& blacklisted : _this->blacklistedFreqs) {
                        if (std::abs(currentFreq - blacklisted) < _this->blacklistTolerance) {
                            alreadyBlacklisted = true;
                            break;
                        }
                    }
                    
                    if (!alreadyBlacklisted) {
                        _this->blacklistedFreqs.push_back(currentFreq);
                        _this->frequencyNameCache.clear();
                        _this->frequencyNameCacheDirty = true;
                        _this->saveConfig();
                        
                        // Auto-resume scanning after blacklisting
                        {
                            std::lock_guard<std::mutex> lck(_this->scanMtx);
                            _this->receiving = false;
                        }
                        _this->applyMuteWhileScanning();
                    }
                }
                break;
        }
    }

};

MOD_EXPORT void _INIT_() {
    json def = json({});
    
    // Legacy single range (for backward compatibility)
    def["startFreq"] = 88000000.0;
    def["stopFreq"] = 108000000.0;
    
    // Common scanner parameters
    def["interval"] = 100000.0;  // 100 kHz - good balance of speed vs coverage
    def["passbandRatio"] = 100.0;  // 100% - recommended starting point for best signal detection
    def["tuningTime"] = 250;
    def["lingerTime"] = 1000.0;
    def["level"] = -50.0;
    def["blacklistTolerance"] = 1000.0;
    def["blacklistedFreqs"] = json::array();
    
    // Squelch delta settings
            def["squelchDelta"] = 2.5f;
        def["squelchDeltaAuto"] = false;
        def["muteWhileScanning"] = true;
        def["aggressiveMute"] = true;
        def["aggressiveMuteLevel"] = -3.0f;
        def["showSignalInfo"] = false;
        def["showSignalTooltip"] = false;
        def["unlockHighSpeed"] = false;
        def["tuningTimeAuto"] = false;
        def["showTriggerLevel"] = true;
    
    // Scanning direction preference 
    def["scanUp"] = true; // Default to increasing frequency
    
    // Multiple frequency ranges support
    def["frequencyRanges"] = json::array();
    def["currentRangeIndex"] = 0;
    
    // Frequency manager integration (now always enabled for simplified operation)
    def["scanRateHz"] = 25;                 // 25 scans per second (recommended starting point)
    
    // Auto-recording defaults
    def["autoRecord"] = false;
    def["autoRecordMinDuration"] = 5.0f;
    def["autoRecordPath"] = "%ROOT%/scanner_recordings";
    def["autoRecordNameTemplate"] = "$y-$M-$d_$h-$m-$s_$f_$r_$n";
    def["recordingFilesCount"] = 0;
    def["recordingSequenceNum"] = 1;
    def["lastResetDate"] = "";

    config.setPath(core::args["root"].s() + "/scanner_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ScannerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ScannerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}