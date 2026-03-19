# Xiaozhi-ESP32 Architecture

## Overview

Xiaozhi-ESP32 is a voice-interactive AI chatbot for ESP32 microcontrollers. It supports 95+ hardware boards, online LLM interaction via WebSocket/MQTT, and offline fallback with on-device wake word detection and command recognition (ESP-SR). Audio is encoded with Opus at 16kHz mono.

---

## Directory Structure

```
xiaozhi-esp32/
├── main/
│   ├── main.cc                  # Entry point
│   ├── application.h/.cc        # Singleton app controller & event loop
│   ├── device_state.h           # DeviceState enum
│   ├── device_state_machine.h   # State transition validation
│   ├── ota.h/.cc                # Over-the-air updates
│   ├── assets.h/.cc             # Asset downloading (fonts, models)
│   ├── mcp_server.h/.cc         # Model Context Protocol tool server
│   ├── audio/                   # Audio pipeline
│   │   ├── audio_service.h/.cc  # Multi-task audio manager
│   │   ├── audio_codec.h        # Abstract I2S codec interface
│   │   ├── audio_processor.h    # AFE (echo cancel, noise suppression)
│   │   └── wake_word/           # Wake word engines
│   ├── boards/                  # Hardware abstraction (95+ boards)
│   │   ├── common/
│   │   │   ├── board.h/.cc      # Abstract board interface
│   │   │   ├── wifi_board.h/.cc # WiFi networking base
│   │   │   └── button.h, backlight.h
│   │   ├── echoear/             # EchoEar board
│   │   └── [other boards]/
│   ├── display/                 # Display implementations
│   │   ├── display.h            # Abstract display interface
│   │   ├── emote_display.h      # Animation expressions
│   │   ├── lcd_display.h        # LVGL-based LCD
│   │   └── oled_display.h       # OLED 128x64
│   ├── protocols/               # Network protocols
│   │   ├── protocol.h           # Abstract protocol interface
│   │   ├── websocket_protocol.h # WebSocket (JSON + binary)
│   │   └── mqtt_protocol.h      # MQTT + AES + UDP
│   ├── led/                     # LED control
│   ├── offline/                 # Offline mode (SD card playback)
│   └── iot/                     # IoT device control
├── components/
│   └── esp-skainet-master/      # ESP-SR library (wake word, commands)
└── sdcard/                      # Offline audio files
```

---

## Core Layers

### 1. Application Core (`main/application.h`)

Singleton event-driven controller that manages the device lifecycle.

**State Machine** (`DeviceState`):
```
Starting → WifiConfiguring → Idle → Connecting → Listening → Speaking
                                   → AudioTesting
                                   → Upgrading → Idle
                                   → Activating → Idle
Any state → FatalError
```

**Event System** (FreeRTOS EventGroup):
- `WAKE_WORD_DETECTED` — triggers listening mode
- `SEND_AUDIO` — audio packet ready to send
- `VAD_CHANGE` — voice activity start/stop
- `NETWORK_CONNECTED/DISCONNECTED` — network state changes
- `TOGGLE_CHAT` — manual chat trigger
- `STATE_CHANGED` — device state transition
- `CLOCK_TICK` — periodic timer

**Initialization** (`main.cc`):
1. Initialize NVS flash
2. `app.Initialize()` — board, display, audio service, SD card
3. `app.Run()` — main event loop (never returns)

---

### 2. Board Abstraction (`main/boards/`)

Factory pattern providing pluggable hardware support.

```
Board (abstract)
├── WifiBoard           # WiFi networking
│   ├── EchoEar
│   ├── M5Stack CoreS3
│   └── [70+ boards]
├── DualNetworkBoard    # WiFi + Cellular
└── RndisBoard          # USB RNDIS
```

**Board Interface**:
- `GetAudioCodec()` — I2S audio in/out
- `GetDisplay()` — display abstraction
- `GetNetwork()` — network interface
- `GetBoardJson()` / `GetDeviceStatusJson()` — device info
- `GetBatteryLevel()`, `GetTemperature()`

Each board lives in `main/boards/[name]/` with a `config.h` (GPIO pins, rates, dimensions) and implementation file.

---

### 3. Audio Pipeline (`main/audio/`)

Multi-task real-time audio system with three FreeRTOS tasks:

```
Microphone (24kHz I2S)
    ↓
┌─ AudioInputTask ─────────────────────┐
│  AudioCodec::InputData()             │
│  → AudioProcessor (AFE)             │
│    ├─ Device AEC                     │
│    ├─ Noise Suppression              │
│    └─ Beamforming (dual-mic)         │
│  → WakeWord Detection                │
│  → Encode Queue (PCM)                │
└──────────────────────────────────────┘
    ↓
┌─ OpusCodecTask ──────────────────────┐
│  Opus Encoder (60ms frames)          │
│  → Send Queue → Protocol::SendAudio  │
│                                      │
│  Decode Queue (from network)         │
│  → Opus Decoder                      │
│  → Playback Queue (PCM)             │
└──────────────────────────────────────┘
    ↓
┌─ AudioOutputTask ────────────────────┐
│  Resampler (→ output rate)           │
│  AudioCodec::OutputData() (I2S TX)   │
│  → Speaker                           │
└──────────────────────────────────────┘
```

**Key Classes**:
- `AudioService` — manages tasks, queues, codec lifecycle
- `AudioCodec` (abstract) — I2S interface, volume/gain control
- `AudioProcessor` (abstract) — AFE voice processing
- `WakeWord` (abstract) — wake word engines (ESP-SR WakeNet9, custom)

**Audio Config**: 16kHz internal, Opus 60ms frames, DTX + VBR enabled, queues of 40 packets.

---

### 4. Display System (`main/display/`)

Strategy pattern with multiple implementations:

```
Display (abstract)
├── EmoteDisplay    # Animation-based expressions (320x240, 360x360, 1024x600)
├── LcdDisplay      # LVGL-based rich graphics
├── OledDisplay     # OLED 128x64
└── NoDisplay       # No-op
```

**Key Methods**: `SetStatus()`, `SetEmotion()`, `SetChatMessage()`, `ShowNotification()`, `UpdateStatusBar()`

Thread-safe via `DisplayLockGuard` (30s timeout).

---

### 5. Network Protocols (`main/protocols/`)

```
Protocol (abstract)
├── WebsocketProtocol   # JSON + binary frames, auto-reconnect
└── MqttProtocol        # MQTT pub/sub + AES encryption + UDP audio
```

**Binary Formats**:
- `BinaryProtocol2`: version, type (OPUS/JSON), timestamp, payload
- `BinaryProtocol3`: compact — type, reserved, payload_size, payload

**Protocol Callbacks** → Application:
- `OnIncomingAudio`, `OnIncomingJson`
- `OnAudioChannelOpened/Closed`
- `OnConnected/Disconnected`, `OnNetworkError`

---

### 6. MCP Server (`main/mcp_server.h`)

Tool registration system for Model Context Protocol:
- Properties: Boolean, Integer (with ranges), String
- Tools with JSON schema validation
- Return types: bool, int, string, JSON, images
- User-only tools (hidden from AI, visible to human)

---

### 7. Offline Mode (`main/offline/`)

- **SD Card Manager** (singleton) — mount/unmount, file I/O, directory listing
- **Simple Audio Player** — plays OGG files from SD card
- **ESP-SR** — on-device wake word (WakeNet9) + command recognition (MultiNet5/6/7)

---

## Data Flow: Voice Interaction

```
[User speaks]
  → Microphone → AudioInputTask → AFE processing → Wake word detected
  → Application sets state to Listening
  → Protocol::OpenAudioChannel()
  → AudioInputTask → Opus encode → Protocol::SendAudio()
  → Server: ASR → LLM → TTS
  → Protocol callback: OnIncomingAudio
  → Opus decode → AudioOutputTask → Speaker
  → Application sets state to Speaking → Idle
```

---

## Threading Model

| Task | Responsibility |
|------|---------------|
| **Main Task** (`app.Run()`) | Event loop, state transitions, protocol I/O |
| **AudioInputTask** | Microphone sampling, AFE, wake word detection |
| **AudioOutputTask** | Speaker playback, resampling |
| **OpusCodecTask** | Opus encoding/decoding |
| **WiFi Task** (system) | Network connection management |
| **HTTP Task** (OTA) | Firmware/asset downloads |
| **Protocol Tasks** | WebSocket/MQTT network I/O |

**Synchronization**: FreeRTOS EventGroups, mutexes (display, decoder, protocol), condition variables for queues, atomic flags.

---

## Configuration (`main/Kconfig.projbuild`)

| Category | Options |
|----------|---------|
| Board Type | 95+ boards (`BOARD_TYPE_*`) |
| Language | 23+ languages |
| Display Assets | LVGL or Emote style |
| Audio Processing | Device AEC vs. Server AEC (mutually exclusive) |
| Protocol | WebSocket or MQTT |
| OTA URL | Firmware update server |

---

## EchoEar Board (`main/boards/echoear/`)

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 |
| Display | 360x360 LCD (ST77916) via QSPI |
| Audio | ES7210 (4-ch ADC) + ES8311 (DAC) |
| Microphones | 2 mics on GPIO 15, 3 |
| SD Card | 1-line SDMMC (GPIO 38/16/17) |
| Touch | CST816S capacitive (I2C GPIO 2/1) |
| IMU | BMI270 (gesture control, GPIO 21 interrupt) |
| Battery | ADC monitoring |
| Backlight | GPIO 44 |

---

## Online vs. Offline

| Feature | Online | Offline |
|---------|--------|---------|
| Wake word detection | Yes (on-device) | Yes (on-device) |
| Command recognition | Server ASR | ESP-SR MultiNet |
| LLM responses | Server LLM | Pre-recorded audio (SD card) |
| Display updates | Full (chat, status) | Minimal (emoji events) |
| OTA / asset updates | Yes | No |
| MCP tools | Yes | No |
