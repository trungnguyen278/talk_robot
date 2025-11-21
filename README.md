# Talk Robot - README

## 📋 Project Overview

Talk Robot là một dự án robot AI tương tác với khả năng nhận diện cảm xúc, xử lý giọng nói, và hiển thị biểu cảm qua màn hình OLED.

## 🏗️ Architecture

```
ESP32 (Microcontroller)
├── Microphone Input (I2S) → ADPCM Encoding
├── WebSocket Client → Server Communication
├── OLED Display (320x240) → Emotion Animation
└── Speaker Output (I2S) → ADPCM Decoding

↔️ Backend Server
├── STT (Speech-to-Text)
├── LLM (Gemini API) + Emotion Analysis
└── TTS (Text-to-Speech)
```

## 🚀 Features

- **Real-time Audio Streaming**: Mic input → ADPCM compression → WebSocket transmission
- **Emotion Recognition**: Detects Happy, Sad, Neutral from user input
- **Animated Display**: Shows corresponding emotion GIF on OLED screen
- **Bi-directional Communication**: Receives TTS audio and commands from server

## 📦 Installation & Upload

### Prerequisites
- **Arduino IDE** with ESP32 board support
- Libraries:
  ```
  - WebSockets by Markus Sattler
  - TFT_eSPI
  - TJpg_Decoder
  - ArduinoJson
  ```

### Upload Steps

1. **Open Arduino IDE**
2. **Load sketch**: File → Open → main.ino
3. **Configure Board**:
   - Board: `ESP32 Dev Module`
   - Upload Speed: `921600`
   - Port: `COM[X]` (your ESP32 port)
4. **Upload**: Click Upload button or `Ctrl+U`

```
Sketch uses [X] bytes of program storage space
```

## ⚙️ Configuration

Edit main.ino constants:

```cpp

#define SERVER_URL        "ws://your.server.com:8000/ws"
#define SAMPLE_RATE       16000
#define I2S_MIC_PORT      I2S_NUM_0
#define I2S_SPEAKER_PORT  I2S_NUM_1
```

## 🎮 State Machine

```
STATE_OFFLINE_WIFI
    ↓
STATE_STREAMING (Mic recording)
    ↓
STATE_WAITING (Server processing)
    ↓
STATE_PLAYING_RESPONSE (Speaker output)
    ↓
[Back to STREAMING]
```

## 📡 WebSocket Protocol

### Client → Server (Text)
```
"PROCESSING_START"    // Resume listening
"LISTENING"           // Acknowledge state
```

### Client → Server (Binary)
ADPCM-encoded audio chunks

### Server → Client (Text)
```
"PROCESSING_START"    // Pause mic
"TTS_END"             // Flush speaker
"00" / "01" / "10"    // Emotion codes (Neutral/Happy/Sad)
```

### Server → Client (Binary)
ADPCM-encoded TTS response

## 🎨 Emotion System

| Code | Emotion | Animation |
|------|---------|-----------|
| `00` | Neutral | macdinh.h |
| `01` | Happy   | vuimung.h |
| `10` | Sad     | buon.h    |

Emotion GIFs stored in: emoji_h

## 🔊 Audio Pipeline

**Microphone**
```
I2S Input → 16-bit PCM → ADPCM Encode → WebSocket Send
```

**Speaker**
```
WebSocket Receive → ADPCM Decode → I2S Output
```

ADPCM State managed in:
- `adpcm_mic_state` (encoding)
- `adpcm_spk_state` (decoding)

## 📂 Key Files

| File | Purpose |
|------|---------|
| main.ino | ESP32 firmware |
| vad_server.py | WebSocket backend |
| pipeline.py | STT → LLM → TTS pipeline |
| emotion_manager.py | Emotion detection |
| emotion.h | Emotion definitions |

## 🐛 Troubleshooting

| Issue | Solution |
|-------|----------|
| WebSocket connection fails | Check SSID/password and server URL |
| No audio output | Verify I2S speaker pins in `setup_i2s_output()` |
| Display shows garbage | Ensure TFT_eSPI pins configured correctly |
| ADPCM buffer overflow | Reduce `AUDIO_CHUNK_SIZE` or increase buffer |

## 📝 Notes

- ADPCM compression ratio: **4:1** (reduces bandwidth)
- Animation frame delay: `ANIMATION_FRAME_DELAY_MS` (default 100ms)
- Max WebSocket message size: Check server config

---

**Happy coding! 🤖💬**