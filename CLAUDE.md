# Claude Instructions for Xiaozhi-ESP32

## ⚠️ CRITICAL: DO NOT BUILD AUTOMATICALLY

**NEVER** run build commands without explicit user permission:
- ❌ `idf.py build`
- ❌ `python ./scripts/release.py`
- ❌ `idf.py flash`
- ❌ Any background builds

**User builds manually!** Only provide build instructions when asked.

## Project Context

### Hardware: EchoEar Board
- **MCU**: ESP32-S3
- **Audio Codec**: ES7210 (4-channel ADC) + ES8311 (DAC)
- **Microphones**: 2 mics (MIC1, MIC2)
- **Features**:
  - Offline voice commands (ESP-SR)
  - BlueFi WiFi provisioning
  - SD card storage
  - Device AEC (Acoustic Echo Cancellation)

### Key Directories
- `main/boards/echoear/` - EchoEar board configuration
- `main/audio/` - Audio service, codecs, wake words
- `components/esp-skainet-master/` - ESP-SR library (reference)
- `sdcard/` - Offline audio files (stories, jokes, etc.)

### Build Commands (When Requested)
```bash
# Setup environment
source /Users/sohilpatel/esp/v5.5/esp-idf/export.sh

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/cu.usbmodem101 flash monitor
```

### Current Configuration
- Language: English (CONFIG_LANGUAGE_EN_US)
- WiFi Provisioning: BlueFi (Bluetooth)
- Audio: Device AEC enabled
- Display: Emote expression style

## Development Guidelines

### When Making Changes
1. **Read code first** before editing
2. **Analyze architecture** before implementing features
3. **Check skainet examples** for ESP-SR reference implementations
4. **Test changes** are user's responsibility
5. **Never assume** - ask for clarification

### ESP-SR (Skainet) Integration
- Reference: `components/esp-skainet-master/examples/en_speech_commands_recognition/`
- Models: WakeNet9 (wake word), MultiNet5/6/7 (commands)
- AFE: Acoustic Front End with 2-mic BSS (Blind Source Separation)

### Forbidden Actions
- ⛔ Auto-building without permission
- ⛔ Running flash commands automatically
- ⛔ Making assumptions about hardware config
- ⛔ Modifying skainet component files
