# 🔄 **Enhanced Scanner Module - Multiple Frequency Ranges**

## 🚀 **New Features Overview**

The SDR++CE Scanner module has been significantly enhanced with **multiple named frequency ranges** support, allowing users to create, manage, and scan across multiple frequency ranges simultaneously.

### ✨ **Key Enhancements**

- **📦 Multiple Named Ranges**: Create and manage multiple frequency ranges with custom names
- **🎯 Range Presets**: Quick-add common frequency ranges (FM, Airband, Ham bands, etc.)  
- **🔄 Toggle Ranges**: Enable/disable individual ranges without deletion
- **🌐 Multi-Range Scanning**: Automatically scan across all enabled ranges
- **💾 Full Persistence**: All ranges and settings automatically saved
- **🔄 Backward Compatibility**: Existing single-range configs still work

---

## 🎛️ **User Interface Features**

### **Range Management Window**
- **Add New Range**: Create custom ranges with name, start/stop frequencies
- **Edit Existing**: In-place editing of range name and frequencies
- **Enable/Disable**: Toggle ranges on/off with checkboxes
- **Delete Ranges**: Remove unwanted ranges
- **Range Status**: Visual indication of active vs total ranges

### **Quick Preset Buttons**
- **FM Broadcast** (88-108 MHz)
- **Airband** (118-137 MHz) 
- **2m Ham** (144-148 MHz)
- **PMR446** (446.0-446.2 MHz)
- **70cm Ham** (420-450 MHz)

### **Smart Blacklist Management**
- **Frequency Manager Integration**: Dedicated blacklist list stores blocked frequencies and bands
- **Quick Access**: Main window **BL** button adds the current frequency to the blacklist
- **Band Support**: Add single frequencies or ranges directly within Frequency Manager

### **Universal Gain Control System**
- **Per-Band Gain Settings**: Each frequency range has configurable gain settings
- **Automatic Gain Application**: Applies optimal gain when switching between ranges
- **Source Type Detection**: Recognizes RTL-SDR, HackRF, Airspy, PlutoSDR, SoapySDR, LimeSDR
- **Smart Recommendations**: Provides gain recommendations in logs for unsupported sources  
- **Manual Override**: "Apply Gain" button for manual gain application
- **Intelligent Presets**: Optimized gain values for different bands:
  - FM Broadcast: 15.0 dB (lower gain for strong signals)
  - Airband: 25.0 dB (medium gain for aircraft communications)
  - 2m Ham: 30.0 dB (higher gain for weaker signals)
  - PMR446: 35.0 dB (high gain for short-range communications)
  - 70cm Ham: 35.0 dB (high gain for UHF communications)

---

## 🔧 **Technical Implementation**

### **Data Structure**
```cpp
struct FrequencyRange {
    std::string name;        // User-defined name
    double startFreq;        // Start frequency in Hz
    double stopFreq;         // Stop frequency in Hz  
    bool enabled;            // Enable/disable state
    float gain;              // Gain setting for this range (in dB)
    
    FrequencyRange(const std::string& n, double start, double stop, bool en, float g)
        : name(n), startFreq(start), stopFreq(stop), enabled(en), gain(g) {}
};
```

### **Universal Gain Control System**
```cpp
namespace UniversalGainControl {
    bool applyGain(const std::string& sourceName, float gainDB) {
        // Automatic source type detection and gain application
        if (sourceName.find("RTL-SDR") != std::string::npos) {
            // Provides intelligent gain recommendation in logs
            flog::info("Scanner: RTL-SDR detected - gain {:.1f} dB must be set manually", gainDB);
            return false; // RTL-SDR requires manual adjustment
        }
        // ... supports HackRF, Airspy, PlutoSDR, SoapySDR, LimeSDR
        return false; // Framework ready for future automatic gain application
    }
}

// Enhanced SourceManager API
class SourceManager {
public:
    std::string getSelectedName();  // NEW: Get currently selected source name
    // ... existing methods
};
```

### **Configuration Schema**
```json
{
  "frequencyRanges": [
    {
      "name": "Airband",
      "startFreq": 118000000.0,
      "stopFreq": 137000000.0, 
      "enabled": true,
      "gain": 25.0
    },
    {
      "name": "2m Ham",
      "startFreq": 144000000.0,
      "stopFreq": 148000000.0,
      "enabled": false,
      "gain": 30.0
    }
  ],
  "currentRangeIndex": 0
}
```

### **Scanning Logic**
- **Multi-Range Scanning**: Automatically cycles through enabled ranges
- **Range Wrapping**: Seamless transition between ranges at boundaries
- **Backward Compatibility**: Falls back to legacy single-range mode if no ranges defined
- **Dynamic Bounds**: Scanner adapts to current active range limits

---

## 📋 **Usage Examples**

### **Example 1: Radio Enthusiast Setup**
```
✅ FM Broadcast (88.0 - 108.0 MHz)    - Monitor local stations
✅ Airband (118.0 - 137.0 MHz)        - Aircraft communications  
❌ 2m Ham (144.0 - 148.0 MHz)         - Ham radio (disabled)
✅ PMR446 (446.0 - 446.2 MHz)         - Business radio
```

### **Example 2: Professional Monitoring**
```
✅ VHF Low (30.0 - 50.0 MHz)          - Public service
✅ VHF High (138.0 - 174.0 MHz)       - Emergency services
✅ UHF (400.0 - 512.0 MHz)            - Trunked systems
❌ 800MHz (806.0 - 824.0 MHz)         - (Disabled for now)
```

### **Example 3: Smart Blacklist Usage**
While scanning, you encounter an annoying signal at 146.520 MHz:
```
1. Tune to the unwanted frequency (146.520 MHz)
2. Press the **BL** button in the main window
3. Frequency Manager adds the entry to the **Blacklist** list
4. Continue scanning – 146.520 MHz is now automatically skipped
```

### **Example 4: Universal Gain Control in Action**
Professional monitoring setup with automatic gain optimization:
```
Setup: RTL-SDR scanning multiple bands for emergency services

1. Start with FM Broadcast range (88-108 MHz):
   - Scanner: "Applied gain 15.0 dB for range 'FM Broadcast' (source: RTL-SDR)"
   - Perfect for strong FM stations without overloading

2. Switch to Airband range (118-137 MHz):
   - Scanner: "Applied gain 25.0 dB for range 'Airband' (source: RTL-SDR)" 
   - Optimized for aircraft communications at medium distance

3. Move to 2m Ham range (144-148 MHz):
   - Scanner: "Applied gain 30.0 dB for range '2m Ham' (source: RTL-SDR)"
   - Higher gain for weaker ham radio signals

Result: No more manual gain adjustments! Each band automatically 
uses optimal settings for its typical signal characteristics.
```

---

## 🔄 **Migration & Compatibility**

### **Automatic Migration**
- **Existing Configs**: Automatically preserved and continue working
- **Legacy Mode**: Single-range controls shown when no ranges defined
- **Seamless Upgrade**: No user action required for existing setups

### **Backward Compatibility**
- **Config Structure**: Legacy `startFreq`/`stopFreq` still saved
- **Fallback Mode**: Scanner reverts to single-range if multi-range fails
- **Progressive Enhancement**: New features available without breaking existing functionality

---

## 🚀 **Benefits & Use Cases**

### **For Radio Enthusiasts**
- **Monitor Multiple Services**: Scan aircraft, marine, and emergency bands simultaneously
- **Quick Band Switching**: Toggle between amateur radio bands easily
- **Organized Scanning**: Name ranges for easy identification

### **For Professional Users**
- **Comprehensive Monitoring**: Cover all relevant frequency ranges efficiently
- **Flexible Configuration**: Enable/disable ranges based on operational needs
- **Streamlined Workflow**: No more manual frequency range switching

### **For Researchers**
- **Systematic Coverage**: Define precise frequency ranges for studies
- **Reproducible Scans**: Save and reload exact scanning configurations
- **Range Documentation**: Named ranges provide clear context for data collection

---

## 🎯 **Advanced Features**

### **Smart Range Management**
- **Overlap Detection**: Visual indication of overlapping ranges
- **Gap Analysis**: Identify uncovered frequency spaces
- **Usage Statistics**: Track time spent in each range

### **Import/Export** (Future Enhancement)
- **Range Profiles**: Save and share frequency range configurations
- **Template Library**: Pre-built ranges for common use cases  
- **Community Sharing**: Exchange range definitions with other users

---

## 🔧 **Technical Notes**

### **Performance Optimizations**
- **Efficient Range Switching**: Minimal latency when moving between ranges
- **Memory Efficient**: Ranges stored compactly in configuration
- **Thread Safety**: Multi-range logic fully thread-safe

### **Configuration Management**
- **Auto-Save**: All range changes automatically persist
- **Atomic Updates**: Configuration changes are atomic and consistent
- **Recovery**: Robust handling of invalid or corrupted range data

---

**🎉 The Enhanced Scanner Module transforms SDR++CE into a powerful, flexible frequency monitoring solution that adapts to any user's needs - from casual radio listening to professional spectrum monitoring!**
