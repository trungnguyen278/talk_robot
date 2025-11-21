# Talk Robot - README

## 📋 Project Overview

Talk Robot là một dự án robot AI tương tác với khả năng nhận diện cảm xúc, xử lý giọng nói và hiển thị biểu cảm trên màn hình TFT SPI 1.54" (240×240). Microphone dùng INMP441 (I2S), loa 4Ω 3W, nguồn từ pin LiPo (>=1600 mAh).

## 🏗️ Architecture

```
ESP32 (Microcontroller)
├── INMP441 Microphone (I2S) → ADPCM Encoding
├── WebSocket Client → Server Communication
├── 1.54" TFT SPI (240x240) → Emotion Animation
└── Speaker Output (I2S → Class-D Amp) → ADPCM Decoding

↔️ Backend Server
├── STT (Speech-to-Text)
├── LLM (Gemini API) + Emotion Analysis
└── TTS (Text-to-Speech)
```

## 🚀 Features

- Real-time audio streaming: INMP441 → ADPCM → WebSocket
- Emotion recognition: Happy / Sad / Neutral
- Animated display: GIF/frames trên TFT SPI 240×240
- Bi-directional audio: Nhận TTS dạng ADPCM từ server, phát qua ampli

## 📦 Installation & Upload

### Prerequisites
- Arduino IDE với hỗ trợ ESP32
- Thư viện:
    ```
    - WebSockets by Markus Sattler
    - TFT_eSPI (cấu hình cho TFT 1.54" SPI 240x240)
    - TJpg_Decoder (nếu dùng JPG)
    - ArduinoJson
    ```

### Upload Steps
1. Open Arduino IDE  
2. Load sketch: File → Open → main.ino  
3. Configure Board: ESP32 Dev Module, Upload Speed 921600, chọn COM port  
4. Upload (Ctrl+U)

## ⚙️ Configuration

Chỉnh các hằng số trong main.ino:

```cpp
#define SERVER_URL        "ws://your.server.com:8000/ws"
#define SAMPLE_RATE       16000
#define I2S_MIC_PORT      I2S_NUM_0   // INMP441
#define I2S_SPEAKER_PORT  I2S_NUM_1
// TFT SPI pins cấu hình trong User_Setup.h của TFT_eSPI
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

Client → Server (Text)
```
"PROCESSING_START"    // Resume listening
"LISTENING"           // Acknowledge state
```

Client → Server (Binary)
ADPCM-encoded audio chunks

Server → Client (Text)
```
"PROCESSING_START"    // Pause mic
"TTS_END"             // Flush speaker
"00" / "01" / "10"    // Emotion codes (Neutral/Happy/Sad)
```

Server → Client (Binary)
ADPCM-encoded TTS response

## 🎨 Emotion System

| Code | Emotion | Animation |
|------|---------|-----------|
| `00` | Neutral | macdinh.h |
| `01` | Happy   | vuimung.h |
| `10` | Sad     | buon.h    |

Emotion frames/GIF lưu trong: .\PCB\emoji_h hoặc /assets/emoji_h (tuỳ repo)

## 🔊 Audio Pipeline

Microphone (INMP441)
```
I2S Input (INMP441) → 16-bit PCM → ADPCM Encode → WebSocket Send
```

Speaker
```
WebSocket Receive → ADPCM Decode → I2S Output → Class-D Amplifier → 4Ω speaker
```

ADPCM State:
- adpcm_mic_state (encoding)
- adpcm_spk_state (decoding)

## 📂 Key Files

| File | Purpose |
|------|---------|
| main.ino | ESP32 firmware |
| vad_server.py | WebSocket backend |
| pipeline.py | STT → LLM → TTS pipeline |
| emotion_manager.py | Emotion detection |
| emotion.h | Emotion definitions |
| .\PCB\* | PCB source, gerbers, BOM, assembly files |

## 🐛 Troubleshooting

| Issue | Solution |
|-------|----------|
| WebSocket connection fails | Kiểm tra SSID/pass và SERVER_URL |
| No audio output | Kiểm tra chân I2S và amp, volume, driver |
| Display hiển thị sai | Cấu hình TFT_eSPI đúng cho module 1.54" 240x240 |


## 📝 Notes

- ADPCM compression ratio: 4:1
- Animation frame delay: ANIMATION_FRAME_DELAY_MS (default 100ms)
- Pinout TFT và INMP441 cần cấu hình chính xác trong code

---

**Happy coding! 🤖💬**

## 🔩 PCB Design (Updated)

### Overview
- Thư mục dự án PCB: .\PCB (tất cả file PCB/gerber/BOM để trong folder này)
- Mục tiêu: PCB ESP32 nhỏ gọn tích hợp INMP441 (I2S), Class-D amp cho loa 4Ω 3W, TFT SPI 1.54" 240×240, mạch sạc/power cho pin LiPo (>=1600 mAh).

### Key Files (tại .\PCB)
- .\PCB\talk_robot.kicad_pcb
- .\PCB\schematic.kicad_sch
- .\PCB\BOM.csv
- .\PCB\gerbers\talk_robot_gerbers.zip
- .\PCB\stencil\talk_robot_paste.gbr
- .\PCB\assembly\ (pick-and-place, assembly drawings)

### Board & Layer Recommendation
- Layers: tối thiểu 2; ưu tiên 4 layers (plane nguồn/ground)
- Kích thước tham khảo: ~80×60 mm (tùy enclosure)
- Lỗ bắt 4 × M3, vùng keep-out cho đầu nối màn hình

### Schematic Highlights
- Nguồn:
    - Pin LiPo (3.7V nom, >=1600 mAh) với bảo vệ cell + mạch sạc (TP4056 hoặc tương đương) trên board hoặc module rời.
    - Regulator 3.3V chất lượng (LDO hoặc buck) cho ESP32 và các IC logic.
    - Cân nhắc bộ cấp cho ampli nếu cần điện áp cao hơn.
- ESP32: hàng header USB-UART (TX/RX/GND/3.3V/EN/BOOT)
- Microphone: INMP441 (I2S) — chân SD/WS/SCK → I2S_MIC
- Audio: I2S output → I2S DAC / Class-D amp tương thích (chọn amp hỗ trợ loa 4Ω 3W và điện áp cung cấp từ LiPo/regulator)
- Display: TFT 1.54" SPI (MOSI, MISO optional, SCLK, CS, DC, RST, BL) — cấu hình trong TFT_eSPI
- Peripherals: nút reset, nút user, LED trạng thái, khe TF-card (tùy chọn)

### Recommended Components
- MCU: ESP32-WROOM (module footprint)
- Mic: INMP441 (I2S MEMS)
- Amp: Class-D amplifier phù hợp với loa 4Ω 3W (đảm bảo điện áp cấp phù hợp)
- Display: TFT SPI 1.54" 240x240 (40-pin hoặc 8-pin module tuỳ model)
- Power: LiPo 3.7V, >=1600 mAh; mạch bảo vệ + sạc

### Connectors & Pin Mapping
- USB-UART: TXD/RXD/GND/3.3V/EN/BOOT
- INMP441 → ESP32: SD (DATA), SCK (BCLK), WS (LRCLK)
- I2S_SPK → Amp: BCLK, LRCLK, DATA_OUT
- TFT SPI → MOSI, SCLK, CS, DC, RST, BL (map trong TFT_eSPI)
- JTAG/Test pads: TMS/TDI/TDO/TCK (tuỳ chọn)

### Footprint & Layout Notes
- Đặt INMP441 xa loa và nguồn chuyển mạch; route ngắn cho tín hiệu audio.
- Decoupling caps gần mọi chân nguồn IC.
- Ground plane liên tục; stitch vias quanh vùng âm thanh và amp.
- Đảm bảo không gian và chốt cơ cho đầu nối màn hình.

### Manufacturing & Assembly
- Xuất Gerber trong .\PCB\gerbers, bao gồm silk, mask, paste
- Finish: ENIG recommended cho fine-pitch; HASL chấp nhận được
- Cung cấp stencil, fiducials, tooling holes nếu panelize

### Test & Bringup Checklist
1. Kiểm tra không short, đo 3.3V và Vin pin
2. Kết nối USB-UART, kiểm tra bootloader
3. Flash firmware, theo dõi serial logs
4. Kiểm tra INMP441 input và phát thử speaker
5. Khởi tạo TFT SPI và kiểm tra animation frames
6. Test WebSocket kết nối với backend


