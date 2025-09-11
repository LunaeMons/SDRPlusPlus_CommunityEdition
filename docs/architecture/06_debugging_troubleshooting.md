# Debugging and Troubleshooting Guide

## Overview

This document provides systematic approaches to debugging common issues in SDR++ module development, based on real-world troubleshooting experience from complex module implementations.

## Table of Contents

1. [Crash Analysis](#crash-analysis)
2. [Configuration Issues](#configuration-issues)
3. [Threading Problems](#threading-problems)
4. [macOS-Specific Debugging](#macos-specific-debugging)
5. [Network and I/O Issues](#network-and-io-issues)
6. [Build and Linking Problems](#build-and-linking-problems)
7. [Performance Debugging](#performance-debugging)

## Crash Analysis

### Constructor Crashes

**Symptoms:**
- App crashes immediately on module creation
- `mutex lock failed` errors
- Pure virtual function call crashes

**Investigation Steps:**

1. **Check Constructor Code:**
```cpp
// ❌ Common crash causes in constructors:
MyModule() : BaseClass() {
    initDSP();           // Virtual function call - undefined behavior
    netSink->start();    // Accessing sinks before full construction
    loadConfig();        // May trigger auto-start features
}
```

2. **Test Minimal Constructor:**
```cpp
// ✅ Safe minimal constructor for testing
MyModule(std::string name) : BaseClass(name, protocol, &config) {
    // Only basic member initialization - no complex operations
}
```

3. **Move Complex Init to enable():**
```cpp
void enable() override {
    if (!initDSP()) {
        flog::error("Failed to initialize DSP");
        return;
    }
    loadConfig();  // Safe after full construction
    enabled = true;
}
```

### Destruction Crashes

**Symptoms:**
- Crashes when disabling/destroying modules
- Segmentation faults during app shutdown
- Thread-related crashes

**Investigation Pattern:**

1. **Check Destruction Order:**
```cpp
~MyModule() {
    destroying = true;        // ✅ Signal destruction first
    if (enabled) disable();   // ✅ Stop DSP threads
    stopAllWorkerThreads();   // ✅ Stop background threads
    // ✅ Mutexes destroyed last
}
```

2. **Protect Callbacks:**
```cpp
static void callback(uint8_t* data, int count, void* ctx) {
    MyModule* _this = (MyModule*)ctx;
    
    // ✅ Early exit if object invalid
    if (!_this || _this->destroying) return;
    
    // ✅ Safe to access object members now
    _this->processData(data, count);
}
```

## Configuration Issues

### "Settings Not Persisting" Investigation

**Step 1: Check Startup Logs**
```bash
# Run with console output to see config messages
./SDR++.app/Contents/MacOS/sdrpp_ce 2>&1 | grep -E "(unused key|repairing|config)"
```

**Common Log Messages:**
- `"Unused key in config [keyName], repairing"` → Missing from default schema
- `"Config file not found, creating"` → First run or wrong path
- `"Failed to parse config"` → JSON syntax error

**Step 2: Verify Default Schema**
```cpp
// ✅ REQUIRED in core/src/core.cpp
void setDefaults() {
    // Every module config key must be here
    defConfig["myModule"]["networkEnabled"] = false;
    defConfig["myModule"]["networkPort"] = 7356;
    defConfig["myModule"]["recordingPath"] = "%ROOT%/recordings";
}
```

**Step 3: Check Config File Location**
```cpp
// Different paths based on build type:
#ifdef IS_MACOS_BUNDLE
    // ~/Library/Application Support/sdrpp/config.json
#else  
    // ~/.config/sdrpp/config.json
#endif
```

**Step 4: Manual Config Test**
```bash
# Find actual config location
./SDR++.app/Contents/MacOS/sdrpp_ce --help
# Manually edit config.json to test loading vs saving
```

### Configuration Type Mismatches

**Problem:** Config values don't match expected types.

```cpp
// ❌ CRASH RISK - No type checking
int port = config.conf[name]["port"];  // Crashes if not integer

// ✅ SAFE - Type checking
int port = 7356;  // Default
if (config.conf[name].contains("port") && 
    config.conf[name]["port"].is_number_integer()) {
    port = config.conf[name]["port"];
}
```

## Threading Problems

### Mutex Lock Failed Errors

**Root Cause Analysis:**

1. **Constructor Virtual Calls:**
```cpp
// ❌ CAUSE: Virtual function in constructor
class BadModule : public BaseClass {
    BadModule() : BaseClass() {
        updateProtocol();  // ❌ Virtual call before object fully constructed
    }
};
```

2. **Sink Access During Construction:**
```cpp
// ❌ CAUSE: Accessing sinks before ready
MyModule() {
    netSink->configure();  // ❌ Sink not fully initialized
}
```

**Solution Pattern:**
```cpp
// ✅ SAFE: Defer to enable()
void enable() override {
    if (!BaseClass::enable()) return;  // Call parent first
    
    updateProtocol();     // ✅ Safe after full construction
    configureNetwork();   // ✅ Sinks are ready
}
```

### Thread Cleanup Issues

**Detection:**
- App hangs on shutdown
- Threads not joining properly
- Resource leaks

**Solution Pattern:**
```cpp
class SafeThreading {
    std::atomic<bool> shouldStop{false};
    std::thread worker;
    std::condition_variable cv;
    
public:
    void start() {
        shouldStop = false;
        worker = std::thread(&SafeThreading::workerLoop, this);
    }
    
    void stop() {
        shouldStop = true;
        cv.notify_all();      // Wake up waiting threads
        
        if (worker.joinable()) {
            worker.join();    // Wait for clean shutdown
        }
    }
    
private:
    void workerLoop() {
        while (!shouldStop) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] { return !queue.empty() || shouldStop; });
            
            if (shouldStop) break;  // Check after wakeup
            
            // Process work...
        }
    }
};
```

## macOS-Specific Debugging

### Console Output Visibility

**CRITICAL:** Never use `open App.app` for debugging - console output is lost.

```bash
# ❌ WRONG: Detaches process, no console output
open SDR++.app

# ✅ CORRECT: Run binary directly to see printf/logs
./SDR++.app/Contents/MacOS/sdrpp_ce
```

### App Bundle vs Build Directory

**Different Behavior Investigation:**

1. **Check Build Flags:**
```bash
# App bundle requires specific flags
cmake -DUSE_BUNDLE_DEFAULTS=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 ..
```

2. **Config Path Differences:**
```cpp
// Build directory: ~/.config/sdrpp/config.json
// App bundle: ~/Library/Application Support/sdrpp/config.json
```

3. **Module Loading Paths:**
```bash
# Build directory: ./build/decoder_modules/my_module/my_module.dylib
# App bundle: ./App.app/Contents/PlugIns/my_module.dylib
```

### Code Signing Issues

**Symptoms:**
- `install_name_tool: warning: changes being made to the file will invalidate the code signature`
- Modules fail to load in app bundle

**Solution:**
```bash
# Re-sign after bundle creation (automatic in make_macos_bundle.sh)
codesign --force --deep --sign - ./SDR++.app
```

## Network and I/O Issues

### Network Connection Debugging

**Investigation Steps:**

1. **Check Port Conflicts:**
```bash
# See what's using ports
netstat -an | grep 7356
lsof -i :7356
```

2. **Test Network Connectivity:**
```bash
# Test if SDR++ is listening
telnet localhost 7356

# Test UDP reception
nc -u localhost 7356
```

3. **Debug Network State:**
```cpp
void debugNetworkState() {
    flog::debug("Network enabled: {}", networkEnabled);
    flog::debug("Sink exists: {}", netSink != nullptr);
    if (netSink) {
        flog::debug("Sink running: {}", netSink->getSink()->isRunning());
        flog::debug("Sink connected: {}", netSink->getSink()->isConnected());
    }
}
```

### File I/O Debugging

**Common Issues:**

1. **Path Validation:**
```cpp
// Check if recording path exists and is writable
bool validatePath(const std::string& path) {
    try {
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path);
        }
        
        // Test write permission
        std::string testFile = path + "/.test_write";
        std::ofstream test(testFile);
        if (test.is_open()) {
            test.close();
            std::filesystem::remove(testFile);
            return true;
        }
    } catch (const std::exception& e) {
        flog::error("Path validation failed: {}", e.what());
    }
    return false;
}
```

2. **File Handle Management:**
```cpp
class SafeFileRecorder {
    std::ofstream file;
    std::mutex fileMtx;
    
public:
    bool startRecording(const std::string& path) {
        std::lock_guard<std::mutex> lock(fileMtx);
        
        if (file.is_open()) {
            file.close();  // Close previous file
        }
        
        file.open(path, std::ios::binary);
        return file.is_open();
    }
    
    void stopRecording() {
        std::lock_guard<std::mutex> lock(fileMtx);
        if (file.is_open()) {
            file.flush();  // Ensure data is written
            file.close();
        }
    }
};
```

## Build and Linking Problems

### Missing Module in App Bundle

**Investigation:**

1. **Check Build Success:**
```bash
# Verify module built successfully
ls -la build/decoder_modules/*/
```

2. **Check Bundle Script:**
```bash
# Verify make_macos_bundle.sh includes your module
grep "my_module" make_macos_bundle.sh
```

3. **Check App Bundle Contents:**
```bash
# See what modules are actually in the bundle
ls -la SDR++.app/Contents/PlugIns/
```

### CMake Configuration Issues

**Common Problems:**

```cmake
# ❌ Module not built
option(OPT_BUILD_MY_MODULE "Build my module" OFF)  # Default OFF!

# ✅ Enable module
option(OPT_BUILD_MY_MODULE "Build my module" ON)   # Default ON
```

### Dependency Issues

**Investigation:**

1. **Check Dependencies:**
```bash
# macOS: Check what libraries are linked
otool -L my_module.dylib

# Linux: Check dependencies  
ldd my_module.so
```

2. **Missing Dependencies:**
```bash
# Install missing libraries
brew install missing-lib

# Update CMake to find them
find_package(MissingLib REQUIRED)
target_link_libraries(my_module PRIVATE MissingLib::MissingLib)
```

## Performance Debugging

### DSP Performance Analysis

**Profiling DSP Chains:**

1. **Add Timing Points:**
```cpp
#ifdef DEBUG_TIMING
    auto start = std::chrono::high_resolution_clock::now();
#endif

    // DSP processing...

#ifdef DEBUG_TIMING
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    flog::debug("DSP processing took: {} μs", duration.count());
#endif
```

2. **Buffer Underrun Detection:**
```cpp
// Monitor stream health
if (stream->getReadLatency() > threshold) {
    flog::warn("Stream latency high: {} samples", stream->getReadLatency());
}
```

### Memory Usage Analysis

**Common Issues:**

1. **Memory Leaks in DSP Paths:**
```cpp
// ❌ Allocates memory in hot path
void processSymbols(uint8_t* symbols, int count) {
    std::vector<float> temp(count);  // ❌ Allocation in RT thread!
}

// ✅ Pre-allocated buffers
class Processor {
    std::vector<float> tempBuffer;
    
public:
    Processor() {
        tempBuffer.resize(MAX_SYMBOLS);  // ✅ Pre-allocate
    }
    
    void processSymbols(uint8_t* symbols, int count) {
        // Use tempBuffer.data() - no allocation
    }
};
```

## Systematic Debugging Approach

### 1. Isolate the Problem

**Build Minimal Test:**
```bash
# Test without new modules
cmake -DOPT_BUILD_MY_MODULE=OFF ..
make -j8
./SDR++_Test.app/Contents/MacOS/sdrpp_ce
```

**Binary Search Module Issues:**
```bash
# Disable half the modules, see if crash persists
cmake -DOPT_BUILD_RADIO=OFF -DOPT_BUILD_SCANNER=OFF ..
```

### 2. Enable Debug Information

**Debug Build:**
```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-g -O0" ..
```

**Address Sanitizer:**
```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -g" ..
```

**Thread Sanitizer:**
```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" ..
```

### 3. Strategic Logging

**Module Lifecycle Logging:**
```cpp
class DebuggableModule {
public:
    DebuggableModule(std::string name) : name(name) {
        flog::debug("Module {} constructor start", name);
        // ... safe initialization
        flog::debug("Module {} constructor complete", name);
    }
    
    ~DebuggableModule() {
        flog::debug("Module {} destructor start", name);
        // ... cleanup
        flog::debug("Module {} destructor complete", name);
    }
    
    void enable() override {
        flog::debug("Module {} enable start", name);
        // ... enable logic
        flog::debug("Module {} enable complete", name);
    }
};
```

### 4. Network Debugging Tools

**Test Network Connectivity:**
```bash
# Check if SDR++ is listening
netstat -an | grep 7356

# Test connection manually
telnet localhost 7356

# Capture network traffic
sudo tcpdump -i lo0 port 7356
```

**Python Test Client:**
```python
import socket
import struct

# Connect to SDR++ digital stream
sock = socket.socket(socket.AF_INET, socket.SOCK_TCP)
sock.connect(('localhost', 7356))

# Read header
header = sock.recv(20)
magic, version, protocol_id = struct.unpack('<IBB', header[:6])
print(f"Magic: 0x{magic:08x}, Version: {version}, Protocol: {protocol_id}")

# Read data
while True:
    data = sock.recv(1024)
    if not data:
        break
    print(f"Received {len(data)} bytes")
```

### 5. File I/O Debugging

**Test File Recording:**
```bash
# Check if files are created
ls -la ~/Documents/SDR++/recordings/

# Check file contents
hexdump -C recording.digi | head

# Check file permissions
ls -la recordings/
```

## Common Error Patterns

### Pattern: "Module Loads but Doesn't Appear"

**Causes:**
1. Module not included in app bundle
2. Missing dependencies
3. Symbol export issues

**Investigation:**
```bash
# Check if module exists in bundle
ls -la SDR++.app/Contents/PlugIns/ | grep my_module

# Check symbols
nm my_module.dylib | grep _INFO_

# Check dependencies
otool -L my_module.dylib
```

### Pattern: "Works in Build Directory, Fails in App Bundle"

**Causes:**
1. Missing `-DUSE_BUNDLE_DEFAULTS=ON` flag
2. Config path differences
3. Resource path issues
4. Missing module in bundle script

**Investigation:**
```bash
# Compare config paths
./build/sdrpp_ce --help
./SDR++.app/Contents/MacOS/sdrpp_ce --help

# Check build flags
grep -r "IS_MACOS_BUNDLE" build/
```

### Pattern: "Settings Reset on Restart"

**Root Cause:** Config keys not in default schema.

**Investigation:**
```bash
# Check for repair messages
./SDR++.app/Contents/MacOS/sdrpp_ce 2>&1 | grep "unused key"

# Verify schema in core.cpp
grep -A5 -B5 "myModule" core/src/core.cpp
```

## Debug Build Configuration

### Comprehensive Debug CMake

```bash
#!/bin/bash
# debug_build.sh - Complete debug configuration

rm -rf build_debug
mkdir build_debug
cd build_debug

cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-g -O0 -DDEBUG_TIMING -DDEBUG_NETWORK" \
  -DUSE_BUNDLE_DEFAULTS=ON \
  -DOPT_BUILD_P25_DIGITAL_DEMOD=ON \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
  ..

make -j8
../make_macos_bundle.sh . ../SDR++_Debug.app
```

### Debug Logging Configuration

```cpp
// In module constructor
#ifdef DEBUG
    flog::setLevel(flog::Level::Debug);
#endif

// Strategic debug points
void criticalFunction() {
    flog::debug("Entering criticalFunction");
    
    try {
        // ... operations
        flog::debug("criticalFunction success");
    } catch (const std::exception& e) {
        flog::error("criticalFunction failed: {}", e.what());
        throw;
    }
}
```

## Troubleshooting Workflow

### 1. Initial Assessment

1. **Reproduce Consistently:** Can you trigger the issue reliably?
2. **Isolate Variables:** Does it happen with specific modules/configs?
3. **Check Recent Changes:** What was modified since last working state?

### 2. Systematic Investigation

1. **Start Simple:** Test with minimal configuration
2. **Add Complexity:** Enable features one by one
3. **Compare Environments:** Build directory vs app bundle
4. **Check Dependencies:** Verify all required libraries present

### 3. Root Cause Analysis

1. **Read Error Messages:** Don't ignore warnings or log messages
2. **Check Timing:** Constructor vs enable() vs runtime issues
3. **Verify Assumptions:** Test expected vs actual behavior
4. **Use Debugger:** GDB/LLDB for crash analysis when needed

### 4. Fix Validation

1. **Test Fix Thoroughly:** Verify issue is actually resolved
2. **Regression Testing:** Ensure fix doesn't break other functionality
3. **Document Solution:** Update architecture docs with lessons learned
4. **Create Test Case:** Add automated test to prevent regression

## Emergency Debugging Commands

**Quick Module Disable:**
```bash
# Temporarily disable problematic module
cmake -DOPT_BUILD_PROBLEMATIC_MODULE=OFF ..
make -j8
```

**Safe Mode Build:**
```bash
# Build with minimal modules for testing
cmake \
  -DOPT_BUILD_RADIO=OFF \
  -DOPT_BUILD_SCANNER=OFF \
  -DOPT_BUILD_DIGITAL_DEMOD=OFF \
  ..
```

**Memory Debugging:**
```bash
# Run with address sanitizer
ASAN_OPTIONS=detect_leaks=1 ./SDR++.app/Contents/MacOS/sdrpp_ce
```

**Thread Debugging:**
```bash
# Run with thread sanitizer  
TSAN_OPTIONS=halt_on_error=1 ./SDR++.app/Contents/MacOS/sdrpp_ce
```

This systematic approach to debugging ensures that issues are resolved thoroughly and the solutions are documented for future reference.
