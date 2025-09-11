# Core Architecture

The core architecture of SDR++CE provides the foundation for all other components. Understanding this layer is essential for any serious development work.

## Application Lifecycle

### Startup Sequence

1. **Argument Parsing** (`sdrpp_main`)
   - Parse command line options (`--root`, `--cfg`, etc.)
   - Determine root directory and config paths
   - Set up logging and error handling

2. **Configuration Loading** (`core::configManager`)
   - Load `config.json` from platform-specific location
   - Apply default configuration schema from `core.cpp`
   - **Repair invalid keys** - removes unknown configuration

3. **Backend Initialization** (`backend::init()`)
   - Initialize GLFW (desktop) or Android backend
   - Create OpenGL context with proper version
   - Set up window, input handling, and icon

4. **Core Initialization** (`MainWindow::init()`)
   - **FFTW Setup**: Create thread-safe FFT plans
   - **Global Objects**: Initialize `sigpath::`, `gui::` namespaces
   - **Module Loading**: Discover and load all modules

5. **Main Loop** (`backend::renderLoop()`)
   - Process platform events (keyboard, mouse)
   - Render UI frame with ImGui
   - Handle real-time DSP processing

## Global Objects

### `core::configManager` (ConfigManager)
**Thread-safe configuration management**

```cpp
// Reading configuration
core::configManager.acquire();
float value = core::configManager.conf["section"]["key"];
core::configManager.release();

// Writing configuration  
core::configManager.acquire();
core::configManager.conf["section"]["key"] = newValue;
core::configManager.release(true); // true = mark dirty for auto-save
```

**Configuration Repair**: Unknown keys are automatically removed during startup to prevent corruption.

### `sigpath::sourceManager` (SourceManager)
**Hardware source coordination**

```cpp
// Register a new source
SourceManager::SourceHandler handler;
handler.ctx = this;
handler.startHandler = startCallback;
sigpath::sourceManager.registerSource("My SDR", &handler);

// Control source
sigpath::sourceManager.selectSource("My SDR");
sigpath::sourceManager.start();
```

### `sigpath::vfoManager` (VFOManager)  
**Virtual receiver management**

```cpp
// Create a VFO
auto vfo = sigpath::vfoManager.createVFO("FM Radio", REF_CENTER, 
                                        0, 200000, 48000, 50, 50);
vfo->setOffset(frequency - centerFreq);
```

## Threading Model

### Critical Rules

| Thread | **Can Do** | **Cannot Do** | **Communication** |
|--------|------------|---------------|-------------------|
| **UI** | Drop frames, block on config | Block DSP thread | Atomic variables |
| **DSP** | Process samples, read atomics | Block, allocate memory | Lock-free patterns |
| **Module** | Block, allocate, file I/O | Interfere with DSP | Thread-safe APIs |

### Thread Communication Patterns

#### UI → DSP (Control)
```cpp
// Pattern: Atomic parameter updates
class MyDSPBlock {
    std::atomic<double> frequency{100e6};
    
public:
    void setFrequency(double f) {
        frequency.store(f, std::memory_order_relaxed);
    }
    
    void process() {
        double f = frequency.load(std::memory_order_relaxed);
        // Use frequency in processing
    }
};
```

#### DSP → UI (Visualization)
```cpp
// Pattern: Mutex-protected buffers
float* buffer = gui::waterfall.getFFTBuffer();  // Locks buf_mtx
memcpy(buffer, fftData, size);                  // Copy data
gui::waterfall.pushFFT();                       // Unlocks, updates display
```

## Configuration System

### Schema-First Design

**Critical**: All configuration keys must be defined in `core/src/core.cpp`:

```cpp
// Default configuration defines valid keys
json defConfig;
defConfig["fftSize"] = 8192;
defConfig["fftRate"] = 20;
defConfig["myModule"]["enabled"] = false;

// Keys not in schema are removed during repair
```

### Platform-Specific Paths

| Platform | Configuration Location |
|----------|----------------------|
| **Windows** | `%APPDATA%\sdrpp\config.json` |
| **Linux** | `~/.config/sdrpp/config.json` |
| **macOS (dev)** | `~/.config/sdrpp/config.json` |
| **macOS (bundle)** | `~/Library/Application Support/sdrpp/config.json` |

The path is determined by the `IS_MACOS_BUNDLE` preprocessor flag.

## Error Handling

### Graceful Degradation
- **Missing modules**: Continue with reduced functionality
- **Hardware failures**: Fall back to file input
- **Config corruption**: Reset to defaults
- **UI errors**: Log and continue rendering

### Logging System
```cpp
flog::debug("Detailed debugging: {}", data);
flog::info("General information: {}", status);  
flog::warn("Warning condition: {}", issue);
flog::error("Error occurred: {}", error);
```

## Performance Architecture

### Memory Management
- **Pre-allocation**: All DSP buffers allocated at startup
- **Alignment**: Cache-line aligned for SIMD (`alignas(64)`)
- **Zero-copy**: Pass pointers, avoid memory copies
- **Pool-based**: Reuse buffers to avoid allocation overhead

### SIMD Optimization
Uses VOLK for cross-platform vector operations:
```cpp
// Complex multiplication with SIMD
volk_32fc_x2_multiply_32fc(output, input1, input2, num_samples);
```

## Common Pitfalls

1. **Config Schema**: Forgetting to add defaults in `core.cpp`
2. **DSP Threading**: Blocking operations in signal path
3. **Memory Allocation**: Dynamic allocation in hot paths
4. **Platform Assumptions**: Hardcoding paths or APIs
5. **Module Dependencies**: Circular or missing dependencies

---

**Next**: [[Module System Overview]] to learn about plugin development