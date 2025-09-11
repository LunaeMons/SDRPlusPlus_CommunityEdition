# 📻 P25 Digital Demodulator Testing Instructions

## 🎯 **Quick Start Testing Guide**

### **Prerequisites**
- ✅ macOS app bundle: `SDR++_P25_Digital.app` (built and ready)
- ✅ Python 3.x installed
- ✅ P25 FSK4 WAV file for testing
- ✅ Test tool: `tools/digital_stream_receiver.py`

### **Step 1: Start the Python Digital Stream Receiver**

Open a terminal and run:
```bash
cd /Users/miguelgomes/PycharmProjects/SDRPlusPlus
python3 tools/digital_stream_receiver.py --host localhost --port 7356 --protocol p25
```

You should see:
```
🚀 SDR++ Digital Stream Receiver
   Listening: localhost:7356 (TCP)
   Output: console
   Expected Protocol: p25

TCP socket listening on localhost:7356
Waiting for SDR++ connection...
✅ Receiver started. Press Ctrl+C to stop.
📡 Waiting for digital stream from SDR++...
```

**Keep this terminal open** - it will receive and display the digital bit streams.

### **Step 2: Launch SDR++ with P25 Digital Demodulator**

```bash
# Option A: Run from app bundle (recommended)
open ./SDR++_P25_Digital.app

# Option B: Run with console output (for debugging)
./SDR++_P25_Digital.app/Contents/MacOS/sdrpp_ce
```

### **Step 3: Configure SDR++ for P25 Testing**

#### **A. Set up File Source**
1. In **Source** section, select **"File Source"**
2. Click **folder icon** and navigate to your P25 FSK4 WAV file
3. Click **"Open File"** 
4. Set **sample rate** to match your WAV file (typically 22050 Hz or 48000 Hz)
5. Click **▶️ Play** to start file playback

#### **B. Add P25 Digital Demodulator**
1. In **Module Manager** → **Decoder Modules** 
2. Look for **"P25 Digital Demod"** in the list
3. Click **"+"** next to **"P25 Digital Demod"**
4. Enter instance name: **"P25_Test"**
5. Click **"Create"**

**✅ Module Status:** The P25 Digital Demod should now appear in the left sidebar under "Decoder Modules"

#### **C. Configure P25 Demodulator**
1. Navigate to **P25_Test** in the left menu
2. **P25 Configuration:**
   - **Mode:** Select **"P25 FSK4"** (for most P25 signals)
   - **Deviation:** Adjust between 1500-2500 Hz (start with 1800 Hz)
   - **RRC Beta:** Keep at 0.2 (standard for P25)
   - **RRC Taps:** Keep at 31

3. **Digital Output:**
   - ☑️ **Enable "Network Output"**
   - **Host:** `localhost`
   - **Port:** `7355`
   - **Protocol:** TCP (default)
   
4. **Optional - File Recording:**
   - ☑️ **Enable "File Recording"** to save raw digital stream

#### **D. Tune to P25 Signal**
1. **Create VFO:** The P25 module should auto-create a VFO
2. **Center on Signal:** Drag the VFO to center on the P25 signal in the waterfall
3. **Adjust Bandwidth:** Set VFO bandwidth to ~9.6 kHz for P25

### **Step 4: Verify Digital Stream Reception**

In your Python receiver terminal, you should see:

```
📡 Digital Stream Header Received:
   Protocol: P25 FSK4, Symbol Rate: 4800 sym/s, Bits/Symbol: 2, Timestamp: 1737984567123456

Dibits: 00 01 10 11 00 01 10 11 01 10 00 11 01 00 10 11...
Dibits: 01 10 11 00 10 01 11 00 01 10 11 00 10 01 00 11...
Dibits: 10 11 00 01 11 00 10 01 00 11 01 10 00 01 11 10...

📊 Reception Statistics:
   Runtime: 15.3 seconds
   Bytes: 73440 (4800.0 B/s)
   Symbols: 73440 (4800.0 sym/s)
   Packets: 1224
   Expected Symbol Rate: 4800 sym/s
   Efficiency: 100.0%
```

### **Step 5: Signal Quality Optimization**

#### **A. Monitor Signal Quality in SDR++**
- **Status:** Should show "Receiving" (green)
- **SNR:** Aim for >10 dB for reliable decoding
- **Signal Quality Bar:** Should be green/yellow (>30%)

#### **B. Fine-tune Parameters**
1. **If no signal detected:**
   - Adjust VFO frequency to center on signal
   - Increase deviation (try 2000-2500 Hz)
   - Check file source is playing

2. **If poor signal quality:**
   - Adjust **Threshold 1** and **Threshold 2** sliders
   - Try different **RRC Beta** values (0.15-0.35)
   - Verify signal is actually P25 FSK4

3. **If symbol rate mismatch:**
   - P25 CAI uses 4800 sym/s
   - Some systems may use different rates
   - Check with spectrum analyzer

---

## 🔧 **Advanced Testing Options**

### **UDP Mode Testing**
```bash
# Start receiver in UDP mode
python3 tools/digital_stream_receiver.py --udp --port 7356 --protocol p25

# In SDR++ P25 module:
# - Change Port to 7356
# - Enable UDP checkbox
```

### **File Recording Testing**
```bash
# Enable file recording in SDR++ P25 module
# Files saved to: ~/Library/Application Support/sdrpp/recordings/
# Format: P25_FSK4_YYYYMMDD_HHMMSS.digi

# View recorded file header:
python3 -c "
import struct
with open('path/to/recording.digi', 'rb') as f:
    header = struct.unpack('<IIHHHB7xQ64s', f.read(96))
    print(f'Magic: {hex(header[0])}')
    print(f'Protocol: {header[2]}')  
    print(f'Symbol Rate: {header[3]}')
    print(f'Bits/Symbol: {header[4]}')
    print(f'Description: {header[8].decode().strip()}')
"
```

### **Multi-Protocol Testing**
The P25 module supports multiple modes:
- **P25 FSK4** - Most common P25 signals
- **P25 CQPSK 4800** - Enhanced digital mode
- **P25 CQPSK 6000** - Higher data rate mode  
- **P25 H-DQPSK** - Harmonized DQPSK
- **P25 H-CPM** - Harmonized CPM

Switch between modes in the **Mode** dropdown and observe changes in symbol rate and parameters.

---

## 🐛 **Troubleshooting**

### **No Digital Stream Received**
1. **Check Python receiver** - Should show "TCP socket listening"
2. **Verify P25 module** - Status should be "Receiving" 
3. **Check network output** - Must be enabled with correct host/port
4. **Verify VFO positioning** - Must be centered on P25 signal

### **Poor Symbol Quality**
1. **Signal too weak** - Increase input gain or use stronger signal
2. **Wrong deviation** - Try values between 1500-2500 Hz
3. **Frequency offset** - Ensure VFO is precisely centered
4. **Sample rate mismatch** - Verify WAV file sample rate settings

### **Connection Issues**
1. **Port already in use** - Try different port (7356, 7357, etc.)
2. **Firewall blocking** - Check macOS firewall settings
3. **Permission denied** - Run Python receiver as regular user

### **Module Not Found**
1. **Check module loading** - Look for errors in SDR++ console
2. **Verify bundle** - Ensure `p25_digital_demod.dylib` exists in app
3. **Dependencies** - All required libraries should be bundled

---

## 📊 **Expected Results**

### **Successful P25 FSK4 Decoding:**
- **Symbol Rate:** 4800 symbols/second
- **Bits per Symbol:** 2 (FSK4)
- **Data Rate:** 9600 bits/second
- **Signal Quality:** >30% for reliable operation
- **Stream Efficiency:** 95-100%

### **Digital Stream Format:**
- **Header:** 16-byte protocol identification
- **Data:** Raw dibits (0, 1, 2, 3) as uint8_t values
- **Timing:** Real-time streaming at symbol rate
- **Network:** TCP by default, UDP optional

---

## 🚀 **Next Steps After Successful Testing**

1. **Validate with Multiple P25 Files** - Test different signal strengths and modes
2. **External Decoder Integration** - Connect to OP25, DSD-FME, or GNU Radio
3. **Performance Analysis** - Monitor CPU usage and latency
4. **Protocol Expansion** - Test other modes (CQPSK, H-DQPSK)

---

## 📞 **Support**

If you encounter issues:
1. **Check console output** - Run app bundle from terminal for debug info
2. **Verify dependencies** - Ensure all Homebrew packages are installed
3. **Test with known good P25 signals** - Use reference recordings
4. **Report findings** - Include symbol rate, deviation, and signal quality metrics

**Ready for testing!** 🎉
