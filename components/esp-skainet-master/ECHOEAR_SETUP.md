# Running en_speech_commands_recognition on EchoEar Board

This guide explains how to run the English speech commands recognition example on your EchoEar board.

## What Was Done

1. **Created EchoEar Board Support**: Added board-specific configuration for EchoEar in the esp-skainet hardware_driver component:
   - `components/hardware_driver/boards/include/echoear_board.h` - GPIO pin definitions
   - `components/hardware_driver/boards/echoear/bsp_board.c` - Board support functions
   - Updated `Kconfig.projbuild` to add EchoEar board option
   - Updated `CMakeLists.txt` to include EchoEar board files

2. **Hardware Configuration**:
   - I2C: SCL=GPIO1, SDA=GPIO2 (for ES8311 DAC and ES7210 ADC)
   - I2S: MCLK=GPIO42, BCLK=GPIO40, WS=GPIO39, DIN=GPIO15, DOUT=GPIO41
   - SD Card: CMD=GPIO38, CLK=GPIO16, D0=GPIO17 (1-line mode)
   - Power Control: GPIO48

## How to Build and Flash

### Step 1: Navigate to the Example Directory
```bash
cd /Users/sohilpatel/Documents/GitHub/xiaozhi-esp32/components/esp-skainet-master/examples/en_speech_commands_recognition
```

### Step 2: Set Target
```bash
idf.py set-target esp32s3
```

### Step 3: Configure the Project
```bash
idf.py menuconfig
```

In menuconfig, navigate to:
- **Audio Media HAL → Audio hardware board** → Select **"EchoEar"**
- **ESP Speech Recognition → Select wake words** → Choose your preferred wake word (e.g., "Hi, ESP" - wn9_hiesp)
- **ESP Speech Recognition → MultiNet** → Select English MultiNet model (should already be selected)

Save and exit (Press 'S' then 'Q')

### Step 4: Build the Project
```bash
idf.py build
```

### Step 5: Flash to EchoEar Board
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```
(Replace `/dev/ttyUSB0` with your actual serial port)

## How It Works

1. **Wake Word Detection**: Say "Hi, ESP" (or your chosen wake word)
2. **Command Recognition**: After wake word is detected, speak one of these commands:
   - "Turn on the light"
   - "Turn off the light"
   - "Turn on the air conditioner"
   - "Turn off the air conditioner"
   - "Increase volume"
   - "Decrease the volume"
   - "Make me a coffee"
   - And many more...

3. **Audio Feedback**: The board will play acknowledgment sounds through the speaker

## Supported Commands

The example supports various English commands (up to 200):
- Lighting control (on/off, colors)
- Temperature control (16-26 degrees)
- Volume control
- Appliance control (TV, soundbox, etc.)

You can add custom commands using the MultiNet API. See the example README for details.

## Customizing Commands

To add/modify commands programmatically, edit `main/main.c`:

```c
esp_mn_commands_clear();                       // Clear existing commands
esp_mn_commands_add(1, "your custom command"); // Add new command
esp_mn_commands_update();                      // Update command list
multinet->print_active_speech_commands(model_data); // Print active commands
```

## Troubleshooting

### Audio Not Working
- Verify I2C connection (ES8311 and ES7210 should be detected on I2C address)
- Check power control GPIO48 is properly configured
- Verify I2S pins are correctly connected

### Wake Word Not Detecting
- Check microphone input (GPIO15)
- Adjust wake word threshold in menuconfig
- Ensure 4-mic array is properly connected to ES7210

### Build Errors
- Make sure you selected "EchoEar" board in menuconfig
- Verify ESP-IDF version is 5.0 or later
- Check that all required components are present

## Board Comparison

EchoEar has similar audio hardware to ESP32-S3-Korvo-2:
- ES8311 audio DAC for speaker output
- ES7210 quad-mic ADC for microphone input
- SD card support via SDMMC interface
- Compatible sample rate: 16kHz
- Channel format: Stereo (2 channels)
- Bits per sample: 32-bit

## Next Steps

1. Test wake word detection
2. Test various speech commands
3. Customize commands for your application
4. Integrate with your xiaozhi-esp32 project if needed

## Notes

- The board initialization uses the same codec configuration as Korvo-2
- Input format is "RMNM" (Reference, Mic, Noise, Mic pattern)
- 4-channel ADC with ES7210 (4 mics)
- Stereo output via ES8311
