## 1. Core Architecture & Application Lifecycle (Deep Dive)

SDR++CE is a multi-threaded application with a strict separation between the User Interface (UI), the Digital Signal Processing (DSP) engine, and the core application logic. This separation is crucial for maintaining real-time performance in the DSP path while providing a responsive user experience.

### 1.1. Threading Model Explained

The application's stability and performance rely on its multi-threaded design. Understanding this is critical for any developer.

1.  **Main/UI Thread:** This is the application's entry point (`sdrpp_main`). It is responsible for all UI rendering via ImGui and handling user input. Because UI operations can sometimes block or stutter, **no real-time DSP calculations are ever performed on this thread.** Its primary role in respect to the DSP engine is to dispatch control commands (e.g., changing frequency, selecting a demodulator) in a thread-safe way.

2.  **DSP Thread(s):** These are high-priority, real-time threads spawned by the DSP components themselves. For instance, `dsp::buffer::SampleFrameBuffer` creates a worker thread to consume samples from an input stream and write them to an internal ring buffer.
    *   **Real-Time Criticality:** Code executing in these threads must be "real-time safe." This means developers must **strictly avoid**:
        *   **Heap Allocations:** `new`, `malloc`, `std::vector::push_back` (if it causes a reallocation), etc. Memory should be allocated during initialization.
        *   **Blocking I/O:** Reading/writing to files, network sockets, etc.
        *   **Contended Locks:** Using standard mutexes that could be held by the lower-priority UI thread, leading to priority inversion and audio dropouts. Communication should use lock-free queues or atomics where possible.
    *   **Communication:**
        *   **Data Streams:** IQ and audio data are passed between DSP blocks using the `dsp::stream` class, which implements a mutex-protected double-buffering mechanism. While not truly lock-free, it provides efficient data transfer by swapping read and write buffers, minimizing contention between producer and consumer threads.
        *   **Control Parameters:** Parameters from the UI (e.g., a VFO's frequency offset) are typically passed to the DSP thread using `std::atomic` variables or, for more complex data, values protected by a lightweight spinlock or mutex that is held for the briefest possible time.

3.  **Module Threads:** Source modules that interface with hardware (like an RTL-SDR) or network streams (`spyserver_source`) create their own dedicated threads. The sole purpose of these threads is to read data from the source and `write()` it into the `dsp::stream` that feeds the main `IQFrontEnd`. This isolates the hardware/network latency from the main DSP processing pipeline.

4.  **Config Auto-Save Thread:** A low-priority background thread that periodically saves the application configuration. This avoids blocking the main UI thread with file I/O.

### 1.2. Application Initialization (`sdrpp_main` Walkthrough)

The startup sequence in `core/src/core.cpp` is critical for establishing the application's state.

1.  **Argument Parsing & Root Path:** The application first parses command-line arguments. The most important is `--root`, which defines the path where `config.json` and other user data are stored. Platform-specific logic ensures the application changes its working directory correctly, especially for macOS app bundles.
2.  **Configuration Loading (`ConfigManager`):**
    *   A large `json` object named `defConfig` is created in `core.cpp`. This object represents the **complete schema** of a valid configuration file, containing every possible key with a default value.
    *   `core::configManager.setPath(...)` is called to set the config file location.
    *   `core::configManager.load(defConfig)` is invoked. This function:
        *   If `config.json` does not exist, it's created and populated with `defConfig`.
        *   If it exists, it's loaded and parsed. If parsing fails due to corruption, the config is reset to `defConfig`.
            *   **Configuration Repair (in `core.cpp`):** After loading, the main initialization code performs schema validation:
            *   It iterates through `defConfig` and adds any missing keys to the loaded configuration.
            *   It then iterates through the loaded config and removes any keys not present in `defConfig` (logged as "Unused key in config [keyName], repairing").
            *   This two-pass validation ensures configuration compatibility across versions and prevents unknown keys from persisting.
            *   **CRITICAL FOR DEVELOPERS:** All module configuration keys MUST be added to `defConfig` in `core/src/core.cpp`, or they will be automatically deleted during startup. This is a common source of "settings not persisting" issues.
    *   `core::configManager.enableAutoSave()` spawns the background save thread that periodically persists changes.
3.  **Backend and GUI Initialization:**
    *   `backend::init(resDir)` initializes the windowing system (GLFW) and graphics context (OpenGL).
    *   `SmGui::init(false)` initializes the ImGui context.
    *   Fonts (`style::loadFonts`), themes (`thememenu::init`), and icons (`icons::load`) are loaded from the resource directory specified in the config.
4.  **Module Loading and Instantiation (`ModuleManager`):**
    *   The `ModuleManager` reads the `modulesDirectory` from the config.
    *   It iterates through all shared libraries (`.dll`, `.so`, `.dylib`) in that directory.
    *   For each library, it uses `dlopen`/`LoadLibrary` to load it into memory. It then uses `dlsym`/`GetProcAddress` to find the exported `_INFO_`, `init`, `createInstance`, `deleteInstance`, and `end` symbols. If successful, the module is added to the `modules` map.
    *   It then iterates through the `moduleInstances` map in the configuration. For each entry, it calls `createInstance` on the corresponding loaded module. The returned `ModuleManager::Instance*` is stored in the `instances` map.
5.  **Module Post-Initialization:** `core::moduleManager.doPostInitAll()` is called. This iterates through all created instances and calls their `postInit()` method. This is the crucial step where modules can interact with each other and the core systems, as they are all guaranteed to be loaded and instantiated at this point.
6.  **Render Loop:** `backend::renderLoop()` begins the main event loop, which continuously draws the UI until the window is closed.
7.  **Shutdown:** When the loop exits, `backend::end()` is called, the signal path is stopped, and `core::configManager.save()` is called one last time to ensure the final state is persisted.

### 1.3. The `core` and `sigpath` Global Objects

These namespaces provide global, singleton-like access to the main application components. This service locator pattern is used extensively by modules to interact with the core functionality without needing to pass pointers around.

**Core Namespace Objects:**
*   `core::configManager`: `ConfigManager` - Manages application settings with thread-safe access and auto-save functionality.
*   `core::moduleManager`: `ModuleManager` - Manages module lifecycle (loading, instantiation, and destruction).
*   `core::modComManager`: `ModuleComManager` - Facilitates inter-module communication through named endpoints.
*   `core::args`: `CommandArgsParser` - Parses and stores command-line arguments.

**Signal Path Namespace Objects:**
*   `sigpath::iqFrontEnd`: `IQFrontEnd` - **The central DSP processing chain.** Initialized in `MainWindow::init()` with:
    *   FFT processing (using FFTW plans created during initialization)
    *   Configurable FFT size (default 65536 samples)
    *   Windowing functions (Nuttall by default)
    *   Sample rate management and decimation
*   `sigpath::vfoManager`: `VFOManager` - Manages Virtual Frequency Oscillators (VFOs):
    *   Creation and deletion of VFOs
    *   Frequency offset and bandwidth management
    *   Color assignment for UI display
*   `sigpath::sourceManager`: `SourceManager` - Registry and controller for input sources:
    *   Source selection and switching
    *   Sample rate negotiation
    *   Tuning commands
*   `sigpath::sinkManager`: `SinkManager` - Registry for audio output modules:
    *   Sink registration and selection
    *   Audio routing from demodulators

### 1.4. Configuration System (`ConfigManager`) In-Depth

Correctly interacting with the `ConfigManager` is essential for module developers.

*   **Accessing Config:** Reading a value is simple, but must be done within an acquire/release block to ensure thread safety.

    ```cpp
    // Example: Reading the FFT size from config
    core::configManager.acquire();
    int fftSize = core::configManager.conf["fftSize"];
    core::configManager.release(); // modified = false (default)
    ```

*   **Modifying Config:** When a UI element (like a checkbox or slider) modifies a setting, it must set the `modified` flag to `true` in the `release()` call.

    ```cpp
    // Example: A checkbox in a module's UI
    core::configManager.acquire();
    bool myModuleEnabled = core::configManager.conf["myModule"]["enabled"];
    if (ImGui::Checkbox("Enable My Module", &myModuleEnabled)) {
        core::configManager.conf["myModule"]["enabled"] = myModuleEnabled;
        core::configManager.release(true); // Mark config as modified
    }
    else {
        core::configManager.release(false); // No change
    }
    ```

*   **Adding Module-Specific Defaults:** To ensure a module's settings are always present and valid, it should add its default values to the `defConfig` object during its `init()` phase. This is done via a `ModuleComManager` endpoint.

    *This is an advanced topic and will be detailed further in the Module System document.*

### 1.5. Platform-Specific Considerations

**macOS Application Bundles:**
*   When built with `USE_BUNDLE_DEFAULTS=ON` and `IS_MACOS_BUNDLE` is defined, the application behavior changes:
    *   Working directory is set to the `.app/Contents/` directory
    *   Modules are loaded from `Contents/Plugins/` instead of a system directory
    *   Configuration is stored in `~/Library/Application Support/sdrpp/` instead of `~/.config/sdrpp/`
    *   Resource paths are relative to the bundle structure

**Windows Considerations:**
*   Console window is hidden unless `--con` flag is used
*   Module files use `.dll` extension
*   Error dialogs are suppressed via `SetErrorMode()`

**Android Specifics:**
*   Module paths must be relative, not absolute
*   Shutdown sequence is different due to Android lifecycle
*   Configuration stored in app-specific storage

### 1.6. Common Pitfalls and Important Notes

1. **DSP Stream is Not Lock-Free:** Despite performance goals, `dsp::stream` uses mutexes for synchronization. Developers should be aware of potential blocking, though the double-buffering design minimizes contention.

2. **Configuration Validation Timing:** Config repair happens in `core.cpp` after `ConfigManager::load()`, not within the ConfigManager itself. Modules should not assume their config keys exist until after the repair phase.

3. **Module Loading Order:** Modules are loaded in filesystem order, which may vary by platform. Dependencies between modules should be handled in `postInit()`, not during construction.

4. **FFTW Thread Safety:** FFTW plan creation/destruction is not thread-safe. Plans must be created during initialization on the main thread.

5. **Real-Time Constraints:** While DSP threads aim for real-time performance, they still use some blocking primitives. Critical paths should minimize time holding locks.

## Architecture Documentation Index

This core architecture document is part of a comprehensive series:

- **[01_core_architecture.md](./01_core_architecture.md)** - Application lifecycle and global systems (this document)
- **[02_module_system.md](./02_module_system.md)** - Module types, lifecycle, and basic patterns
- **[03_signal_path_dsp.md](./03_signal_path_dsp.md)** - DSP engine, signal flow, and processing blocks
- **[04_ui_system.md](./04_ui_system.md)** - UI architecture and ImGui integration
- **[05_advanced_module_patterns.md](./05_advanced_module_patterns.md)** - Complex module patterns, threading, and integration
- **[06_debugging_troubleshooting.md](./06_debugging_troubleshooting.md)** - Systematic debugging approaches and common issues

For module developers, start with documents 01-02 for basics, then review 05-06 for advanced patterns and troubleshooting techniques.
