/*
 * ESP32 Voice Assistant Client (WebSocket PCM Streaming)
 * - 16 kHz mono PCM, 1024-byte chunks
 * - Jitter buffer bằng FreeRTOS RingBuffer + speaker task (prebuffer)
 * - Tối ưu Wi-Fi (no sleep, TX power), heartbeat WebSocket
 *
 * Yêu cầu:
 *  - ArduinoWebsockets
 *  - WiFiManager
 *  - TFT_eSPI + TJpg_Decoder (nếu dùng UI)
 *  - ESP32 Arduino core
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoWebsockets.h>
#include "driver/i2s.h"
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

// ===============================================================
// 1) CẤU HÌNH
// ===============================================================

// --- Mạng & WebSocket ---
char websocket_server_host[40] = "13.239.36.114";   // có thể thay bằng IP EC2/Public
const uint16_t websocket_server_port = 8000;
const char* websocket_server_path = "/ws";

#define TIMEOUT_MS 15000  // không nhận data > 15s => ping + reconnect

// --- Chân I2S (đã dùng sơ đồ mới) ---
#define I2S_MIC_SERIAL_CLOCK   14
#define I2S_MIC_WORD_SELECT    15
#define I2S_MIC_SERIAL_DATA    32

#define I2S_SPEAKER_SERIAL_CLOCK 26
#define I2S_SPEAKER_WORD_SELECT  25
#define I2S_SPEAKER_SERIAL_DATA  22

// --- Cấu hình Audio ---
#define I2S_SAMPLE_RATE        16000
#define I2S_BITS_PER_SAMPLE    I2S_BITS_PER_SAMPLE_16BIT
#define I2S_MIC_PORT           I2S_NUM_0
#define I2S_SPEAKER_PORT       I2S_NUM_1
#define I2S_READ_CHUNK_SIZE    1024        // bytes/chunk mic (≈ 32ms @16kHz)

#define SPEAKER_GAIN           1.0f        // 1.0 = không khuếch đại

// --- Jitter Buffer (downlink) ---
#define NET_CHUNK_BYTES        1024        // server gửi 1024 bytes PCM/khung
#define TTS_RINGBUF_BYTES      (64 * 1024) // 64KB ~ ~2s @16k mono
#define TTS_PREBUFFER_BYTES    (8 * 1024)  // ~0.25s đệm trước khi phát

// --- DMA Output tăng để tránh under-run ---
#define I2S_SPK_DMA_COUNT      12
#define I2S_SPK_DMA_LEN        512

// --- TFT pins (nếu cần) ---
#define TFT_BL -1

// ===============================================================
// 2) BIẾN TOÀN CỤC
// ===============================================================

using namespace websockets;
WebsocketsClient client;

enum State {
  STATE_DISCONNECT,
  STATE_STREAMING,        // gửi mic → server
  STATE_FREE,             // idle/animation rảnh
  STATE_WAITING,          // server đang xử lý
  STATE_PLAYING_RESPONSE  // nhận và phát PCM từ server
};
volatile State currentState = STATE_FREE;

byte i2s_read_buffer[I2S_READ_CHUNK_SIZE];

TFT_eSPI tft = TFT_eSPI();

#include "../../include/emotion.h"   // sửa đường dẫn cho phù hợp project
extern VideoInfo ptit;
extern VideoInfo* emotionList[];
extern VideoInfo* animationList[];
volatile uint8_t emotion = EMOTION_NEUTRAL;

unsigned long lastReceivedTime = 0;

// Ring buffer cho PCM xuống loa
RingbufHandle_t tts_ringbuf = NULL;
volatile bool tts_playing = false;

// ===============================================================
// 3) UI / ANIMATION
// ===============================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void display_task(void* pvParameters) {
  uint16_t current_frame = 0;
  uint8_t current_emotion = EMOTION_NEUTRAL;
  uint8_t current_animation = 0;

  for (;;) {
    if (currentState == STATE_FREE) {
      if (current_frame >= animationList[current_animation]->num_frames) {
        current_frame = 0;
        current_animation = random(0, (int)(sizeof(animationList) / sizeof(animationList[0])));
      }
      const uint8_t* jpg_data = (const uint8_t*)pgm_read_ptr(&animationList[current_animation]->frames[current_frame]);
      uint16_t jpg_size = pgm_read_word(&animationList[current_animation]->frames_size[current_frame]);
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    } else if (currentState != STATE_DISCONNECT) {
      current_emotion = emotion;
      if (current_frame >= emotionList[current_emotion]->num_frames) current_frame = 0;
      const uint8_t* jpg_data = (const uint8_t*)pgm_read_ptr(&emotionList[current_emotion]->frames[current_frame]);
      uint16_t jpg_size = pgm_read_word(&emotionList[current_emotion]->frames_size[current_frame]);
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    ++current_frame;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ===============================================================
// 4) I2S
// ===============================================================
void setup_i2s_input() {
  Serial.println("Configuring I2S Input (Microphone)...");
  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = I2S_MIC_SERIAL_CLOCK,
      .ws_io_num = I2S_MIC_WORD_SELECT,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SERIAL_DATA
  };
  ESP_ERROR_CHECK(i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_MIC_PORT, &pins));
}

void setup_i2s_output() {
  Serial.println("Configuring I2S Output (Speaker)...");
  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = I2S_SPK_DMA_COUNT,
      .dma_buf_len = I2S_SPK_DMA_LEN,
      .use_apll = true,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = I2S_SPEAKER_SERIAL_CLOCK,
      .ws_io_num = I2S_SPEAKER_WORD_SELECT,
      .data_out_num = I2S_SPEAKER_SERIAL_DATA,
      .data_in_num = I2S_PIN_NO_CHANGE
  };
  ESP_ERROR_CHECK(i2s_driver_install(I2S_SPEAKER_PORT, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_SPEAKER_PORT, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_SPEAKER_PORT));
}

// ===============================================================
// 5) WEBSOCKET
// ===============================================================
void onWebsocketEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.println("Websocket connection opened.");
    currentState = STATE_STREAMING;
    lastReceivedTime = millis();
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("Websocket connection closed.");
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.println("Websocket received ping.");
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.println("Websocket received pong.");
  }
}

void onWebsocketMessage(WebsocketsMessage message) {
  lastReceivedTime = millis();

  if (message.isText()) {
    String text_msg = String(message.c_str());
    Serial.printf("Server text: %s\n", text_msg.c_str());

    if (text_msg == "PROCESSING_START") {
      Serial.println("Server is processing. Pausing mic.");
      currentState = STATE_WAITING;
      emotion = EMOTION_NEUTRAL;
    } else if (text_msg == "TTS_END") {
      Serial.println("End of TTS. Draining ring buffer then back to streaming.");
      // Đợi ring buffer rỗng (tối đa ~2s)
      for (int i = 0; i < 200; ++i) {
        size_t free_sz = xRingbufferGetCurFreeSize(tts_ringbuf);
        if (free_sz == TTS_RINGBUF_BYTES) break;
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      currentState = STATE_STREAMING;
      emotion = EMOTION_NEUTRAL;
      tts_playing = false;
    } else if (text_msg == "LISTENING") {
      Serial.println("LISTENING");
    } else {
      // Map emotion code
      if (text_msg == "00") emotion = EMOTION_NEUTRAL;
      else if (text_msg == "01") emotion = EMOTION_HAPPY;
      else if (text_msg == "10") emotion = EMOTION_SAD;
    }
  }
  else if (message.isBinary()) {
    // Chuyển sang phát loa nếu chưa
    if (currentState != STATE_PLAYING_RESPONSE) {
      Serial.println("Receiving audio → start playback mode.");
      currentState = STATE_PLAYING_RESPONSE;
      i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
      tts_playing = false; // speaker task sẽ tự bật khi đủ prebuffer
    }

    const uint8_t* raw = (const uint8_t*)message.c_str();
    size_t len = message.length();

    // Đưa chunk vào ring buffer; nếu tràn -> drop-oldest để giữ real-time
    BaseType_t ok = xRingbufferSend(tts_ringbuf, raw, len, pdMS_TO_TICKS(5));
    if (ok != pdTRUE) {
      size_t drop_size;
      uint8_t* drop = (uint8_t*)xRingbufferReceiveUpTo(tts_ringbuf, &drop_size, 0, len);
      if (drop) vRingbufferReturnItem(tts_ringbuf, drop);
      xRingbufferSend(tts_ringbuf, raw, len, 0);
    }
  }
}

// ===============================================================
// 6) TASKS
// ===============================================================
void audio_processing_task(void* pvParameters) {
  size_t bytes_read = 0;

  for (;;) {
    if (currentState == STATE_STREAMING && client.available()) {
      i2s_read(I2S_MIC_PORT, i2s_read_buffer, I2S_READ_CHUNK_SIZE, &bytes_read, portMAX_DELAY);
      if (bytes_read == I2S_READ_CHUNK_SIZE) {
        client.sendBinary((const char*)i2s_read_buffer, bytes_read);
        // pace nhẹ; i2s_read đã chặn ~30ms
        vTaskDelay(pdMS_TO_TICKS(2));
      } else {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

void speaker_task(void* pvParameters) {
  size_t item_size;
  uint8_t* item = nullptr;

  for (;;) {
    if (currentState == STATE_PLAYING_RESPONSE) {
      // Kiểm tra lượng đệm
      size_t free_sz = xRingbufferGetCurFreeSize(tts_ringbuf);
      size_t buffered = TTS_RINGBUF_BYTES - free_sz;

      if (!tts_playing) {
        if (buffered >= TTS_PREBUFFER_BYTES) {
          tts_playing = true;
        } else {
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
      }

      item = (uint8_t*)xRingbufferReceive(tts_ringbuf, &item_size, pdMS_TO_TICKS(20));
      if (item && item_size > 0) {
        // Áp dụng gain nếu cần (mặc định 1.0f: không thay đổi)
        if (SPEAKER_GAIN != 1.0f) {
          int16_t* s = (int16_t*)item;
          size_t n = item_size / sizeof(int16_t);
          for (size_t i = 0; i < n; ++i) {
            float v = (float)s[i] * SPEAKER_GAIN;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            s[i] = (int16_t)v;
          }
        }

        size_t written = 0;
        i2s_write(I2S_SPEAKER_PORT, item, item_size, &written, portMAX_DELAY);
        vRingbufferReturnItem(tts_ringbuf, (void*)item);
      } else {
        // tạm thiếu dữ liệu
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    } else {
      tts_playing = false;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// ===============================================================
// 7) SETUP & LOOP
// ===============================================================
void setup() {
  Serial.begin(115200);

  // Tối ưu Wi-Fi & WebSocket
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // ====== UI khởi động ======
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  tft.begin();
  tft.setRotation(2);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(13);
  tft.drawString("PTIT", 40, 90);
  delay(1000);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Connecting to WiFi", 15, 100);

  // ====== WiFiManager ======
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("VoiceAssistant-Config")) {
    Serial.println("WiFi config timeout. Restarting...");
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Config Failed. Restarting...", 10, 100);
    delay(2000);
    ESP.restart();
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP address: "); Serial.println(WiFi.localIP());
  Serial.print("WebSocket Server IP: "); Serial.println(websocket_server_host);

  // ====== TFT/JPEG ======
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);
  Serial.println("TFT Display Initialized.");

  // ====== I2S ======
  setup_i2s_input();
  setup_i2s_output();

  // ====== Ring buffer ======
  tts_ringbuf = xRingbufferCreate(TTS_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
  assert(tts_ringbuf != NULL);

  // ====== WebSocket ======
  client.onEvent(onWebsocketEvent);
  client.onMessage(onWebsocketMessage);
  // Heartbeat nếu thư viện hỗ trợ:
  #ifdef ARDUINO_WEBSOCKETS_VERSION
  client.setHeartbeat(15000, 3000, 2); // 15s keepalive, 3s timeout, 2 lần fail
  #endif

  if (!client.connect(websocket_server_host, websocket_server_port, websocket_server_path)) {
    Serial.println("Initial WS connect failed, retrying in 2s...");
    delay(2000);
    client.connect(websocket_server_host, websocket_server_port, websocket_server_path);
  }

  // ====== Tasks ======
  xTaskCreatePinnedToCore(audio_processing_task, "Audio Task",   4096, NULL, 10, NULL, 1);
  xTaskCreatePinnedToCore(display_task,         "Display Task",  4096, NULL,  5, NULL, 0);
  xTaskCreatePinnedToCore(speaker_task,         "Speaker Task",  4096, NULL, 10, NULL, 1);

  Serial.println("==============================================");
  Serial.println(" Voice Assistant Client Ready");
  Serial.println("==============================================");

  lastReceivedTime = millis();
  currentState = STATE_FREE;
  emotion = EMOTION_NEUTRAL;
}

void loop() {
  client.poll();

  // Wi-Fi giữ ổn định
  if (WiFi.status() != WL_CONNECTED) {
    if (currentState != STATE_DISCONNECT) {
      Serial.println("WiFi disconnected!");
      currentState = STATE_DISCONNECT;
    }
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Reconnecting WiFi...", 10, 100);
    delay(2000);
    return;
  }

  // Timeout nhận tin → ping & reconnect
  if (millis() - lastReceivedTime > TIMEOUT_MS) {
    Serial.println("No message in timeout window, send ping/reconnect.");
    currentState = (currentState == STATE_PLAYING_RESPONSE) ? STATE_PLAYING_RESPONSE : STATE_STREAMING;
    emotion = EMOTION_NEUTRAL;

    if (!client.ping()) {
      Serial.println("Ping failed → reconnect WS...");
      client.close();
      delay(1000);
      client.connect(websocket_server_host, websocket_server_port, websocket_server_path);
    }
    lastReceivedTime = millis();
  }

  // Nếu WS mất kết nối (không ở chế độ phát/đợi) → reconnect
  if (!client.available() && currentState != STATE_PLAYING_RESPONSE && currentState != STATE_WAITING) {
    emotion = EMOTION_NEUTRAL;
    Serial.println("WebSocket disconnected. Reconnecting...");
    if (!client.connect(websocket_server_host, websocket_server_port, websocket_server_path)) {
      Serial.println("Reconnect failed.");
      delay(1500);
    } else {
      Serial.println("Reconnected to WebSocket server.");
      currentState = STATE_FREE;
    }
  }

  delay(10);
}