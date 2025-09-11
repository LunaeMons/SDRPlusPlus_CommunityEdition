# Advanced Module Development Patterns

## Overview

This document captures critical patterns, anti-patterns, and lessons learned from implementing complex modules in SDR++ Community Edition. It focuses on advanced scenarios like digital demodulation, network streaming, and inter-module communication that go beyond basic module types.

## Table of Contents

1. [Constructor Safety Patterns](#constructor-safety-patterns)
2. [Threading and Synchronization](#threading-and-synchronization)
3. [Configuration Management Best Practices](#configuration-management-best-practices)
4. [Network and File I/O Patterns](#network-and-file-io-patterns)
5. [DSP Chain Integration](#dsp-chain-integration)
6. [UI Development Patterns](#ui-development-patterns)
7. [Debugging and Troubleshooting](#debugging-and-troubleshooting)
8. [Performance Considerations](#performance-considerations)

## Constructor Safety Patterns

### ❌ CRITICAL: Never Access Sinks/VFOs in Constructors

**Problem:** Virtual function calls and sink access during construction can cause crashes.

```cpp
// ❌ DANGEROUS - Can cause mutex lock failures
class BadModule : public DigitalDemodulatorBase {
    BadModule(std::string name) : DigitalDemodulatorBase(name, protocol, &config) {
        updateProtocolType();  // ❌ Accesses netSink during construction!
        loadConfig();          // ❌ May auto-start network threads!
        initVFO();            // ❌ VFO not ready during construction!
    }
};
```

**Solution:** Defer complex initialization to `enable()` or `initDSP()`:

```cpp
// ✅ SAFE - Proper initialization order
class SafeModule : public DigitalDemodulatorBase {
    SafeModule(std::string name) : DigitalDemodulatorBase(name, protocol, &config) {
        // Only set basic defaults in constructor
        p25Mode = P25_MODE_FSK4;
        showConstellation = true;
        // Note: updateProtocolType() will be called in initDSP()
    }
    
    bool initDSP() override {
        loadConfig();          // ✅ Safe - called when fully constructed
        updateProtocolType();  // ✅ Safe - sinks are ready
        // ... DSP initialization
    }
};
```

### ✅ Constructor Best Practices

1. **Minimal Constructors:** Only set basic defaults and register with core systems
2. **No Virtual Calls:** Never call virtual functions from constructors
3. **No Sink Access:** Don't start network/file operations during construction
4. **Defer Complex Init:** Move DSP setup to `enable()` or `initDSP()`

## Threading and Synchronization

### ❌ Mutex Anti-Patterns

**Problem:** Improper mutex usage can cause deadlocks and crashes.

```cpp
// ❌ DANGEROUS - const_cast on mutex
bool isConnected() const {
    std::lock_guard<std::mutex> lck(const_cast<std::mutex&>(connMtx));  // ❌
    return connection && connection->isOpen();
}
```

**Solution:** Declare mutexes as `mutable` for const methods:

```cpp
// ✅ SAFE - Proper const-correctness
class NetworkSink {
private:
    mutable std::mutex connMtx;  // ✅ mutable allows const method access

public:
    bool isConnected() const {
        std::lock_guard<std::mutex> lck(connMtx);  // ✅ No cast needed
        return connection && connection->isOpen();
    }
};
```

### ✅ Thread-Safe Destruction Pattern

**Problem:** Callbacks can access destroyed objects during destruction.

```cpp
// ❌ DANGEROUS - Race condition during destruction
~MyModule() {
    if (enabled) disable();  // DSP might still call callbacks!
    // Mutex destroyed here, but callbacks might still run
}
```

**Solution:** Use atomic flags and proper shutdown order:

```cpp
// ✅ SAFE - Proper destruction order
class SafeModule {
    std::atomic<bool> destroying{false};
    
public:
    ~SafeModule() {
        destroying = true;        // Signal destruction to callbacks
        if (enabled) disable();   // Stop DSP first
        netSink->stop();         // Stop network threads
        fileSink->stop();        // Stop file threads
        // Now safe to destroy mutexes
    }
    
    static void callback(uint8_t* data, int count, void* ctx) {
        SafeModule* _this = (SafeModule*)ctx;
        if (!_this || _this->destroying) return;  // ✅ Early exit
        // ... safe processing
    }
};
```

### ✅ Network Thread Patterns

**Server Pattern (Recommended for SDR++):**
```cpp
// ✅ SDR++ acts as server, external tools connect as clients
void workerThread() {
    listener = net::listen("0.0.0.0", port);
    while (!shouldStop) {
        connection = listener->accept();  // Wait for client
        // Process data...
    }
}
```

**Client Pattern (Use with caution):**
```cpp
// ⚠️ Only use when SDR++ needs to connect to existing servers
void workerThread() {
    connection = net::connect(hostname, port);
    // Send data...
}
```

## Configuration Management Best Practices

### ✅ Schema-First Configuration

**Problem:** Adding new config keys without updating the default schema causes "unused key" errors.

```cpp
// ❌ MISSING - Config key not in default schema
void menuHandler() {
    if (ImGui::Checkbox("New Feature", &newFeature)) {
        config.acquire();
        config.conf[name]["newFeature"] = newFeature;  // ❌ Will be deleted!
        config.release(true);
    }
}
```

**Solution:** Always update default schema in `core/src/core.cpp`:

```cpp
// ✅ IN core/src/core.cpp - Add to defConfig
defConfig["myModule"]["newFeature"] = false;

// ✅ IN module - Now safe to use
void menuHandler() {
    if (ImGui::Checkbox("New Feature", &newFeature)) {
        config.acquire();
        config.conf[name]["newFeature"] = newFeature;  // ✅ Persisted
        config.release(true);
    }
}
```

### ✅ Robust Config Loading Pattern

```cpp
void loadConfig() {
    config.acquire();
    try {
        if (config.conf[name].contains("complexSetting") && 
            config.conf[name]["complexSetting"].is_number_integer()) {
            complexSetting = config.conf[name]["complexSetting"];
        }
        if (config.conf[name].contains("boolSetting") && 
            config.conf[name]["boolSetting"].is_boolean()) {
            boolSetting = config.conf[name]["boolSetting"];
        }
    } catch (const std::exception& e) {
        flog::error("Config error: {}, using defaults", e.what());
    }
    config.release();
}
```

### ✅ Auto-Start Configuration Pattern

**Problem:** Auto-starting features during construction can cause crashes.

```cpp
// ❌ DANGEROUS - Auto-start in constructor
MyModule() {
    loadConfig();
    if (networkEnabled) {
        startNetwork();  // ❌ Can crash during construction
    }
}
```

**Solution:** Auto-start only when module is enabled:

```cpp
// ✅ SAFE - Auto-start in enable()
void enable() {
    // ... DSP initialization
    enabled = true;
    
    // Auto-start network if enabled in config
    if (networkEnabled && netSink) {
        bool started = netSink->start();
        if (started) {
            flog::info("Auto-started network output");
        }
    }
}
```

## Network and File I/O Patterns

### ✅ Digital Stream Header Format

For network/file protocols, use a standardized header:

```cpp
struct DigitalStreamHeader {
    uint32_t magic = 0x44494749;     // "DIGI" in little-endian
    uint8_t version = 1;
    uint8_t protocolId;              // From ProtocolType enum
    uint16_t symbolRate;             // Symbols per second
    uint32_t sampleRate;             // Samples per second  
    uint8_t bitsPerSymbol;           // 1, 2, 4, etc.
    uint8_t reserved[7];             // Future expansion
} __attribute__((packed));
```

### ✅ Network Sink Architecture

**Server Pattern (Recommended):**
```cpp
class DigitalNetworkSink {
    void workerThread() {
        // SDR++ listens, external tools connect
        listener = net::listen("0.0.0.0", port);
        while (!shouldStop) {
            connection = listener->accept();
            sendHeader();  // Send format info
            processDataQueue();
        }
    }
};
```

**Benefits:**
- External tools can reconnect without restarting SDR++
- Multiple clients can connect (if implemented)
- Follows SDR++ convention (audio network sink is also server)

### ✅ File Recording Patterns

**Professional File Interface:**
```cpp
// Folder selection with picker dialog
FolderSelect folderSelect;

// Filename template system  
char nameTemplate[1024] = "$p_$t_$d-$M-$y_$h-$m-$s";

// Generate actual filename
std::string generateFileName(const std::string& nameTemplate, 
                           const std::string& protocolName) const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    std::string result = nameTemplate;
    result = std::regex_replace(result, std::regex("\\$p"), protocolName);
    result = std::regex_replace(result, std::regex("\\$t"), name);
    // ... other variables
    return result;
}
```

## DSP Chain Integration

### ✅ Digital Demodulation Chain Pattern

**Complete FSK4 Example:**
```cpp
bool initDSP() override {
    double symbolRate = protocolConfig->symbolRate;
    double sampleRate = getRequiredSampleRate();
    
    // 1. GFSK Demodulator
    gfskDemod.init(nullptr, sampleRate, symbolRate, protocolConfig->deviation);
    
    // 2. Clock Recovery (Mueller & Muller)
    clockRecovery.init(&gfskDemod.out, symbolRate / sampleRate, 0.01f, 0.5f, 0.01f);
    
    // 3. Symbol Slicer (FSK4 = 4-level)
    quaternarySlicer.init(&clockRecovery.out);
    
    // 4. Output Handler
    digitalSink.init(&quaternarySlicer.out, digitalStreamHandler, this);
    
    return true;
}
```

### ❌ DSP Chain Anti-Patterns

**Problem:** Creating duplicate data paths bypasses the main handler.

```cpp
// ❌ DANGEROUS - Bypasses main data flow
bool initDSP() override {
    // ... DSP setup
    netSink->init(&quaternarySlicer.out);  // ❌ Direct connection!
    fileSink->init(&quaternarySlicer.out); // ❌ Bypasses handler!
    return true;
}
```

**Solution:** All data flows through a single handler:

```cpp
// ✅ CORRECT - Single data path
bool initDSP() override {
    // ... DSP setup
    digitalSink.init(&quaternarySlicer.out, digitalStreamHandler, this);
    // Handler distributes to network/file sinks
    return true;
}

static void digitalStreamHandler(uint8_t* data, int count, void* ctx) {
    MyModule* _this = (MyModule*)ctx;
    
    // Send to all enabled outputs
    if (_this->networkEnabled) _this->netSink->sendData(data, count);
    if (_this->fileEnabled) _this->fileSink->writeData(data, count);
}
```

## UI Development Patterns

### ✅ Professional File Recording UI

**Complete Pattern from Recorder Module:**
```cpp
// Enable/disable checkbox
if (ImGui::Checkbox("Enable File Recording", &fileRecordingEnabled)) {
    if (!fileRecordingEnabled && isRecording()) {
        stopRecording();  // Auto-stop if disabled
    }
    saveConfig();
}

// Disable controls if not enabled
if (!fileRecordingEnabled) { style::beginDisabled(); }

// Folder selection with picker
ImGui::LeftLabel("Recording Path");
if (folderSelect.render("##rec_path")) {
    if (folderSelect.pathIsValid()) {
        saveConfig();
    }
}

// Name template with variables
ImGui::LeftLabel("Name Template"); 
ImGui::FillWidth();
if (ImGui::InputText("##name_template", nameTemplate, sizeof(nameTemplate))) {
    saveConfig();
}
ImGui::TextWrapped("Variables: $p=protocol, $t=module, $y=year...");

// Live preview
std::string preview = generateFileName(nameTemplate, protocolName) + ".digi";
ImGui::Text("Preview: %s", preview.c_str());

// Recording controls
bool canRecord = folderSelect.pathIsValid();
if (!canRecord) { style::beginDisabled(); }

if (!isRecording()) {
    if (ImGui::Button("Start Recording", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        startRecording();
    }
} else {
    if (ImGui::Button("Stop Recording", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        stopRecording();
    }
    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                      "Recording: %llu samples", getSamplesWritten());
}

if (!canRecord) { style::endDisabled(); }
if (!fileRecordingEnabled) { style::endDisabled(); }
```

### ✅ Network Configuration UI Pattern

```cpp
// Network enable checkbox with auto-start
if (ImGui::Checkbox("Network Output", &networkEnabled)) {
    if (networkEnabled && netSink) {
        netSink->setNetworkConfig(networkHost, networkPort, useUDP);
        bool started = netSink->start();
        if (!started) {
            networkEnabled = false;  // Reset if failed
        }
    } else if (netSink) {
        netSink->stop();
    }
    saveConfig();
}

// Network configuration (only if enabled)
if (!networkEnabled) { style::beginDisabled(); }

ImGui::LeftLabel("Host");
ImGui::SetNextItemWidth(menuWidth * 0.65f);
if (ImGui::InputText("##host", networkHost, sizeof(networkHost))) {
    saveConfig();
}

ImGui::SameLine();
ImGui::SetNextItemWidth(menuWidth * 0.25f);
if (ImGui::InputInt("Port##", &networkPort)) {
    networkPort = std::clamp(networkPort, 1024, 65535);
    saveConfig();
}

if (ImGui::Checkbox("UDP", &useUDP)) {
    saveConfig();
}

if (!networkEnabled) { style::endDisabled(); }
```

## Threading and Synchronization

### ✅ DSP Callback Safety Pattern

**Problem:** DSP callbacks can be called during object destruction.

```cpp
// ❌ UNSAFE - No protection during destruction
static void digitalStreamHandler(uint8_t* data, int count, void* ctx) {
    MyModule* _this = (MyModule*)ctx;
    _this->processData(data, count);  // ❌ May crash if object destroying
}
```

**Solution:** Use atomic flags and early exit:

```cpp
// ✅ SAFE - Protected callback
class SafeModule {
    std::atomic<bool> destroying{false};
    
public:
    ~SafeModule() {
        destroying = true;  // Signal destruction
        // ... cleanup
    }
    
    static void digitalStreamHandler(uint8_t* data, int count, void* ctx) {
        SafeModule* _this = (SafeModule*)ctx;
        
        // Early exit if object invalid or destroying
        if (!_this || !_this->enabled || _this->destroying) {
            return;
        }
        
        _this->processData(data, count);
    }
};
```

### ✅ Worker Thread Management

```cpp
class NetworkSink {
    std::atomic<bool> shouldStop{false};
    std::thread worker;
    
public:
    bool start() {
        shouldStop = false;
        try {
            worker = std::thread(&NetworkSink::workerThread, this);
            return true;
        } catch (const std::exception& e) {
            flog::error("Failed to start worker: {}", e.what());
            return false;
        }
    }
    
    void stop() {
        shouldStop = true;
        queueCV.notify_all();  // Wake up waiting threads
        
        if (worker.joinable()) {
            worker.join();     // Wait for clean shutdown
        }
    }
    
    void workerThread() {
        while (!shouldStop) {
            // ... work with proper shouldStop checks
        }
    }
};
```

## Configuration Management Best Practices

### ✅ Default Schema Registration

**CRITICAL:** All config keys must exist in default schema or they're deleted.

```cpp
// ✅ REQUIRED in core/src/core.cpp
void setDefaults() {
    // Add ALL module config keys to default schema
    defConfig["digitalDemod"]["networkEnabled"] = false;
    defConfig["digitalDemod"]["networkHost"] = "localhost";
    defConfig["digitalDemod"]["networkPort"] = 7356;
    defConfig["digitalDemod"]["recordingPath"] = "%ROOT%/recordings";
    defConfig["digitalDemod"]["nameTemplate"] = "$p_$t_$d-$M-$y_$h-$m-$s";
}
```

### ✅ Configuration Loading Pattern

```cpp
void loadConfig() {
    configManager->acquire();
    
    // Set defaults if section doesn't exist
    if (!configManager->conf.contains(name)) {
        configManager->conf[name]["setting1"] = defaultValue1;
        configManager->conf[name]["setting2"] = defaultValue2;
    }
    
    // Load with type checking
    if (configManager->conf[name].contains("setting1")) {
        setting1 = configManager->conf[name]["setting1"];
    }
    
    configManager->release();
    // Note: Don't auto-start features here if called from constructor!
}
```

### ✅ Platform-Aware Configuration

```cpp
// Handle different config locations
#ifdef IS_MACOS_BUNDLE
    // macOS app bundle: ~/Library/Application Support/sdrpp/config.json
#else
    // Development build: ~/.config/sdrpp/config.json
#endif
```

## Network and File I/O Patterns

### ✅ Port Management

```cpp
// Avoid port conflicts with existing sinks
const int AUDIO_NETWORK_PORT = 7355;    // Existing audio sink
const int DIGITAL_NETWORK_PORT = 7356;  // New digital sink
```

### ✅ File Format Standards

```cpp
// Use structured headers for binary files
struct DigitalFileHeader {
    uint32_t magic = 0x44494749;        // "DIGI"
    uint8_t version = 1;
    uint8_t protocolId;
    uint16_t symbolRate;
    uint32_t sampleRate;
    uint8_t bitsPerSymbol;
    uint32_t totalSamples;               // Updated during recording
    uint8_t reserved[8];
} __attribute__((packed));
```

### ✅ Network Protocol Design

```cpp
// Send header for each new connection/periodically for UDP
void sendHeader() {
    DigitalStreamHeader header;
    header.protocolId = static_cast<uint8_t>(protocolType);
    header.symbolRate = protocolConfig->symbolRate;
    header.sampleRate = protocolConfig->sampleRate;
    header.bitsPerSymbol = protocolConfig->bitsPerSymbol;
    
    connection->write(sizeof(header), 
                     reinterpret_cast<const uint8_t*>(&header));
}
```

## DSP Chain Integration

### ✅ VFO Integration Pattern

```cpp
void enable() {
    // Create VFO with proper parameters
    vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 
                                       0, getRequiredBandwidth(), 
                                       getRequiredSampleRate(), 
                                       getRequiredSampleRate(), 
                                       getRequiredSampleRate(), true);
    
    if (!vfo) {
        flog::error("Failed to create VFO");
        return;
    }
    
    // Initialize DSP chain
    if (!initDSP()) {
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = nullptr;
        return;
    }
    
    // Connect VFO to DSP chain
    vfo->setOutput(&dspChain.in);
    
    // Set VFO parameters after protocolConfig is loaded
    if (protocolConfig) {
        vfo->setSnapInterval(protocolConfig->symbolRate / 10);
    }
    
    startDSP();
    enabled = true;
}
```

### ✅ DSP Block Error Handling

```cpp
bool initDSP() override {
    try {
        // Validate configuration
        if (!protocolConfig || protocolConfig->symbolRate == 0) {
            flog::error("Invalid protocol configuration");
            return false;
        }
        
        // Initialize blocks with error checking
        gfskDemod.init(nullptr, sampleRate, symbolRate, deviation);
        clockRecovery.init(&gfskDemod.out, symbolRate/sampleRate, 0.01f, 0.5f, 0.01f);
        
        return true;
    } catch (const std::exception& e) {
        flog::error("DSP initialization failed: {}", e.what());
        return false;
    }
}
```

## Debugging and Troubleshooting

### ✅ Debug Logging Patterns

```cpp
// Console output visibility (CRITICAL for debugging)
// ❌ WRONG: open App.app (detaches process, no console)
// ✅ CORRECT: ./App.app/Contents/MacOS/binary_name

// Strategic debug logging
void digitalStreamHandler(uint8_t* data, int count, void* ctx) {
    if (count > 0) {
        std::string hex;
        for (int i = 0; i < std::min(8, count); i++) {
            char hexByte[4];
            snprintf(hexByte, sizeof(hexByte), "%02X ", data[i]);
            hex += hexByte;
        }
        flog::debug("Symbols: {} bytes, first: {}", count, hex);
    }
}
```

### ✅ Crash Investigation Pattern

1. **Isolate the problem:**
   - Test minimal builds without new modules
   - Use debug builds with symbols
   - Check constructor vs runtime crashes

2. **Threading issues:**
   - Look for `mutex lock failed` → Constructor/destruction race
   - Look for `pure virtual call` → Constructor calling virtual functions
   - Look for segfaults → Null pointer access, destroyed objects

3. **Configuration issues:**
   - Check for "unused key" messages → Missing default schema
   - Check config file location (bundle vs development paths)
   - Verify JSON type matching (int vs bool vs string)

## Performance Considerations

### ✅ Real-Time DSP Guidelines

1. **Memory Allocation:**
   - Pre-allocate all buffers during initialization
   - Never call `malloc()`/`new` in DSP threads
   - Use fixed-size containers

2. **Thread Priorities:**
   - DSP threads > UI threads > File I/O threads
   - Use `std::thread` with appropriate priority settings

3. **Lock-Free When Possible:**
   - Use `std::atomic` for simple values
   - Prefer single-producer/single-consumer queues
   - Minimize mutex usage in hot paths

### ✅ Symbol Processing Optimization

```cpp
// Efficient symbol processing
void processSymbols(uint8_t* symbols, int count) {
    // Batch processing instead of per-symbol
    const int BATCH_SIZE = 64;
    
    for (int i = 0; i < count; i += BATCH_SIZE) {
        int batchCount = std::min(BATCH_SIZE, count - i);
        processBatch(&symbols[i], batchCount);
    }
}
```

## Module Communication Patterns

### ✅ Inter-Module Communication

```cpp
// Using ModuleComManager for module-to-module communication
class ProviderModule : public ModuleManager::Instance {
public:
    void postInit() override {
        // Register service
        core::modComManager.registerInterface("myService", getServiceHandler, this);
    }
    
private:
    static void getServiceHandler(int code, void* in, void* out, void* ctx) {
        ProviderModule* _this = (ProviderModule*)ctx;
        switch (code) {
            case GET_DATA:
                *((float*)out) = _this->getData();
                break;
        }
    }
};

class ConsumerModule : public ModuleManager::Instance {
    void useService() {
        if (core::modComManager.interfaceExists("myService")) {
            float data;
            core::modComManager.callInterface("myService", GET_DATA, nullptr, &data);
        }
    }
};
```

## Common Pitfalls and Solutions

### ❌ Constructor Virtual Function Calls

**Problem:**
```cpp
// ❌ UNDEFINED BEHAVIOR
class BadModule : public BaseClass {
    BadModule() : BaseClass() {
        initDSP();  // ❌ Virtual function call in constructor!
    }
};
```

**Solution:**
```cpp
// ✅ SAFE
class GoodModule : public BaseClass {
    GoodModule() : BaseClass() {
        // Only basic initialization
    }
    
    void enable() override {
        initDSP();  // ✅ Called after full construction
    }
};
```

### ❌ Config Auto-Start During Construction

**Problem:**
```cpp
// ❌ DANGEROUS
MyModule() {
    loadConfig();
    if (networkEnabled) startNetwork();  // ❌ Can crash
}
```

**Solution:**
```cpp
// ✅ SAFE
void enable() {
    // Auto-start only when module is enabled
    if (networkEnabled) startNetwork();
}
```

### ❌ Improper Mutex Usage

**Problem:**
```cpp
// ❌ CONST-CORRECTNESS VIOLATION
bool isConnected() const {
    std::lock_guard<std::mutex> lck(const_cast<std::mutex&>(mtx));  // ❌
}
```

**Solution:**
```cpp
// ✅ PROPER CONST-CORRECTNESS
class NetworkSink {
    mutable std::mutex mtx;  // ✅ Allows const method access
public:
    bool isConnected() const {
        std::lock_guard<std::mutex> lck(mtx);  // ✅ No cast needed
    }
};
```

## Testing and Validation

### ✅ Module Testing Checklist

1. **Startup Testing:**
   - [ ] App starts without crashes
   - [ ] Module appears in correct menu
   - [ ] Multiple instances can be created

2. **Lifecycle Testing:**
   - [ ] Enable/disable works without crashes
   - [ ] Configuration persists across restarts
   - [ ] Clean shutdown without hanging

3. **Threading Testing:**
   - [ ] No race conditions during destruction
   - [ ] Proper thread cleanup on disable
   - [ ] No mutex deadlocks

4. **Integration Testing:**
   - [ ] Works with different source modules
   - [ ] No interference with other modules
   - [ ] Proper resource cleanup

### ✅ Debug Build Configuration

```cmake
# Enable debug symbols and sanitizers
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address -fsanitize=thread")
```

## Architecture Evolution

### ✅ Adding New Protocol Support

**Base Class Pattern:**
```cpp
// 1. Create abstract base class
class DigitalDemodulatorBase : public ModuleManager::Instance {
protected:
    virtual bool initDSP() = 0;
    virtual void startDSP() = 0;
    virtual void stopDSP() = 0;
    // ... common functionality
};

// 2. Implement specific protocols
class P25Demodulator : public DigitalDemodulatorBase {
    bool initDSP() override {
        // P25-specific DSP chain
    }
};

class DMRDemodulator : public DigitalDemodulatorBase {
    bool initDSP() override {
        // DMR-specific DSP chain  
    }
};
```

### ✅ External Tool Integration

**Hybrid C++/Python Approach:**
- **C++:** High-performance real-time DSP (demodulation, filtering, slicing)
- **Python:** Complex protocol decoding (P25, DMR, TETRA parsing)
- **Interface:** Network streams with standardized headers

**Benefits:**
- Leverages existing Python libraries (OP25, GNU Radio)
- Keeps real-time constraints in C++
- Allows rapid prototyping of new protocols

## Summary

The key lessons for advanced module development:

1. **Constructor Safety:** Never access complex systems during construction
2. **Threading:** Use proper synchronization and destruction patterns
3. **Configuration:** Always update default schema, handle types safely
4. **UI Standards:** Follow existing patterns for professional interfaces
5. **DSP Integration:** Single data path, proper VFO lifecycle
6. **External Integration:** Use network/file interfaces for complex protocols

These patterns ensure stable, maintainable, and performant modules that integrate seamlessly with SDR++ Community Edition.
