# Scanner Trigger Level Visualization Feature

## Overview
This feature adds a visual indicator for the scanner trigger level on the FFT display, making it easier for users to understand and adjust their scanning threshold.

## Implementation Details

### 1. Core Components Added

#### FFT Redraw Handler
- Added `EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler` to scanner module
- Registered with `gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler)` in constructor
- Properly unbound in destructor for clean shutdown

#### Trigger Level Line Drawing
- Horizontal orange line drawn across the entire FFT display width
- Line position calculated based on trigger level value in dBFS
- Automatic scaling with FFT display range (fftMin to fftMax)
- Line thickness scales with UI scale factor for consistency

#### Text Label
- Shows trigger level value (e.g., "-50.0 dBFS") next to the line
- Positioned at right edge of FFT display
- Semi-transparent black background for better visibility
- Orange text color matching the line

### 2. Configuration Integration

#### New Configuration Option
- Added `showTriggerLevel` boolean setting (default: true)
- Persisted in scanner configuration file
- Loaded/saved with other scanner settings

#### User Interface Control
- Added "Show Trigger Level" checkbox in scanner menu
- Tooltip explains the feature purpose and benefits
- Immediate save when toggled

### 3. Visual Design

#### Color Scheme
- **Line Color**: Orange (`IM_COL32(255, 165, 0, 200)`) with transparency
- **Text Color**: Solid orange (`IM_COL32(255, 165, 0, 255)`)
- **Background**: Semi-transparent black (`IM_COL32(0, 0, 0, 128)`)

#### Positioning Logic
```cpp
// Calculate Y position (FFT display is inverted, max at top)
float scaleFactor = (args.max.y - args.min.y) / vertRange;
float yPos = args.max.y - ((clampedLevel - fftMin) * scaleFactor);
```

#### Responsive Behavior
- Line position updates in real-time as trigger level changes
- Automatically clamps to visible FFT range
- Scales properly with different FFT zoom levels
- Works with all FFT display modes

### 4. Code Changes Summary

#### Files Modified
- `misc_modules/scanner/src/main.cpp` - Main implementation

#### Key Functions Added
- `static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx)` - Main drawing function

#### Configuration Keys Added
- `showTriggerLevel` (boolean, default: true)

#### UI Elements Added
- "Show Trigger Level" checkbox with tooltip

### 5. Benefits for Users

#### Improved Usability
- Visual feedback for trigger level setting
- Easier to understand relationship between signals and threshold
- Helps optimize scanning sensitivity

#### Better Signal Analysis
- Clear indication of detection threshold
- Assists in avoiding false triggers
- Helps identify optimal trigger levels for different bands

#### Enhanced Workflow
- No need to guess trigger level effectiveness
- Visual confirmation of scanner configuration
- Immediate feedback when adjusting settings

### 6. Technical Implementation Notes

#### Performance Considerations
- Drawing only occurs when `showTriggerLevel` is enabled
- Minimal computational overhead (simple line drawing)
- Uses existing ImGui drawing infrastructure
- No impact on DSP performance

#### Thread Safety
- FFT redraw handler runs on UI thread
- No interaction with DSP threads
- Safe access to configuration variables

#### Memory Management
- No dynamic memory allocation in drawing code
- Uses stack-allocated variables for calculations
- Proper cleanup in destructor

### 7. Testing and Validation

#### Functionality Tests
- ✅ Line appears when feature is enabled
- ✅ Line disappears when feature is disabled
- ✅ Line position updates with trigger level changes
- ✅ Text label shows correct dBFS value
- ✅ Configuration persists across app restarts

#### Visual Tests
- ✅ Line is clearly visible against FFT background
- ✅ Text is readable with background contrast
- ✅ Colors are consistent with design
- ✅ Scaling works properly at different UI scales

#### Integration Tests
- ✅ No interference with existing scanner functionality
- ✅ Compatible with all scanner modes
- ✅ Works with frequency manager integration
- ✅ No impact on scanning performance

### 8. Usage Instructions

#### Enabling the Feature
1. Open SDR++ and navigate to the Scanner module
2. Locate the "Show Trigger Level" checkbox
3. Check the box to enable the visualization (enabled by default)

#### Using the Feature
1. Adjust the "Trigger Level" slider in the scanner
2. Observe the orange horizontal line on the FFT display
3. The line shows exactly where your trigger threshold is set
4. Signals above the line will trigger the scanner
5. Use this visual guide to optimize your trigger level

#### Tips for Best Results
- Set trigger level just above the noise floor
- Use the visual line to avoid setting it too high (missing weak signals)
- Adjust based on band conditions and desired sensitivity
- The line helps identify when signals are strong enough to trigger

### 9. Future Enhancements (Potential)

#### Additional Visual Elements
- Color-coded zones (noise floor, signal area, etc.)
- Multiple threshold lines for different scanning modes
- Signal strength indicators relative to trigger level

#### Advanced Features
- Automatic trigger level adjustment based on noise floor
- Historical trigger level effectiveness display
- Band-specific trigger level recommendations

### 10. Conclusion

This feature significantly improves the user experience of the scanner module by providing clear visual feedback about the trigger level setting. It makes the scanner more intuitive to use and helps users optimize their scanning configuration for better results.

The implementation follows SDR++ architectural patterns, maintains performance, and integrates seamlessly with existing functionality while providing substantial value to users.
