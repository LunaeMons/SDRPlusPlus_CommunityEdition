# Scanner Enhancement Implementation - Technical Documentation

## Feature Implementation Complete: Issues #8, #9, #10

The scanner module has been redesigned from a basic frequency range scanner to a comprehensive frequency manager-integrated system. This implementation provides professional-grade scanning capabilities with automated radio configuration.

---

## System Overview

1. **Open Frequency Manager** module in the sidebar
2. **Add frequencies or bands** to your frequency lists
3. **Enable scanning** by checking the **[S]** checkbox next to entries you want to scan
4. **Open Scanner** module - it will automatically use your frequency manager entries
5. **Click Start** to begin scanning!

---

## 📊 Frequency Manager: New Band Support (Issue #10)

### Creating Single Frequencies
- **Frequency**: Target frequency (e.g., 121.500 MHz for air traffic control)
- **Mode**: Demodulation mode (NFM, WFM, AM, etc.)
- **Bandwidth**: Channel width
- **Scannable [S]**: Check to include in scanner

### Creating Frequency Bands (NEW!)
1. **Enable "Is Band"** checkbox
2. **Start Frequency**: Beginning of band (e.g., 118.000 MHz for airband)
3. **End Frequency**: End of band (e.g., 137.000 MHz)
4. **Step Frequency**: How often to check for signals (e.g., 25 kHz)
5. **Scannable [S]**: Check to include in scanner

**How Bands Affect Scanner:**
- Scanner steps through the band using your **Step Frequency**
- Uses **full VFO bandwidth** for signal detection (catches wide signals)
- Perfect for discovering new activity in a frequency range
- Example: Airband (118-137 MHz, 25 kHz steps) finds all air traffic

---

## ⚙️ Per-Entry Tuning Profiles (Issue #9)

Each frequency manager entry can have its own **Tuning Profile** that automatically configures your radio:

### Profile Settings:
- **Demod Mode**: NFM, WFM, AM, USB, LSB, etc.
- **Bandwidth**: Channel width in Hz (e.g., 12500 for NFM)
- **Squelch Enabled**: Automatic noise suppression
- **Squelch Level**: Threshold in dB (-50 dB typical)
- **RF Gain**: Receiver sensitivity (20 dB typical)
- **Profile Name**: Custom name (auto-generated if empty)

### How Profiles Work:
- Scanner **automatically applies** the profile when stopping on a frequency
- **No manual adjustment needed** - radio configures itself!
- Example: 121.500 MHz → AM 5kHz, 446.100 MHz → NFM 12.5kHz
- Saves time and ensures optimal settings for each frequency

---

## 🔍 Enhanced Scanner Controls

### Core Controls:
- **Start/Stop**: Begin/end scanning
- **<< / >>**: Scan direction (decreasing/increasing frequency)
- **Trigger Level**: Signal strength threshold for stopping (-120 to 0 dBFS)
  - Lower = more sensitive, Higher = less sensitive

### Timing Controls:
- **Interval**: Step size for band scanning
  - 5-25 kHz: Precise coverage (slower)
  - 50-200 kHz: Fast discovery (faster)
  - Only affects bands, not single frequencies

- **Tuning Time**: Hardware settling delay (100-10000 ms)
  - Increase if missing signals (slow hardware)
  - Decrease for faster scanning (stable hardware)
  - Default: 250 ms

- **Linger Time**: How long to stay on active signals (100-10000 ms)
  - 500-1000 ms: Quick signal identification
  - 2000+ ms: Voice communications
  - Default: 1000 ms

### Signal Detection Controls:
- **Scan Rate**: How fast to check frequencies (5-50/sec)
  - Start at 25/sec, increase if hardware supports
  - Higher rates may miss weak signals

- **Passband Ratio**: Signal detection bandwidth (10-100%)
  - Start at 100% for best detection
  - Lower if catching too many false positives

---

## 🚫 Blacklist Management

Blacklist entries are now handled by the **Frequency Manager**:

- Open the Frequency Manager and select the **Blacklist** list to add or edit frequencies and bands.
- The scanner automatically skips anything contained in this list.
- Use the **BL** button on the main window to quickly add the currently tuned frequency to the blacklist.

---

## 💡 Pro Tips

### Optimal Settings:
1. **Start with defaults**: All controls have optimized starting values
2. **Single frequencies**: Use for known active channels
3. **Bands**: Use for discovery and monitoring ranges
4. **Profiles**: Create once, automatically applied forever
5. **Direction**: Use >> for normal scanning, << to reverse

### Example Setups:

**Air Traffic Control:**
- Single Frequency: 121.500 MHz
- Profile: AM, 5 kHz bandwidth, squelch enabled
- Scanner automatically switches to AM when signal found

**Ham Radio Band:**
- Band: 144.000 - 148.000 MHz, 25 kHz steps
- Profile: NFM, 12.5 kHz bandwidth
- Scanner searches entire band, stops on activity

**Emergency Services:**
- Mixed single frequencies and bands
- Custom profiles for each service type
- Blacklist inactive or problematic frequencies

---

## 🔧 Troubleshooting

### Scanner Won't Start:
- **Check**: SDR source is running (click "Start" in main interface)
- **Check**: At least one frequency manager entry has [S] enabled

### Missing Signals:
- **Increase**: Trigger Level sensitivity (lower number)
- **Increase**: Tuning Time (give hardware more settling time)
- **Check**: Profile settings match actual signal type

### Too Many False Positives:
- **Decrease**: Passband Ratio
- **Increase**: Trigger Level threshold
- **Use**: Single frequencies instead of wide bands

### Scanner Too Slow:
- **Increase**: Scan Rate
- **Increase**: Interval (for bands)
- **Decrease**: Tuning Time

---

## 🎯 What's Different from Before

### Removed (Simplified):
- ❌ Manual frequency range entry
- ❌ "Use Frequency Manager" toggle (always enabled)
- ❌ "Apply Profiles" toggle (always enabled)
- ❌ Complex dual-mode interface

### Added (Enhanced):
- ✅ **Automatic frequency manager integration**
- ✅ **Band scanning with custom step sizes**
- ✅ **Automatic tuning profile application**
- ✅ **Enhanced blacklist with named entries**
- ✅ **Professional tooltips and guidance**
- ✅ **Optimized defaults for immediate use**

---

## 🏆 Enjoy Your New Scanner!

The scanner now provides a **professional-grade** scanning experience with **zero manual configuration** once you've set up your frequency manager entries and profiles. 

Each frequency can have its own optimal radio settings that are applied automatically - no more manual adjustments needed during scanning!

**Questions?** All controls have detailed tooltips - just hover over any setting for specific guidance.

Happy scanning! 📻✨
