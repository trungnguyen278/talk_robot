#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoWebsockets.h>
#include "driver/i2s.h"
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

// ===============================================================
// 1. CẤU HÌNH
// ===============================================================

// --- Cấu hình Mạng & WebSocket ---
// MODIFIED: Changed to a mutable char array to hold the IP from WiFiManager
char websocket_server_host[40] = "192.168.0.103"; // Default IP
const uint16_t websocket_server_port = 8000;
const char *websocket_server_path = "/ws";
#define TIMEOUT_MS 5000

// --- Chân cắm I2S (THEO SƠ ĐỒ MỚI ĐÃ SỬA LỖI) ---
#define I2S_MIC_SERIAL_CLOCK 14
#define I2S_MIC_WORD_SELECT 15
#define I2S_MIC_SERIAL_DATA 32

#define I2S_SPEAKER_SERIAL_CLOCK 26
#define I2S_SPEAKER_WORD_SELECT 25
#define I2S_SPEAKER_SERIAL_DATA 22

// --- Cài đặt I2S ---
#define I2S_SAMPLE_RATE 16000
#define I2S_MIC_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT // Mic INMP441 cần 32-bit
#define I2S_SPK_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT // Speaker vẫn giữ 16-bit
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_SPEAKER_PORT I2S_NUM_1
#define I2S_READ_CHUNK_SIZE 1024

// ==== ESP32 setup for your TFT ====
#define TFT_MOSI 21 // In some display driver board, it might be written as "SDA" and so on.
#define TFT_SCLK 23
#define TFT_CS 5   // Chip select control pin
#define TFT_DC 18  // Data Command control pin
#define TFT_RST 19 // Reset pin (could connect to Arduino RESET pin)
#define TFT_BL -1  // No pin connected

// --- Cấu hình Âm thanh & Animation ---
#define SPEAKER_GAIN 0.5f
#define PLAYBACK_BUFFER_SIZE 8192
#define ANIMATION_FRAME_DELAY_MS 50

// ===============================================================
// 2. BIẾN TOÀN CỤC VÀ KHAI BÁO
// ===============================================================

// --- Biến cho WebSocket & Âm thanh ---
using namespace websockets;
WebsocketsClient client;
// State enumeration
enum State
{
  STATE_DISCONNECT,
  STATE_STREAMING,
  STATE_FREE,
  STATE_WAITING,
  STATE_PLAYING_RESPONSE
};
volatile State currentState = STATE_FREE;
int32_t i2s_read_buffer[I2S_READ_CHUNK_SIZE / 4];
byte playback_buffer[PLAYBACK_BUFFER_SIZE];
size_t playback_buffer_fill = 0;

// --- Biến cho Màn hình & Animation ---
TFT_eSPI tft = TFT_eSPI();
typedef struct _VideoInfo
{
  const uint8_t *const *frames;
  const uint16_t *frames_size;
  uint16_t num_frames;
} VideoInfo;

// Include emotion definitions and video list
#include "..\..\include\emotion.h"

// Declare logo ptit as extern
extern VideoInfo ptit;
// Declare emotionList as extern
extern VideoInfo *emotionList[];
// Declare animationList as extern
extern VideoInfo *animationList[];
// emotion variable to track current emotion state
volatile uint8_t emotion = EMOTION_NEUTRAL;

unsigned long lastReceivedTime = millis();
// ===============================================================
// 3. CÁC HÀM CHO MÀN HÌNH (ANIMATION)
// (This section is unchanged)
// ===============================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= tft.height())
    return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Task to handle display animation
void display_task(void *pvParameters)
{
  uint16_t current_frame = 0;
  uint8_t current_emotion = EMOTION_NEUTRAL;
  uint8_t current_animation = 0;
  while (true)
  {
    // if in free state, play random animations
    if (currentState == STATE_FREE)
    {
      // Check frame bounds for animation. If exceeded, reset and pick new animation
      if (current_frame >= animationList[current_animation]->num_frames)
      {
        current_frame = 0;
        current_animation = random(0, sizeof(animationList) / sizeof(animationList[0]));
      }
      // Get JPEG data for the current animation frame
      const uint8_t *jpg_data = (const uint8_t *)pgm_read_ptr(&animationList[current_animation]->frames[current_frame]);
      // Get size of the current animation frame
      uint16_t jpg_size = pgm_read_word(&animationList[current_animation]->frames_size[current_frame]);
      // Draw the JPEG image
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    }
    // If connected, display emotion animations from server
    else if (currentState != STATE_DISCONNECT)
    {
      // Read the current emotion safely
      current_emotion = emotion;

      // Check frame bounds
      if (current_frame >= emotionList[current_emotion]->num_frames)
      {
        current_frame = 0;
      }
      // Get JPEG data for the current frame
      const uint8_t *jpg_data = (const uint8_t *)pgm_read_ptr(&emotionList[current_emotion]->frames[current_frame]);
      // Get size of the current frame
      uint16_t jpg_size = pgm_read_word(&emotionList[current_emotion]->frames_size[current_frame]);
      // Draw the JPEG image
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    }
    // If disconnected, do nothing (could display a static image or message)
    else
    {
      // delay for a while before checking again
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // Move to next frame
    ++current_frame;
    // Delay for the next frame
    vTaskDelay(pdMS_TO_TICKS(ANIMATION_FRAME_DELAY_MS));
  }
}

// ===============================================================
// 4. CÁC HÀM I2S VÀ WEBSOCKET
// (This section is unchanged)
// ===============================================================

// I2S Setup Functions
void setup_i2s_input()
{
  Serial.println("Configuring I2S Input (Microphone)...");
  i2s_config_t i2s_mic_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_MIC_BITS_PER_SAMPLE,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256};
  i2s_pin_config_t i2s_mic_pins = {
      .bck_io_num = I2S_MIC_SERIAL_CLOCK,
      .ws_io_num = I2S_MIC_WORD_SELECT,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SERIAL_DATA};
  ESP_ERROR_CHECK(i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins));
}

// I2S Output Setup Function
void setup_i2s_output()
{
  Serial.println("Configuring I2S Output (Speaker)...");
  i2s_config_t i2s_speaker_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_SPK_BITS_PER_SAMPLE,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
      .use_apll = true,
      .tx_desc_auto_clear = true};
  i2s_pin_config_t i2s_speaker_pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = I2S_SPEAKER_SERIAL_CLOCK,
      .ws_io_num = I2S_SPEAKER_WORD_SELECT,
      .data_out_num = I2S_SPEAKER_SERIAL_DATA,
      .data_in_num = I2S_PIN_NO_CHANGE};
  ESP_ERROR_CHECK(i2s_driver_install(I2S_SPEAKER_PORT, &i2s_speaker_config, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_SPEAKER_PORT, &i2s_speaker_pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_SPEAKER_PORT));
}

// WebSocket Event Handlers
void onWebsocketEvent(WebsocketsEvent event, String data)
{
  if (event == WebsocketsEvent::ConnectionOpened)
  {
    Serial.println("Websocket connection opened.");
    currentState = STATE_STREAMING;
  }
  else if (event == WebsocketsEvent::ConnectionClosed)
  {
    Serial.println("Websocket connection closed.");
  }
  else if (event == WebsocketsEvent::GotPing)
  {
    Serial.println("Websocket received ping.");
  }
  else if (event == WebsocketsEvent::GotPong)
  {
    Serial.println("Websocket received pong.");
  }
}

// WebSocket Message Handler
void onWebsocketMessage(WebsocketsMessage message)
{
  // --- Handling Text Messages ---
  lastReceivedTime = millis();
  if (message.isText())
  {

    String text_msg = String(message.c_str());
    Serial.printf("Server sent text: %s\n", text_msg.c_str());

    if (text_msg == "PROCESSING_START")
    {
      Serial.println("Server is processing. Pausing mic.");
      currentState = STATE_WAITING;
      emotion = EMOTION_NEUTRAL;
    }
    else if (text_msg == "TTS_END")
    {
      Serial.println("End of TTS. Returning to streaming mode.");
      // Flush any remaining audio in playback buffer
      if (playback_buffer_fill > 0)
      {
        size_t bytes_written = 0;

        i2s_write(I2S_SPEAKER_PORT, playback_buffer, playback_buffer_fill, &bytes_written, portMAX_DELAY);
        playback_buffer_fill = 0;
      }
      currentState = STATE_STREAMING;
      emotion = EMOTION_NEUTRAL;
    }
    else if (text_msg == "LISTENING")
    {
      Serial.println("listening ");
    }
    else
    {
      // Assume any other text message is emotion details
      Serial.println("Received emotion details from server.");
      if (text_msg == "00")
      {
        emotion = EMOTION_NEUTRAL;
      }
      else if (text_msg == "01")
      {
        emotion = EMOTION_HAPPY;
      }
      else if (text_msg == "10")
      {
        emotion = EMOTION_SAD;
      }
      else
      {
        // Serial.println("Unknown emotion code received.");
      }
    }

    // --- Handling Binary Audio Data ---
  }
  else if (message.isBinary())
  {
    if (currentState != STATE_PLAYING_RESPONSE)
    {
      Serial.println("Receiving audio from server, pausing mic and starting playback...");
      currentState = STATE_PLAYING_RESPONSE;
      i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
      playback_buffer_fill = 0;
    }

    size_t len = message.length();
    // ⚠️ Dùng c_str() để lấy dữ liệu nhị phân từ String
    const char *raw_data = message.c_str();

    // Tạo buffer tạm để xử lý dữ liệu PCM 16-bit
    int16_t temp_write_buffer[len / sizeof(int16_t)];
    memcpy(temp_write_buffer, raw_data, len);

    // Áp dụng gain nhưng giữ mức thấp để tránh clipping
    for (size_t i = 0; i < len / sizeof(int16_t); i++)
    {
      float amplified = temp_write_buffer[i] * SPEAKER_GAIN;
      // if (amplified > 32767.0f)
      // {
      //   Serial.println("Clipping detected in audio sample max!");
      //   amplified = 32767.0f;
      // }
      // if (amplified < -32768.0f) {
      //   Serial.println("Clipping detected in audio sample min!");
      //   amplified = -32768.0f;
      // }
      temp_write_buffer[i] = (int16_t)amplified;
    }

    // Copy dữ liệu đã xử lý vào playback buffer
    if (playback_buffer_fill + len <= PLAYBACK_BUFFER_SIZE)
    {
      memcpy(playback_buffer + playback_buffer_fill, temp_write_buffer, len);
      playback_buffer_fill += len;
    }

    // Flush khi đủ ngưỡng
    const size_t FLUSH_THRESHOLD = 2048;
    if (playback_buffer_fill >= FLUSH_THRESHOLD)
    {
      size_t bytes_written = 0;
      i2s_write(I2S_SPEAKER_PORT, playback_buffer, playback_buffer_fill, &bytes_written, portMAX_DELAY);
      playback_buffer_fill = 0;
    }
  }
}

void audio_processing_task(void *pvParameters)
{
  size_t bytes_read;
  while (true)
  {
    // Only read and send audio when in streaming state
    // Serial.println("Current State: " + String(currentState));
    if (currentState == STATE_STREAMING)
    {
      // Read audio data from I2S microphone
      i2s_read(I2S_MIC_PORT, i2s_read_buffer, sizeof(i2s_read_buffer), &bytes_read, portMAX_DELAY);

      int samples = bytes_read / sizeof(int32_t);
      int16_t pcm_buffer[samples];

      for (int i = 0; i < samples; i++)
      {
        // INMP441: dữ liệu nằm ở 24 bit cao, dịch xuống để lấy đúng biên độ
        pcm_buffer[i] = (int16_t)(i2s_read_buffer[i] >> 14);
      }
      // Log: in ra 5 mẫu đầu tiên để kiểm tra
      Serial.print("PCM[0..4]: ");
      for (int i = 0; i < 5 && i < samples; i++)
      {
        Serial.print(pcm_buffer[i]);
        Serial.print(" ");
      }
      Serial.println();
      if (client.available())
      {
        client.sendBinary((const char *)pcm_buffer, samples * sizeof(int16_t));
      }

      // thông báo ra serial thông tin vừa thu âm
      // Serial.println("Sent " + String(bytes_read) + " bytes of audio data to server.");
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// ===============================================================
// 5. SETUP & LOOP CHÍNH
// ===============================================================

void setup()
{
  Serial.begin(115200);

  // --- Initialize Display Early to show WiFi status ---
  // TFT ON
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  tft.begin();
  tft.setRotation(2);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(13);
  tft.drawString("PTIT", 40, 90);
  delay(5000);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);

  tft.drawString("Connecting to WiFi", 15, 100);

  // ==================== MODIFIED: WiFiManager Setup ====================
  WiFiManager wm;

  // ADDED: Create a custom parameter for the server IP
  // Arguments: ID, Label, Default Value, Max Length
  // WiFiManagerParameter custom_server_ip("server_ip", "WebSocket Server IP", websocket_server_host, 40);

  // ADDED: Add the custom parameter to the WiFiManager portal
  //  wm.addParameter(&custom_server_ip);

  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("VoiceAssistant-Config"))
  {
    Serial.println("Failed to connect and hit timeout");
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Config Failed.", 20, 100);
    tft.drawString("Restarting...", 20, 130);
    delay(3000);
    ESP.restart();
  }
  // =================================================================

  // ADDED: If connection is successful, read the custom IP address from the portal
  // and copy it into our variable.
  // strcpy(websocket_server_host, custom_server_ip.getValue());

  Serial.println("\nWiFi connected!");
  Serial.println("IP address: " + WiFi.localIP().toString());
  Serial.println("WebSocket Server IP: " + String(websocket_server_host));

  // --- Initialize Display fully ---
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);
  Serial.println("TFT Display Initialized.");

  setup_i2s_input();
  setup_i2s_output();

  client.onEvent(onWebsocketEvent);
  client.onMessage(onWebsocketMessage);

  // Now connect to the IP address provided by the user
  client.connect(websocket_server_host, websocket_server_port, websocket_server_path);

  xTaskCreatePinnedToCore(audio_processing_task, "Audio Task", 4096, NULL, 10, NULL, 1);
  xTaskCreatePinnedToCore(display_task, "Display Task", 4096, NULL, 5, NULL, 0);

  Serial.println("==============================================");
  Serial.println(" Voice Assistant Client with Mochi UI Ready");
  Serial.println("==============================================");

  lastReceivedTime = millis();
}

void loop()
{
  client.poll();

  if (WiFi.status() != WL_CONNECTED)
  {
    if (currentState != STATE_DISCONNECT)
    {
      Serial.println("WiFi disconnected!");
      currentState = STATE_DISCONNECT;
    }
    delay(500);
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("Reconnecting to WiFi...", 5, 100);
    delay(2000);
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Reconnecting to WiFi...");
    }
    else
    {
      Serial.println("Reconnected to WiFi, IP: " + WiFi.localIP().toString());
      currentState = STATE_STREAMING;
      emotion = EMOTION_NEUTRAL;
    }

    return; // Chờ kết nối lại trước khi xử lý websocket
  }

  // Check for timeout
  if (millis() - lastReceivedTime > TIMEOUT_MS)
  {
    Serial.println("No message received for " + String(TIMEOUT_MS) + " ms. Setting emotion to STUNNED.");
    currentState = STATE_STREAMING;
    emotion = EMOTION_NEUTRAL;
    // ping server if not disconnect and re connect
    if (!client.ping())
    {
      Serial.println("Ping failed. Reconnecting to WebSocket server...");
      client.close();
      delay(2000);
      client.connect(websocket_server_host, websocket_server_port, websocket_server_path);
    }
    lastReceivedTime = millis();
    return;
  }

  if (!client.available() && currentState != STATE_PLAYING_RESPONSE && currentState != STATE_WAITING)
  {
    // If disconnected, set emotion to STUNNED
    emotion = EMOTION_NEUTRAL;

    // Reset current video index if not playing or waiting
    Serial.println("WebSocket disconnected. Reconnecting...");

    // Attempt to reconnect to the configured server IP
    if (!client.connect(websocket_server_host, websocket_server_port, websocket_server_path))
    {
      Serial.println("Reconnect attempt failed.");
      delay(2000);
    }
    // If reconnected, set emotion back to NEUTRAL
    else
    {
      Serial.println("Reconnected to WebSocket server.");
      currentState = STATE_FREE;
    }
  }
  delay(10);
}