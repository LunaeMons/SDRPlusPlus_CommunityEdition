#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <module_com.h>
#include <thread>
#include <radio_interface.h>
#include <sstream>
#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <fstream>
#include <optional>
#include <atomic>
#include <mutex>
#include <algorithm>

SDRPP_MOD_INFO{
    /* Name:            */ "frequency_manager",
    /* Description:     */ "Frequency manager module for SDR++",
    /* Author:          */ "Ryzerth;Zimm",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1
};

// Forward declare demodModeList for TuningProfile
extern const char* demodModeList[];

// Performance-optimized TuningProfile struct (defined before FrequencyBookmark)
struct TuningProfile {
    // Core demodulation settings (cache-friendly layout)
    int demodMode = 0;           // Index into demodModeList
    float bandwidth = 12500.0f;  // Bandwidth in Hz
    bool squelchEnabled = false; // Squelch on/off
    float squelchLevel = -50.0f; // Squelch level (dB)

    // Extended settings (only implemented/functional ones)
    int deemphasisMode = 0;    // De-emphasis setting (0=off, 1=50us, 2=75us)
    bool agcEnabled = true;    // AGC on/off
    float rfGain = 20.0f;      // RF gain (dB) - if implemented in source module
    double centerOffset = 0.0; // Frequency offset from center

    // Profile metadata
    std::string name;      // User-friendly profile name
    bool autoApply = true; // Auto-apply when tuning to this frequency

    // Default constructor
    TuningProfile() = default;

    // Copy constructor and assignment operator
    TuningProfile(const TuningProfile&) = default;
    TuningProfile& operator=(const TuningProfile&) = default;
    TuningProfile(TuningProfile&&) = default;
    TuningProfile& operator=(TuningProfile&&) = default;

    // Performance-optimized methods
    bool isValid() const {
        return bandwidth > 0.0f &&
               squelchLevel >= -100.0f && squelchLevel <= 0.0f &&
               demodMode >= 0 && demodMode < 8;
    }

    // Fast serialization (minimal string operations)
    json toJson() const {
        json j;
        j["demodMode"] = demodMode;
        j["bandwidth"] = bandwidth;
        j["squelchEnabled"] = squelchEnabled;
        j["squelchLevel"] = squelchLevel;
        j["deemphasisMode"] = deemphasisMode;
        j["agcEnabled"] = agcEnabled;
        j["rfGain"] = rfGain;
        j["centerOffset"] = centerOffset;
        j["autoApply"] = autoApply;
        if (!name.empty()) j["name"] = name;
        return j;
    }

    // Fast deserialization with safe defaults
    static TuningProfile fromJson(const json& j) {
        TuningProfile p;
        p.demodMode = j.value("demodMode", 0);
        p.bandwidth = j.value("bandwidth", 12500.0f);
        p.squelchEnabled = j.value("squelchEnabled", false);
        p.squelchLevel = j.value("squelchLevel", -50.0f);
        p.deemphasisMode = j.value("deemphasisMode", 0);
        p.agcEnabled = j.value("agcEnabled", true);

        // MIGRATION FIX: Ensure rfGain is never 0 (fix old configs with 0 dB gain)
        float gainValue = j.value("rfGain", 20.0f);
        p.rfGain = (gainValue <= 0.0f) ? 20.0f : gainValue;

        p.centerOffset = j.value("centerOffset", 0.0);
        p.autoApply = j.value("autoApply", true);
        p.name = j.value("name", "");
        return p;
    }

    // Generate automatic profile name based on settings
    std::string generateAutoName() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %.1fkHz %s",
                 demodModeList[demodMode],
                 bandwidth / 1000.0f,
                 squelchEnabled ? "SQ" : "");
        return std::string(buf);
    }
};

struct FrequencyBookmark {
    // Original fields (backward compatibility)
    double frequency;
    double bandwidth;
    int mode;
    bool selected;

    // NEW: Band support (performance-optimized layout)
    bool isBand = false;           // Flag: true = band, false = single frequency
    double startFreq = 0.0;        // For bands: start frequency
    double endFreq = 0.0;          // For bands: end frequency
    double stepFreq = 100000.0;    // For bands: scanning step (default 100kHz)
    std::string notes;             // User notes/description
    std::vector<std::string> tags; // Tags for categorization (pre-allocated for performance)

    // NEW: Tuning Profile support (memory-efficient optional)
    std::optional<TuningProfile> profile; // Optional tuning profile for this bookmark

    // NEW: Scanner integration (performance-critical)
    bool scannable = false; // Include in scanner scan list

    // Default constructor
    FrequencyBookmark() = default;

    // Copy constructor and assignment operator
    FrequencyBookmark(const FrequencyBookmark&) = default;
    FrequencyBookmark& operator=(const FrequencyBookmark&) = default;
    FrequencyBookmark(FrequencyBookmark&&) = default;
    FrequencyBookmark& operator=(FrequencyBookmark&&) = default;

    // Performance-optimized methods
    bool isValid() const {
        if (isBand) {
            double bandBandwidth = endFreq - startFreq;
            double sdrBandwidth = sigpath::iqFrontEnd.getEffectiveSamplerate();

            return startFreq < endFreq &&
                   stepFreq > 0 &&
                   stepFreq <= bandBandwidth &&
                   stepFreq <= sdrBandwidth;
        }
        return frequency > 0;
    }

    // Check if this bookmark has a tuning profile
    bool hasProfile() const {
        return profile.has_value();
    }

    // Get profile with safe access
    const TuningProfile* getProfile() const {
        return profile.has_value() ? &profile.value() : nullptr;
    }

    // Set profile (creates if needed)
    void setProfile(const TuningProfile& p) {
        profile = p;
    }

    // Remove profile
    void clearProfile() {
        profile.reset();
    }

    // Efficient JSON serialization with minimal string operations
    json toJson() const {
        json j;
        j["frequency"] = frequency;
        j["bandwidth"] = bandwidth;
        j["mode"] = mode;

        // Only include band fields if this is actually a band (space optimization)
        if (isBand) {
            j["isBand"] = true;
            j["startFreq"] = startFreq;
            j["endFreq"] = endFreq;
            j["stepFreq"] = stepFreq;
            if (!notes.empty()) j["notes"] = notes;
            if (!tags.empty()) j["tags"] = tags;
        }

        // Only include profile if present (space optimization)
        if (profile.has_value()) {
            j["profile"] = profile.value().toJson();
        }

        // Scanner integration
        if (scannable) j["scannable"] = true; // Only save if true (space optimization)

        return j;
    }

    // Fast deserialization with proper defaults
    static FrequencyBookmark fromJson(const json& j) {
        FrequencyBookmark bm;
        bm.frequency = j.value("frequency", 0.0);
        bm.bandwidth = j.value("bandwidth", 0.0);
        bm.mode = j.value("mode", 0);
        bm.selected = false;

        // Band support with safe defaults
        bm.isBand = j.value("isBand", false);
        if (bm.isBand) {
            bm.startFreq = j.value("startFreq", 0.0);
            bm.endFreq = j.value("endFreq", 0.0);
            bm.stepFreq = j.value("stepFreq", 100000.0);
            bm.notes = j.value("notes", "");
            if (j.contains("tags") && j["tags"].is_array()) {
                bm.tags = j["tags"].get<std::vector<std::string>>();
            }
        }

        // Profile support with safe defaults
        if (j.contains("profile") && j["profile"].is_object()) {
            bm.profile = TuningProfile::fromJson(j["profile"]);
        }

        // Scanner integration
        bm.scannable = j.value("scannable", false);

        return bm;
    }

    // Get display frequency (for sorting and display)
    double getDisplayFreq() const {
        return isBand ? startFreq : frequency;
    }

    // Get frequency span (for display)
    double getSpan() const {
        return isBand ? (endFreq - startFreq) : 0.0;
    }
};

struct WaterfallBookmark {
    std::string listName;
    std::string bookmarkName;
    FrequencyBookmark bookmark;

    // Default constructor
    WaterfallBookmark() = default;

    // Copy constructor and assignment operator
    WaterfallBookmark(const WaterfallBookmark&) = default;
    WaterfallBookmark& operator=(const WaterfallBookmark&) = default;
    WaterfallBookmark(WaterfallBookmark&&) = default;
    WaterfallBookmark& operator=(WaterfallBookmark&&) = default;
};

ConfigManager config;

const char* demodModeList[] = {
    "NFM",
    "WFM",
    "AM",
    "DSB",
    "USB",
    "CW",
    "LSB",
    "RAW"
};

const char* demodModeListTxt = "NFM\0WFM\0AM\0DSB\0USB\0CW\0LSB\0RAW\0";

enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

const char* bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";

class FrequencyManagerModule : public ModuleManager::Instance {
public:
    FrequencyManagerModule(std::string name) {
        this->name = name;

        config.acquire();
        std::string selList = config.conf["selectedList"];
        bookmarkDisplayMode = config.conf["bookmarkDisplayMode"];
        config.release();

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks();

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);

        // CRITICAL: Register interface for scanner integration
        core::modComManager.registerInterface(name, "frequency_manager", moduleInterfaceHandler, this);
    }

    ~FrequencyManagerModule() {
        core::modComManager.unregisterInterface("frequency_manager");
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
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

    // PERFORMANCE-CRITICAL: Real-time scanner integration data structure
    struct ScanEntry {
        double frequency;                  // Target frequency
        const TuningProfile* profile;      // Cached profile pointer (O(1) access)
        const FrequencyBookmark* bookmark; // Source bookmark reference
        bool isFromBand;                   // true = generated from band, false = direct frequency

        ScanEntry(double freq, const TuningProfile* prof = nullptr,
                  const FrequencyBookmark* bm = nullptr, bool fromBand = false)
            : frequency(freq), profile(prof), bookmark(bm), isFromBand(fromBand) {}

        // Fast comparison for sorting
        bool operator<(const ScanEntry& other) const { return frequency < other.frequency; }
    };

    // PERFORMANCE-CRITICAL: Real-time scanner integration API
    // Get current scan list (thread-safe, lock-free read when possible)
    const std::vector<ScanEntry>& getScanList() const {
        if (scanListDirty.load()) {
            rebuildScanList();
        }
        return cachedScanList;
    }

    // Check if scan list needs rebuilding (lock-free)
    bool isScanListDirty() const {
        return scanListDirty.load();
    }

    // Mark scan list as dirty (lock-free, immediate scanner notification)
    void markScanListDirty() {
        scanListDirty.store(true);
    }

    // Check if a frequency is blacklisted either as a single entry or within a band
    bool isFrequencyBlacklisted(double freq) const {
        config.acquire();
        bool result = false;
        if (config.conf.contains("lists") && config.conf["lists"].contains("Blacklist")) {
            for (auto [name, bm] : config.conf["lists"]["Blacklist"]["bookmarks"].items()) {
                FrequencyBookmark fbm = FrequencyBookmark::fromJson(bm);
                if (fbm.isBand) {
                    if (freq >= fbm.startFreq && freq <= fbm.endFreq) {
                        result = true;
                        break;
                    }
                }
                else {
                    if (std::abs(freq - fbm.frequency) < 1000.0) {
                        result = true;
                        break;
                    }
                }
            }
        }
        config.release();
        return result;
    }

    // Add a frequency to the blacklist list if it is not already present
    void addBlacklistEntry(double freq) {
        config.acquire();
        if (!config.conf["lists"].contains("Blacklist")) {
            config.conf["lists"]["Blacklist"]["showOnWaterfall"] = false;
            config.conf["lists"]["Blacklist"]["bookmarks"] = json::object();
        }
        auto& bms = config.conf["lists"]["Blacklist"]["bookmarks"];
        for (auto [name, bm] : bms.items()) {
            FrequencyBookmark fbm = FrequencyBookmark::fromJson(bm);
            if (fbm.isBand) {
                if (freq >= fbm.startFreq && freq <= fbm.endFreq) {
                    config.release();
                    return;
                }
            }
            else {
                if (std::abs(freq - fbm.frequency) < 1000.0) {
                    config.release();
                    return;
                }
            }
        }
        FrequencyBookmark fbm;
        fbm.frequency = freq;
        fbm.bandwidth = 0;
        fbm.mode = 7;
        fbm.selected = false;
        char nameBuf[64];
        snprintf(nameBuf, sizeof(nameBuf), "%.0f Hz", freq);
        bms[nameBuf] = fbm.toJson();
        config.release(true);
    }

private:
    // PERFORMANCE-OPTIMIZED: Rebuild scan list from current bookmarks (called when dirty)
    void rebuildScanList() const {
        std::lock_guard<std::mutex> lock(scanListMutex);

        // Double-check pattern (avoid unnecessary work if another thread already rebuilt)
        if (!scanListDirty.load()) {
            return;
        }

        cachedScanList.clear();
        cachedScanList.reserve(1000); // Pre-allocate for performance

        // Build scan list from all scannable bookmarks in current list
        for (const auto& [bookmarkName, bookmark] : bookmarks) {
            if (!bookmark.scannable) continue; // Skip non-scannable entries

            if (bookmark.isBand) {
                // Generate scan points for band (performance-optimized)
                double freq = bookmark.startFreq;
                while (freq <= bookmark.endFreq) {
                    cachedScanList.emplace_back(
                        freq,
                        bookmark.getProfile(), // Cache profile pointer
                        &bookmark,             // Cache bookmark reference
                        true                   // Mark as from band
                    );
                    freq += bookmark.stepFreq;

                    // Safety check to prevent infinite loops
                    if (bookmark.stepFreq <= 0) break;
                }
            }
            else {
                // Add single frequency entry
                cachedScanList.emplace_back(
                    bookmark.frequency,
                    bookmark.getProfile(), // Cache profile pointer
                    &bookmark,             // Cache bookmark reference
                    false                  // Not from band
                );
            }
        }

        // Sort for efficient scanning (cache-friendly sequential access)
        std::sort(cachedScanList.begin(), cachedScanList.end());

        // Clear dirty flag (atomic, lock-free)
        scanListDirty.store(false);

        flog::info("FrequencyManager: Rebuilt scan list with {} entries", (int)cachedScanList.size());
    }
    static void applyBookmark(FrequencyBookmark bm, std::string vfoName) {
        // For bands, use the start frequency
        double targetFreq = bm.isBand ? bm.startFreq : bm.frequency;

        if (vfoName == "") {
            // TODO: Replace with proper tune call
            gui::waterfall.setCenterFrequency(targetFreq);
            gui::waterfall.centerFreqMoved = true;
        }
        else {
            // Fast tuning to target frequency
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, targetFreq);

            if (core::modComManager.interfaceExists(vfoName)) {
                if (core::modComManager.getModuleName(vfoName) == "radio") {
                    // Apply tuning profile if present (PERFORMANCE-OPTIMIZED)
                    const TuningProfile* profile = bm.getProfile();
                    if (profile && profile->autoApply) {
                        // Fast profile application - direct interface calls, no lookups
                        applyTuningProfile(*profile, vfoName);
                    }
                    else if (!bm.isBand) {
                        // Fallback: apply basic settings for frequencies without profiles
                        int mode = bm.mode;
                        float bandwidth = bm.bandwidth;
                        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                    }
                }
            }
        }

        // Log the action for user feedback
        if (bm.isBand) {
            flog::info("Frequency Manager: Applied band '{}' - tuned to start frequency {:.3f} MHz",
                       "bookmark", targetFreq / 1e6);
        }

        if (bm.hasProfile()) {
            const TuningProfile* profile = bm.getProfile();
            flog::info("Frequency Manager: Applied profile '{}'",
                       profile->name.empty() ? profile->generateAutoName() : profile->name);
        }
    }

    // PERFORMANCE-OPTIMIZED profile application (< 10ms target)
    static void applyTuningProfile(const TuningProfile& profile, const std::string& vfoName) {
        if (!core::modComManager.interfaceExists(vfoName) ||
            core::modComManager.getModuleName(vfoName) != "radio") {
            return;
        }

        try {
            // Core demodulation settings (fast direct calls)
            int mode = profile.demodMode;
            float bandwidth = profile.bandwidth;
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);

            // Squelch settings
            if (profile.squelchEnabled) {
                float squelchLevel = profile.squelchLevel;
                // TODO: Add squelch interface calls when available
                // core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_SQUELCH, &squelchLevel, NULL);
            }

            // Extended settings (optional, for advanced users)
            // TODO: Add interface calls for advanced settings when available:
            // - AGC enable/disable
            // - RF gain (may need source module interface)
            // - De-emphasis mode
            // - Center offset
        }
        catch (const std::exception& e) {
            flog::error("Frequency Manager: Error applying tuning profile: {}", e.what());
        }
    }

    bool bookmarkEditDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Edit##freq_manager_edit_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedBookmarkName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::BeginTable(("freq_manager_edit_table" + name).c_str(), 2);

            // Name field
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Name");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, 1023)) {
                editedBookmarkName = nameBuf;
            }

            // Type selector (Frequency vs Band)
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Type");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            const char* typeOptions = "Frequency\0Band\0";
            int typeIndex = editedBookmark.isBand ? 1 : 0;
            if (ImGui::Combo(("##freq_manager_edit_type" + name).c_str(), &typeIndex, typeOptions)) {
                editedBookmark.isBand = (typeIndex == 1);
                // Set reasonable defaults when switching to band
                if (editedBookmark.isBand && editedBookmark.startFreq == 0.0) {
                    editedBookmark.startFreq = editedBookmark.frequency;
                    editedBookmark.endFreq = editedBookmark.frequency + 1000000.0; // +1MHz default
                    editedBookmark.stepFreq = 100000.0;                            // 100kHz default step
                }
            }

            if (editedBookmark.isBand) {
                // Band-specific fields
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("Start Freq");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                ImGui::InputDouble(("##freq_manager_edit_start" + name).c_str(), &editedBookmark.startFreq);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("End Freq");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                ImGui::InputDouble(("##freq_manager_edit_end" + name).c_str(), &editedBookmark.endFreq);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("Step");
                if (ImGui::IsItemHovered()) {
                    double bandwidth = editedBookmark.endFreq - editedBookmark.startFreq;
                    ImGui::SetTooltip("Frequency step size for band scanning (Hz)\n"
                                      "Creates major scan points: Start -> Start+Step -> Start+2*Step -> End\n"
                                      "\n"
                                      "CRITICAL REQUIREMENT:\n"
                                      "Step frequency MUST be <= band width (%.1f kHz)\n"
                                      "Otherwise scanner will skip the entire band!\n"
                                      "\n"
                                      "EFFICIENT TWO-TIER SCANNING:\n"
                                      "1. HARDWARE TUNING: Radio tunes to each step (108.0, 109.0 MHz)\n"
                                      "   Captures FFT spectrum data across radio bandwidth\n"
                                      "2. FFT ANALYSIS: Uses Scanner Interval for digital analysis\n"
                                      "   Checks 108.005, 108.010... in captured data (NO retuning!)\n"
                                      "\n"
                                      "WHY YOUR 1000kHz + 5kHz WORKS PERFECTLY:\n"
                                      "- Step = Hardware tuning (slow, but only every 1000kHz)\n"
                                      "- Scanner Interval = Digital FFT analysis (fast, every 5kHz)\n"
                                      "- Result = Fast major hops + thorough spectral coverage\n"
                                      "- Radio bandwidth limits effective interval range per step\n"
                                      "\n"
                                      "RECOMMENDED STEP SIZES:\n"
                                      "- 100-1000 kHz: Optimal for wide band scanning with intervals\n"
                                      "- 25-100 kHz: Balanced for mixed scanning types\n"
                                      "- 5-25 kHz: Maximum precision, hardware-limited speed\n"
                                      "\n"
                                      "TIP: Larger steps work great with small intervals (FFT magic!)",
                                      bandwidth / 1e3);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                ImGui::InputDouble(("##freq_manager_edit_step" + name).c_str(), &editedBookmark.stepFreq);

                // Show bandwidth constraints info with warnings
                double bandBandwidth = editedBookmark.endFreq - editedBookmark.startFreq;
                double sdrBandwidth = sigpath::iqFrontEnd.getEffectiveSamplerate();
                double maxAllowedStep = (std::min)(bandBandwidth, sdrBandwidth);

                // Color-code based on step frequency validity
                ImVec4 textColor;
                if (editedBookmark.stepFreq > maxAllowedStep) {
                    textColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red - invalid
                }
                else if (editedBookmark.stepFreq > maxAllowedStep * 0.8) {
                    textColor = ImVec4(1.0f, 0.6f, 0.2f, 1.0f); // Orange - close to limit
                }
                else {
                    textColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Gray - normal info
                }

                ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                ImGui::Text("Max: %.1f MHz (Band: %.1f, SDR: %.1f)",
                            maxAllowedStep / 1e6, bandBandwidth / 1e6, sdrBandwidth / 1e6);
                ImGui::PopStyleColor();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Step frequency is limited by:\n"
                                      "- Band bandwidth: %.1f MHz\n"
                                      "- SDR hardware bandwidth: %.1f MHz\n"
                                      "\n"
                                      "Maximum allowed step: %.1f MHz\n"
                                      "Current step: %.1f MHz\n"
                                      "\n"
                                      "%s",
                                      bandBandwidth / 1e6, sdrBandwidth / 1e6,
                                      maxAllowedStep / 1e6, editedBookmark.stepFreq / 1e6,
                                      editedBookmark.stepFreq > maxAllowedStep ? "WARNING: Step exceeds limits!" : "Step frequency is within valid range.");
                }


                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("Notes");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputText(("##freq_manager_edit_notes" + name).c_str(), editedNotes, 1023)) {
                    editedBookmark.notes = editedNotes;
                }
            }
            else {
                // Frequency-specific fields (original)
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("Frequency");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                ImGui::InputDouble(("##freq_manager_edit_freq" + name).c_str(), &editedBookmark.frequency);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("Bandwidth");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                ImGui::InputDouble(("##freq_manager_edit_bw" + name).c_str(), &editedBookmark.bandwidth);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("Mode");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                ImGui::Combo(("##freq_manager_edit_mode" + name).c_str(), &editedBookmark.mode, demodModeListTxt);
            }

            ImGui::EndTable();

            ImGui::Spacing();

            // Scanner Integration section
            ImGui::Separator();
            ImGui::Text("Scanner Integration");
            bool scannable = editedBookmark.scannable;
            if (ImGui::Checkbox("Include in Scanner", &scannable)) {
                editedBookmark.scannable = scannable;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, this entry will be included in scanner frequency list");
            }

            ImGui::Spacing();

            // Tuning Profile section
            bool hasProfile = editedBookmark.hasProfile();
            if (ImGui::CollapsingHeader("Tuning Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();

                // Profile enable/disable
                bool enableProfile = hasProfile;
                if (ImGui::Checkbox("Enable Tuning Profile", &enableProfile)) {
                    if (enableProfile && !hasProfile) {
                        // Create new profile with current radio settings
                        TuningProfile newProfile;
                        if (gui::waterfall.selectedVFO != "" &&
                            core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                            int mode;
                            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                            newProfile.demodMode = mode;
                            newProfile.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                        }
                        newProfile.name = newProfile.generateAutoName();
                        editedBookmark.setProfile(newProfile);
                        strcpy(editedProfileName, newProfile.name.c_str());
                    }
                    else if (!enableProfile && hasProfile) {
                        editedBookmark.clearProfile();
                    }
                }

                if (editedBookmark.hasProfile()) {
                    const TuningProfile* profile = editedBookmark.getProfile();
                    editedProfile = *profile; // Copy for editing

                    ImGui::Spacing();

                    // Profile name
                    ImGui::LeftLabel("Profile Name");
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::InputText(("##profile_name" + name).c_str(), editedProfileName, sizeof(editedProfileName))) {
                        editedProfile.name = editedProfileName;
                        editedBookmark.setProfile(editedProfile);
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Auto-Name")) {
                        std::string autoName = editedProfile.generateAutoName();
                        strcpy(editedProfileName, autoName.c_str());
                        editedProfile.name = autoName;
                        editedBookmark.setProfile(editedProfile);
                    }

                    // Basic settings
                    ImGui::LeftLabel("Mode");
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::Combo(("##profile_mode" + name).c_str(), &editedProfile.demodMode, demodModeListTxt)) {
                        editedBookmark.setProfile(editedProfile);
                    }

                    ImGui::LeftLabel("Bandwidth (Hz)");
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::InputFloat(("##profile_bw" + name).c_str(), &editedProfile.bandwidth, 1000.0f, 10000.0f, "%.0f")) {
                        editedProfile.bandwidth = (std::max)(1000.0f, editedProfile.bandwidth);
                        editedBookmark.setProfile(editedProfile);
                    }

                    ImGui::LeftLabel("Squelch Enabled");
                    if (ImGui::Checkbox(("##profile_squelch_en" + name).c_str(), &editedProfile.squelchEnabled)) {
                        editedBookmark.setProfile(editedProfile);
                    }

                    if (editedProfile.squelchEnabled) {
                        ImGui::LeftLabel("Squelch Level (dB)");
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat(("##profile_squelch_lvl" + name).c_str(), &editedProfile.squelchLevel, -100.0f, 0.0f, "%.1f")) {
                            editedBookmark.setProfile(editedProfile);
                        }
                    }

                    // Advanced settings (collapsible)
                    if (ImGui::CollapsingHeader("Advanced Settings")) {
                        ImGui::Indent();

                        ImGui::LeftLabel("RF Gain (dB)");
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat(("##profile_rf_gain" + name).c_str(), &editedProfile.rfGain, 0.0f, 50.0f, "%.1f")) {
                            editedBookmark.setProfile(editedProfile);
                        }

                        ImGui::LeftLabel("AGC Enabled");
                        if (ImGui::Checkbox(("##profile_agc" + name).c_str(), &editedProfile.agcEnabled)) {
                            editedBookmark.setProfile(editedProfile);
                        }

                        ImGui::LeftLabel("Center Offset (Hz)");
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::InputDouble(("##profile_offset" + name).c_str(), &editedProfile.centerOffset, 1000.0, 10000.0, "%.0f")) {
                            editedBookmark.setProfile(editedProfile);
                        }

                        ImGui::Unindent();
                    }
                }

                ImGui::Unindent();
            }

            // Validation and status
            bool isValid = editedBookmark.isValid();
            if (!isValid) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Invalid configuration!");
            }

            // Profile validation
            if (editedBookmark.hasProfile() && !editedBookmark.getProfile()->isValid()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Invalid profile settings!");
                isValid = false;
            }

            bool applyDisabled = (strlen(nameBuf) == 0) || !isValid ||
                                 (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);
            if (applyDisabled) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                open = false;

                // If editing, delete the original one
                if (editOpen) {
                    bookmarks.erase(firstEditedBookmarkName);
                }
                bookmarks[editedBookmarkName] = editedBookmark;

                saveByName(selectedListName);
                markScanListDirty(); // PERFORMANCE: Immediate scanner update
            }
            if (applyDisabled) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool newListDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##freq_manager_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Name");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, 1023)) {
                editedListName = nameBuf;
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                open = false;

                config.acquire();
                if (renameListOpen) {
                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                }
                else {
                    config.conf["lists"][editedListName]["showOnWaterfall"] = true;
                    config.conf["lists"][editedListName]["bookmarks"] = json::object();
                }
                refreshWaterfallBookmarks(false);
                config.release(true);
                refreshLists();
                loadByName(editedListName);
            }
            if (strlen(nameBuf) == 0 || alreadyExists) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool selectListsDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "Select lists##freq_manager_sel_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            // No need to lock config since we're not modifying anything and there's only one instance
            for (auto [listName, list] : config.conf["lists"].items()) {
                bool shown = list["showOnWaterfall"];
                if (ImGui::Checkbox((listName + "##freq_manager_sel_list_").c_str(), &shown)) {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    refreshWaterfallBookmarks(false);
                    config.release(true);
                }
            }

            if (ImGui::Button("Ok")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    void refreshLists() {
        listNames.clear();
        listNamesTxt = "";

        config.acquire();
        for (auto [_name, list] : config.conf["lists"].items()) {
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
        config.release();
    }

    void refreshWaterfallBookmarks(bool lockConfig = true) {
        if (lockConfig) { config.acquire(); }
        waterfallBookmarks.clear();
        for (auto [listName, list] : config.conf["lists"].items()) {
            if (!((bool)list["showOnWaterfall"])) { continue; }
            WaterfallBookmark wbm;
            wbm.listName = listName;
            for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
                wbm.bookmarkName = bookmarkName;
                // Use efficient deserialization and handle bands
                wbm.bookmark = FrequencyBookmark::fromJson(bm);
                wbm.bookmark.selected = false;

                // For bands, add multiple waterfall bookmarks (start, middle, end)
                if (wbm.bookmark.isBand) {
                    // Add start frequency
                    wbm.bookmark.frequency = wbm.bookmark.startFreq;
                    wbm.bookmarkName = bookmarkName + " (Start)";
                    waterfallBookmarks.push_back(wbm);

                    // Add end frequency
                    wbm.bookmark.frequency = wbm.bookmark.endFreq;
                    wbm.bookmarkName = bookmarkName + " (End)";
                    waterfallBookmarks.push_back(wbm);
                }
                else {
                    // Regular frequency bookmark
                    waterfallBookmarks.push_back(wbm);
                }
            }
        }
        if (lockConfig) { config.release(); }
    }

    void loadFirst() {
        if (listNames.size() > 0) {
            loadByName(listNames[0]);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName) {
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
            // Use performance-optimized deserialization
            FrequencyBookmark fbm = FrequencyBookmark::fromJson(bm);
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
        markScanListDirty(); // PERFORMANCE: Immediate scanner update
    }

    void saveByName(std::string listName) {
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks) {
            // Use performance-optimized serialization
            config.conf["lists"][listName]["bookmarks"][bmName] = bm.toJson();
        }
        refreshWaterfallBookmarks(false);
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // TODO: Replace with something that won't iterate every frame
        std::vector<std::string> selectedNames;
        for (auto& [name, bm] : _this->bookmarks) {
            if (bm.selected) { selectedNames.push_back(name); }
        }

        float lineHeight = ImGui::GetTextLineHeightWithSpacing();

        float btnSize = ImGui::CalcTextSize("Rename").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##freq_manager_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.release(true);
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Rename##_freq_mgr_ren_lst_" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;
        }
        if (_this->listNames.size() == 0) { style::endDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##_freq_mgr_add_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            // Find new unique default name
            if (std::find(_this->listNames.begin(), _this->listNames.end(), "New List") == _this->listNames.end()) {
                _this->editedListName = "New List";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New List (%d)", i);
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end()) { break; }
                }
                _this->editedListName = buf;
            }
            _this->newListOpen = true;
        }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##_freq_mgr_del_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (_this->selectedListName == "") { style::endDisabled(); }

        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Deleting list named \"%s\". Are you sure?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            config.acquire();
            config.conf["lists"].erase(_this->selectedListName);
            _this->refreshWaterfallBookmarks(false);
            config.release(true);
            _this->refreshLists();
            _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
            if (_this->listNames.size() > 0) {
                _this->loadByName(_this->listNames[_this->selectedListId]);
            }
            else {
                _this->selectedListName = "";
            }
        }

        if (_this->selectedListName == "") { style::beginDisabled(); }
        // Draw buttons on top of the list
        ImGui::BeginTable(("freq_manager_btn_table" + _this->name).c_str(), 4);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Add##_freq_mgr_add_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // Reset bookmark to frequency mode
            _this->editedBookmark = FrequencyBookmark();
            _this->editedBookmark.isBand = false;
            _this->createBandMode = false;

            // If there's no VFO selected, just save the center freq
            if (gui::waterfall.selectedVFO == "") {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency();
                _this->editedBookmark.bandwidth = 0;
                _this->editedBookmark.mode = 7;
            }
            else {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                _this->editedBookmark.mode = 7;
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                    int mode;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                    _this->editedBookmark.mode = mode;
                }
            }

            _this->editedBookmark.selected = false;
            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("New Bookmark") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "New Bookmark";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New Bookmark (%d)", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }

            // Clear edit buffers
            strcpy(_this->editedNotes, _this->editedBookmark.notes.c_str());
            strcpy(_this->editedProfileName, "");
        }

        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(("Add Band##_freq_mgr_add_band_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // Reset bookmark to band mode
            _this->editedBookmark = FrequencyBookmark();
            _this->editedBookmark.isBand = true;
            _this->createBandMode = true;

            // Set reasonable defaults for band
            double currentFreq = gui::waterfall.getCenterFrequency();
            if (gui::waterfall.selectedVFO != "") {
                currentFreq += sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
            }

            _this->editedBookmark.startFreq = currentFreq - 500000.0; // -500kHz
            _this->editedBookmark.endFreq = currentFreq + 500000.0;   // +500kHz
            _this->editedBookmark.stepFreq = 100000.0;                // 100kHz step
            _this->editedBookmark.selected = false;
            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("New Band") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "New Band";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New Band (%d)", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }

            // Clear edit buffers
            strcpy(_this->editedNotes, _this->editedBookmark.notes.c_str());
            strcpy(_this->editedProfileName, "");
        }

        ImGui::TableSetColumnIndex(2);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Remove##_freq_mgr_rem_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::TableSetColumnIndex(3);
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Edit##_freq_mgr_edt_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->editOpen = true;
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];

            // Load values into edit buffers
            strcpy(_this->editedNotes, _this->editedBookmark.notes.c_str());

            // Load profile name buffer
            if (_this->editedBookmark.hasProfile()) {
                strcpy(_this->editedProfileName, _this->editedBookmark.getProfile()->name.c_str());
            }
            else {
                strcpy(_this->editedProfileName, "");
            }
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        ImGui::EndTable();

        // Bookmark delete confirm dialog
        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::TextUnformatted("Deleting selected bookmaks. Are you sure?");
            }) == GENERIC_DIALOG_BUTTON_YES) {
            for (auto& _name : selectedNames) { _this->bookmarks.erase(_name); }
            _this->saveByName(_this->selectedListName);
            _this->markScanListDirty(); // PERFORMANCE: Immediate scanner update
        }

        // Bookmark list
        if (ImGui::BeginTable(("freq_manager_bkm_table" + _this->name).c_str(), 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200.0f * style::uiScale))) {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("P", ImGuiTableColumnFlags_WidthFixed, 20.0f); // Profile indicator
            ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed, 20.0f); // Scanner toggle
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Details");
            ImGui::TableSetupScrollFreeze(5, 1);
            ImGui::TableHeadersRow();

            // Add helpful tooltip for new UX functionality
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Frequency Manager Controls:");
                ImGui::Separator();
                ImGui::Text("- Single-click: Select entry");
                ImGui::Text("- Double-click: Apply entry (tune to frequency)");
                ImGui::Text("- Right-click: Edit entry");
                ImGui::Text("- Edit button: Edit selected entry");
                ImGui::EndTooltip();
            }
            for (auto& [name, bm] : _this->bookmarks) {
                ImGui::TableNextRow();

                // Type column with color-coded indicators
                ImGui::TableSetColumnIndex(0);
                if (bm.isBand) {
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Band");
                }
                else {
                    ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Freq");
                }

                // Profile indicator column
                ImGui::TableSetColumnIndex(1);
                if (bm.hasProfile()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "[P]");
                    if (ImGui::IsItemHovered()) {
                        const TuningProfile* prof = bm.getProfile();
                        ImGui::BeginTooltip();
                        ImGui::Text("Profile: %s", prof->name.empty() ? prof->generateAutoName().c_str() : prof->name.c_str());
                        ImGui::Text("Mode: %s", demodModeList[prof->demodMode]);
                        ImGui::Text("Bandwidth: %.1f kHz", prof->bandwidth / 1000.0f);
                        if (prof->squelchEnabled) {
                            ImGui::Text("Squelch: %.1f dB", prof->squelchLevel);
                        }
                        ImGui::EndTooltip();
                    }
                }
                else {
                    ImGui::TextDisabled("-");
                }

                // Scanner toggle column (PERFORMANCE-CRITICAL: Quick access UX)
                ImGui::TableSetColumnIndex(2);
                bool isScannable = bm.scannable;
                if (ImGui::Checkbox(("##scan_" + name).c_str(), &isScannable)) {
                    bm.scannable = isScannable;
                    _this->saveByName(_this->selectedListName);
                    _this->markScanListDirty(); // PERFORMANCE: Immediate scanner update
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Include this entry in scanner frequency list\n%s",
                                      isScannable ? "Scanner will tune to this frequency" : "Scanner will skip this entry");
                }

                // Name column
                ImGui::TableSetColumnIndex(3);
                ImVec2 min = ImGui::GetCursorPos();

                if (ImGui::Selectable((name + "##_freq_mgr_bkm_name_" + _this->name).c_str(), &bm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    // if shift or control isn't pressed, deselect all others
                    if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                        for (auto& [_name, _bm] : _this->bookmarks) {
                            if (name == _name) { continue; }
                            _bm.selected = false;
                        }
                    }
                }

                // ENHANCED UX: Double-click applies bookmark (tune to frequency)
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click: Apply bookmark (tune to frequency)
                    applyBookmark(bm, gui::waterfall.selectedVFO);
                }

                // ENHANCED UX: Right-click opens edit dialog
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    // Right-click: Edit bookmark (open edit dialog)
                    _this->editOpen = true;
                    _this->editedBookmark = bm;
                    _this->editedBookmarkName = name;
                    _this->firstEditedBookmarkName = name;

                    // Load values into edit buffers
                    strcpy(_this->editedNotes, _this->editedBookmark.notes.c_str());

                    // Load profile name buffer
                    if (_this->editedBookmark.hasProfile()) {
                        strcpy(_this->editedProfileName, _this->editedBookmark.getProfile()->name.c_str());
                    }
                    else {
                        strcpy(_this->editedProfileName, "");
                    }

                    // Ensure the bookmark is selected for editing
                    for (auto& [_name, _bm] : _this->bookmarks) {
                        _bm.selected = (_name == name);
                    }
                }

                // Details column
                ImGui::TableSetColumnIndex(4);
                if (bm.isBand) {
                    // Band details: start-end MHz, step, span
                    double spanMHz = (bm.endFreq - bm.startFreq) / 1e6;
                    double stepkHz = bm.stepFreq / 1e3;
                    ImGui::Text("%.3f-%.3f MHz (%.0f kHz, %.1f MHz span)",
                                bm.startFreq / 1e6, bm.endFreq / 1e6, stepkHz, spanMHz);
                }
                else {
                    // Frequency details: frequency and mode
                    ImGui::Text("%s %s", utils::formatFreq(bm.frequency).c_str(), demodModeList[bm.mode]);
                }
                ImVec2 max = ImGui::GetCursorPos();
            }
            ImGui::EndTable();
        }


        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Apply##_freq_mgr_apply_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            FrequencyBookmark& bm = _this->bookmarks[selectedNames[0]];
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        // Draw import and export buttons
        ImGui::BeginTable(("freq_manager_bottom_btn_table" + _this->name).c_str(), 2);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Import##_freq_mgr_imp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen) {
            _this->importOpen = true;
            _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, pfd::opt::multiselect);
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Export##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen) {
            _this->exportedBookmarks = json::object();
            config.acquire();
            for (auto& _name : selectedNames) {
                _this->exportedBookmarks["bookmarks"][_name] = config.conf["lists"][_this->selectedListName]["bookmarks"][_name];
            }
            config.release();
            _this->exportOpen = true;
            _this->exportDialog = new pfd::save_file("Export bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" });
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::EndTable();

        if (ImGui::Button(("Select displayed lists##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->selectListsOpen = true;
        }

        ImGui::LeftLabel("Bookmark display mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##_freq_mgr_dms_" + _this->name).c_str(), &_this->bookmarkDisplayMode, bookmarkDisplayModesTxt)) {
            config.acquire();
            config.conf["bookmarkDisplayMode"] = _this->bookmarkDisplayMode;
            config.release(true);
        }

        if (_this->selectedListName == "") { style::endDisabled(); }

        if (_this->createOpen) {
            _this->createOpen = _this->bookmarkEditDialog();
        }

        if (_this->editOpen) {
            _this->editOpen = _this->bookmarkEditDialog();
        }

        if (_this->newListOpen) {
            _this->newListOpen = _this->newListDialog();
        }

        if (_this->renameListOpen) {
            _this->renameListOpen = _this->newListDialog();
        }

        if (_this->selectListsOpen) {
            _this->selectListsOpen = _this->selectListsDialog();
        }

        // Handle import and export
        if (_this->importOpen && _this->importDialog->ready()) {
            _this->importOpen = false;
            std::vector<std::string> paths = _this->importDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0) {
                _this->importBookmarks(paths[0]);
            }
            delete _this->importDialog;
        }
        if (_this->exportOpen && _this->exportDialog->ready()) {
            _this->exportOpen = false;
            std::string path = _this->exportDialog->result();
            if (path != "") {
                _this->exportBookmarks(path);
            }
            delete _this->exportDialog;
        }
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }

                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.min.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0) {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.min.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }

                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.max.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.max.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0) {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.max.y - nameSize.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

        // First check that the mouse clicked outside of any label. Also get the bookmark that's hovered
        bool inALabel = false;
        WaterfallBookmark hoveredBookmark;
        std::string hoveredBookmarkName;

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--) {
                auto& bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMin.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMin.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--) {
                auto& bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMax.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMax.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }

        // Check if mouse was already down
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel) {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        // If yes, cancel
        if (_this->mouseAlreadyDown || !inALabel) { return; }

        gui::waterfall.inputHandled = true;

        double centerXpos = args.fftRectMin.x + std::round((hoveredBookmark.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
        ImVec2 nameSize = ImGui::CalcTextSize(hoveredBookmarkName.c_str());
        ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) ? (args.fftRectMax.y - nameSize.y) : args.fftRectMin.y);
        ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) ? args.fftRectMax.y : args.fftRectMin.y + nameSize.y);
        ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
        ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            _this->mouseClickedInLabel = true;
            applyBookmark(hoveredBookmark.bookmark, gui::waterfall.selectedVFO);
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredBookmarkName.c_str());
        ImGui::Separator();
        ImGui::Text("List: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Frequency: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Bandwidth: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Mode: %s", demodModeList[hoveredBookmark.bookmark.mode]);
        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog;
    pfd::save_file* exportDialog;

    void importBookmarks(std::string path) {
        std::ifstream fs(path);
        json importBookmarks;
        fs >> importBookmarks;

        if (!importBookmarks.contains("bookmarks")) {
            flog::error("File does not contains any bookmarks");
            return;
        }

        if (!importBookmarks["bookmarks"].is_object()) {
            flog::error("Bookmark attribute is invalid");
            return;
        }

        // Load every bookmark using efficient deserialization
        for (auto const [_name, bm] : importBookmarks["bookmarks"].items()) {
            if (bookmarks.find(_name) != bookmarks.end()) {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            // Use performance-optimized deserialization
            FrequencyBookmark fbm = FrequencyBookmark::fromJson(bm);
            fbm.selected = false;

            // Validate bookmark before adding
            if (!fbm.isValid()) {
                flog::warn("Invalid bookmark '{0}' skipped during import", _name);
                continue;
            }

            bookmarks[_name] = fbm;
        }
        saveByName(selectedListName);
        markScanListDirty(); // PERFORMANCE: Immediate scanner update

        fs.close();
    }

    void exportBookmarks(std::string path) {
        std::ofstream fs(path);
        fs << exportedBookmarks;
        fs.close();
    }

    std::string name;
    bool enabled = true;
    bool createOpen = false;
    bool editOpen = false;
    bool newListOpen = false;
    bool renameListOpen = false;
    bool selectListsOpen = false;

    bool deleteListOpen = false;
    bool deleteBookmarksOpen = false;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::map<std::string, FrequencyBookmark> bookmarks;

    std::string editedBookmarkName = "";
    std::string firstEditedBookmarkName = "";
    FrequencyBookmark editedBookmark;

    // Band editing support
    bool createBandMode = false;
    char editedNotes[1024] = "";
    char editedTags[512] = "";

    // Profile editing support
    bool profileEditOpen = false;
    TuningProfile editedProfile;
    char editedProfileName[256] = "";
    bool profileAdvancedMode = false;


    mutable std::vector<ScanEntry> cachedScanList;   // Pre-computed scan list (performance-optimized)
    mutable std::atomic<bool> scanListDirty{ true }; // Lock-free dirty flag for change detection
    mutable std::mutex scanListMutex;                // Minimal locking for scan list updates

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;

    int bookmarkDisplayMode = 0;

    // SCANNER INTEGRATION: Interface handler for ModuleComManager
    enum InterfaceCommands {
        CMD_GET_SCAN_LIST = 1,
        CMD_GET_BOOKMARK_NAME = 2, // Get bookmark name for a specific frequency
        CMD_IS_FREQ_BLACKLISTED = 3,
        CMD_ADD_BLACKLIST = 4
    };

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;

        switch (code) {
        case CMD_GET_SCAN_LIST: {
            // Return the current scan list to the scanner
            if (out) {
                const std::vector<ScanEntry>& scanList = _this->getScanList();
                *static_cast<const std::vector<ScanEntry>**>(out) = &scanList;
                flog::debug("FrequencyManager: Returned scan list with {} entries to scanner", (int)scanList.size());
            }
            else {
                flog::error("FrequencyManager: getScanList called with null output pointer");
            }
            break;
        }
        case CMD_GET_BOOKMARK_NAME: {
            // Get bookmark name for a specific frequency
            // Input: double* frequency, Output: std::string* name
            if (in && out) {
                double targetFreq = *static_cast<double*>(in);
                std::string* resultName = static_cast<std::string*>(out);

                // PRIORITY 1: Search for single frequency matches first (more specific)
                for (const auto& [bookmarkName, bookmark] : _this->bookmarks) {
                    if (!bookmark.isBand) {
                        // For single frequencies, check with tolerance (use 1 kHz default)
                        if (std::abs(bookmark.frequency - targetFreq) < 1000.0) {
                            *resultName = bookmarkName;
                            flog::debug("FrequencyManager: Found SPECIFIC bookmark '{}' for frequency {:.3f} MHz",
                                        bookmarkName, targetFreq / 1e6);
                            return;
                        }
                    }
                }

                // PRIORITY 2: If no single frequency match, search for band matches (less specific)
                for (const auto& [bookmarkName, bookmark] : _this->bookmarks) {
                    if (bookmark.isBand) {
                        // For bands, check if frequency is within the band range
                        if (targetFreq >= bookmark.startFreq && targetFreq <= bookmark.endFreq) {
                            *resultName = bookmarkName + " [Band]";
                            flog::debug("FrequencyManager: Found BAND name '{}' for frequency {:.3f} MHz",
                                        bookmarkName, targetFreq / 1e6);
                            return;
                        }
                    }
                }

                // No matching bookmark found
                *resultName = "";
                flog::debug("FrequencyManager: No bookmark found for frequency {:.3f} MHz", targetFreq / 1e6);
            }
            else {
                flog::error("FrequencyManager: getBookmarkName called with null pointers");
            }
            break;
        }
        case CMD_IS_FREQ_BLACKLISTED: {
            if (in && out) {
                double freq = *static_cast<double*>(in);
                bool* result = static_cast<bool*>(out);
                *result = _this->isFrequencyBlacklisted(freq);
            }
            else {
                flog::error("FrequencyManager: isFrequencyBlacklisted called with null pointers");
            }
            break;
        }
        case CMD_ADD_BLACKLIST: {
            if (in) {
                double freq = *static_cast<double*>(in);
                _this->addBlacklistEntry(freq);
            }
            else {
                flog::error("FrequencyManager: addBlacklist called with null input");
            }
            break;
        }
        default:
            flog::warn("FrequencyManager: Unknown interface command: {}", code);
            break;
        }
    }
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["selectedList"] = "General";
    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["bookmarks"] = json::object();
    def["lists"]["Blacklist"]["showOnWaterfall"] = false;
    def["lists"]["Blacklist"]["bookmarks"] = json::object();

    config.setPath(core::args["root"].s() + "/frequency_manager_config.json");
    config.load(def);
    config.enableAutoSave();

    // Check if of list and convert if they're the old type
    config.acquire();
    if (!config.conf.contains("bookmarkDisplayMode")) {
        config.conf["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    }
    for (auto [listName, list] : config.conf["lists"].items()) {
        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
        json newList;
        newList = json::object();
        newList["showOnWaterfall"] = true;
        newList["bookmarks"] = list;
        config.conf["lists"][listName] = newList;
    }
    config.release(true);
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FrequencyManagerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FrequencyManagerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
