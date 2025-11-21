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
char websocket_server_host[40] = "13.239.36.114"; // Default IP 13.239.36.114
const uint16_t websocket_server_port = 8000;
const char *websocket_server_path = "/ws";
#define TIMEOUT_MS 20000

// --- Chân cắm I2S (THEO SƠ ĐỒ MỚI ĐÃ SỬA LỖI) ---
#define I2S_MIC_SERIAL_CLOCK 14
#define I2S_MIC_WORD_SELECT 15
#define I2S_MIC_SERIAL_DATA 32

#define I2S_SPEAKER_SERIAL_CLOCK 26
#define I2S_SPEAKER_WORD_SELECT 25
#define I2S_SPEAKER_SERIAL_DATA 22

// --- Cài đặt I2S ---
#define I2S_SAMPLE_RATE 16000
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
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

#define MIC_RING_BUFFER_SIZE 32768 // ~0.5s dữ liệu ở 16kHz
uint8_t mic_ring_buffer[MIC_RING_BUFFER_SIZE];
volatile size_t mic_write_pos = 0;
volatile size_t mic_read_pos = 0;
// ==== ADPCM & SPEAKER RING BUFFER ====
// buffer nhận từ server (dạng ADPCM)
#define SPK_RING_BUFFER_SIZE 16384
uint8_t spk_ring_buffer[SPK_RING_BUFFER_SIZE];
volatile size_t spk_write_pos = 0;
volatile size_t spk_read_pos = 0;

// ===============================================================
// 2. BIẾN TOÀN CỤC VÀ KHAI BÁO
// ===============================================================

// --- Biến cho WebSocket & Âm thanh ---
using namespace websockets;
WebsocketsClient client;
// State enumeration
enum State
{
  STATE_OFFLINE_WIFI,
  STATE_DISCONNECTED_WS,
  STATE_FREE,
  STATE_STREAMING,
  STATE_WAITING,
  STATE_PLAYING_RESPONSE
};
volatile State currentState = STATE_OFFLINE_WIFI;
byte i2s_read_buffer[I2S_READ_CHUNK_SIZE];
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
extern VideoInfo *logoPTIT;
// Declare stunnedEmotion as extern
extern VideoInfo *stunnedEmotion;
// Declare emotionList as extern
extern VideoInfo *emotionList[];
// Declare animationList as extern
extern VideoInfo *animationList[];
// Declare thinkingEmotion as extern
extern VideoInfo *thinkingEmotion;


// emotion variable to track current emotion state
volatile uint8_t emotion = EMOTION_NEUTRAL;

unsigned long lastReceivedTime = millis();

// state ADPCM (1 kênh, 16-bit)
typedef struct
{
  int16_t predictor;
  int8_t index;
} AdpcmState;

AdpcmState adpcm_mic_state = {0, 0};
AdpcmState adpcm_spk_state = {0, 0};

// ================= IMA ADPCM (DVI4) IMPLEMENTATION =================

static const int8_t indexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t stepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

int adpcm_encode_block(const int16_t *pcm_in, size_t num_samples,
                       uint8_t *adpcm_out, AdpcmState *st)
{
  int predictor = st->predictor;
  int index = st->index;
  if (index < 0)
    index = 0;
  if (index > 88)
    index = 88;
  int step = stepTable[index];

  size_t out_index = 0;
  uint8_t out_byte = 0;
  bool high_nibble = false;

  for (size_t i = 0; i < num_samples; ++i)
  {
    int sample = pcm_in[i];
    sample = sample * 3.0f; // tăng gain đầu vào

    // Clamp để tránh overflow int16 range
    if (sample > 32767)
      sample = 32767;
    if (sample < -32768)
      sample = -32768;
    int diff = sample - predictor;
    int sign = 0;
    if (diff < 0)
    {
      sign = 8;
      diff = -diff;
    }

    int delta = 0;
    int tempStep = step;

    if (diff >= tempStep)
    {
      delta |= 4;
      diff -= tempStep;
    }
    tempStep >>= 1;
    if (diff >= tempStep)
    {
      delta |= 2;
      diff -= tempStep;
    }
    tempStep >>= 1;
    if (diff >= tempStep)
    {
      delta |= 1;
    }

    int nibble = delta | sign;

    int diffq = step >> 3;
    if (delta & 4)
      diffq += step;
    if (delta & 2)
      diffq += (step >> 1);
    if (delta & 1)
      diffq += (step >> 2);

    if (sign)
      predictor -= diffq;
    else
      predictor += diffq;

    if (predictor > 32767)
      predictor = 32767;
    if (predictor < -32768)
      predictor = -32768;

    index += indexTable[nibble];
    if (index < 0)
      index = 0;
    if (index > 88)
      index = 88;
    step = stepTable[index];

    // pack 2 nibble / 1 byte: low then high
    if (!high_nibble)
    {
      out_byte = (uint8_t)(nibble & 0x0F);
      high_nibble = true;
    }
    else
    {
      out_byte |= (uint8_t)((nibble & 0x0F) << 4);
      adpcm_out[out_index++] = out_byte;
      high_nibble = false;
    }
  }

  // nếu còn lẻ 1 nibble
  if (high_nibble)
  {
    adpcm_out[out_index++] = out_byte;
  }

  st->predictor = (int16_t)predictor;
  st->index = (int8_t)index;

  return (int)out_index; // số byte ADPCM sinh ra
}

int adpcm_decode_block(const uint8_t *adpcm_in, size_t num_bytes,
                       int16_t *pcm_out, AdpcmState *st)
{
  int predictor = st->predictor;
  int index = st->index;
  if (index < 0)
    index = 0;
  if (index > 88)
    index = 88;
  int step = stepTable[index];

  size_t out_samples = 0;

  for (size_t i = 0; i < num_bytes; ++i)
  {
    uint8_t byte = adpcm_in[i];

    // 2 nibble: low, high
    for (int shift = 0; shift <= 4; shift += 4)
    {
      int nibble = (byte >> shift) & 0x0F;
      int sign = nibble & 8;
      int delta = nibble & 7;

      int diffq = step >> 3;
      if (delta & 4)
        diffq += step;
      if (delta & 2)
        diffq += (step >> 1);
      if (delta & 1)
        diffq += (step >> 2);

      if (sign)
        predictor -= diffq;
      else
        predictor += diffq;

      if (predictor > 32767)
        predictor = 32767;
      if (predictor < -32768)
        predictor = -32768;

      index += indexTable[nibble];
      if (index < 0)
        index = 0;
      if (index > 88)
        index = 88;
      step = stepTable[index];

      int32_t sample = (int32_t)(predictor * SPEAKER_GAIN);

      if (sample > 32767)
        sample = 32767;
      if (sample < -32768)
        sample = -32768;

      pcm_out[out_samples++] = (int16_t)sample;
    }
  }

  st->predictor = (int16_t)predictor;
  st->index = (int8_t)index;

  return (int)out_samples; // số sample PCM sinh ra
}

void clearMicRingBuffer()
{

  mic_write_pos = 0;
  mic_read_pos = 0;

  Serial.println("[RING] Cleared mic ring buffer");
}

void clearSpkRingBuffer()
{
  spk_write_pos = 0;
  spk_read_pos = 0;
  Serial.println("[RING] Cleared spk ring buffer");
}

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
    // If WiFi is offline, display a message
    else if (currentState == STATE_OFFLINE_WIFI)
    {
      // delay
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // If WebSocket is disconnected, display stunned animation
    else if (currentState == STATE_DISCONNECTED_WS)
    {
      // stunned animation
      //  Check frame bounds for stunned animation
      if (current_frame >= stunnedEmotion->num_frames)
      {
        current_frame = 0;
      }
      // Get JPEG data for the current stunned frame
      const uint8_t *jpg_data = (const uint8_t *)pgm_read_ptr(&stunnedEmotion->frames[current_frame]);
      // Get size of the current stunned frame
      uint16_t jpg_size = pgm_read_word(&stunnedEmotion->frames_size[current_frame]);
      // Draw the JPEG image
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    }
    else if (currentState == STATE_WAITING)
    {
      // thinking animation
      //  Check frame bounds for thinking animation
      if (current_frame >= thinkingEmotion->num_frames)
      {
        current_frame = 0;
      }
      // Get JPEG data for the current thinking frame
      const uint8_t *jpg_data = (const uint8_t *)pgm_read_ptr(&thinkingEmotion->frames[current_frame]);
      // Get size of the current thinking frame
      uint16_t jpg_size = pgm_read_word(&thinkingEmotion->frames_size[current_frame]);
      // Draw the JPEG image
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    }
    // If in streaming or waiting or playing response state, show current emotion
    else
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
      .bits_per_sample = I2S_BITS_PER_SAMPLE,
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
      .bits_per_sample = I2S_BITS_PER_SAMPLE,
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
    currentState = STATE_DISCONNECTED_WS;
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
  
  if (message.isText())
  {

    String text_msg = String(message.c_str());
    Serial.printf("[WS-TEXT] Received: %s\n", text_msg.c_str());

    if (text_msg == "PROCESSING_START")
    {
      Serial.println("Server is processing. Pausing mic.");
      currentState = STATE_WAITING;
      emotion = EMOTION_NEUTRAL;
      clearMicRingBuffer();
      clearSpkRingBuffer();
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
      clearMicRingBuffer();
      clearSpkRingBuffer(); // *** mới: clear buffer nhận
      // cũng có thể reset state decoder nếu muốn
      adpcm_spk_state.predictor = 0;
      adpcm_spk_state.index = 0;
    }

    size_t len = message.length();
    // Serial.printf("[WS-BIN] Received %d bytes ADPCM from server\n", len);
    const uint8_t *raw_data = (const uint8_t *)message.c_str();

    // Đẩy nguyên ADPCM vào spk_ring_buffer để net_task decode
    for (size_t i = 0; i < len; i++)
    {
      spk_ring_buffer[spk_write_pos++] = raw_data[i];
      if (spk_write_pos >= SPK_RING_BUFFER_SIZE)
      {
        spk_write_pos = 0;
      }
    }
  }
  lastReceivedTime = millis();
}

void audio_processing_task(void *pvParameters)
{
  size_t bytes_read;
  while (true)
  {
    if (currentState == STATE_OFFLINE_WIFI ||
        currentState == STATE_DISCONNECTED_WS)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue; // NGỪNG xử lý mic/spk buffer hoàn toàn
    }

    if (currentState == STATE_STREAMING || currentState == STATE_FREE)
    {
      i2s_read(I2S_MIC_PORT, i2s_read_buffer, I2S_READ_CHUNK_SIZE, &bytes_read, portMAX_DELAY);

      // Serial.printf("[MIC] Read %d bytes from I2S\n", bytes_read);
      if (bytes_read == I2S_READ_CHUNK_SIZE)
      {
        // i2s_read_buffer là PCM 16-bit
        int16_t *pcm = (int16_t *)i2s_read_buffer;
        size_t num_samples = bytes_read / sizeof(int16_t);

        // ADPCM 4:1 → cần khoảng num_samples / 2 bytes
        uint8_t adpcm_buf[I2S_READ_CHUNK_SIZE / 4];

        int adpcm_bytes = adpcm_encode_block(pcm, num_samples, adpcm_buf, &adpcm_mic_state);

        // ghi ADPCM vào mic_ring_buffer
        for (int i = 0; i < adpcm_bytes; i++)
        {
          mic_ring_buffer[mic_write_pos++] = adpcm_buf[i];
          if (mic_write_pos >= MIC_RING_BUFFER_SIZE)
          {
            mic_write_pos = 0;
          }
        }

        // Serial.printf("[MIC] Encoded %d samples -> %d bytes ADPCM, write_pos=%d, read_pos=%d\n", num_samples, adpcm_bytes, mic_write_pos, mic_read_pos);
      }
    }
    else if (currentState == STATE_PLAYING_RESPONSE)
    {
      if (spk_read_pos != spk_write_pos)
      {
        const size_t ADPCM_CHUNK = 512;
        size_t available = (spk_write_pos >= spk_read_pos)
                               ? (spk_write_pos - spk_read_pos)
                               : (SPK_RING_BUFFER_SIZE - spk_read_pos);

        // 🟡 THÊM LOG KIỂM TRA BUFFER TRỐNG
        if (available == 0)
        {
          Serial.println("[WARN] Speaker buffer EMPTY!!!");
        }
        else if (available < ADPCM_CHUNK)
        {
          Serial.printf("[WARN] Low buffer: %d bytes\n", available);
        }
        size_t use_size = min(ADPCM_CHUNK, available);

        uint8_t adpcm_block[ADPCM_CHUNK];
        memcpy(adpcm_block, &spk_ring_buffer[spk_read_pos], use_size);

        int16_t pcm_buf[ADPCM_CHUNK * 4];
        int pcm_samples = adpcm_decode_block(adpcm_block, use_size, pcm_buf, &adpcm_spk_state);

        if (pcm_samples > 0)
        {
          size_t bytes_written;
          i2s_write(I2S_SPEAKER_PORT,
                    pcm_buf,
                    pcm_samples * sizeof(int16_t),
                    &bytes_written,
                    portMAX_DELAY);
        }

        spk_read_pos = (spk_read_pos + use_size) % SPK_RING_BUFFER_SIZE;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
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
  tft.setRotation(1);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(13);
  tft.drawString("PTIT", 40, 90);
  delay(2000);
  // display logo PTIT
  /*
  const uint8_t *jpg_data = (const uint8_t *)pgm_read_ptr(&logoPTIT->frames[0]);
  uint16_t jpg_size = pgm_read_word(&logoPTIT->frames_size[0]);
  TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
  delay(2000);
  */
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

  if (!wm.autoConnect("PTalkPTIT-Config"))
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
  if (client.connect(websocket_server_host, websocket_server_port, websocket_server_path))
  {
    Serial.println("WebSocket connected successfully.");
  }
  else
  {
    Serial.println("WebSocket connection failed.");
  }

  // --- Create Tasks ---

  xTaskCreatePinnedToCore(audio_processing_task, "Audio Task", 8192, NULL, 10, NULL, 1);

  xTaskCreatePinnedToCore(display_task, "Display Task", 4096, NULL, 5, NULL, 0);

  Serial.println("==============================================");
  Serial.println(" Voice Assistant Client with Mochi UI Ready");
  Serial.println("==============================================");

  lastReceivedTime = millis();
}

void loop()
{
  client.poll(); // xử lý WS Event (Opened / Closed / Message)

  // === 1️⃣ Kiểm tra WiFi ===
  if (WiFi.status() != WL_CONNECTED)
  {
    if (currentState != STATE_OFFLINE_WIFI)
    {
      Serial.println("[STATE] -> OFFLINE_WIFI");
      currentState = STATE_OFFLINE_WIFI;
      clearMicRingBuffer();
      clearSpkRingBuffer();
    }

    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("Reconnecting WiFi...", 5, 100);
    delay(500);
    return;
  }

  // === 2️⃣ Kiểm tra WebSocket có đúng kết nối ===
  if (!client.available() && currentState != STATE_PLAYING_RESPONSE && currentState != STATE_WAITING)
  {
    if (currentState != STATE_DISCONNECTED_WS)
    {
      Serial.println("[STATE] -> DISCONNECTED_WS");
      currentState = STATE_DISCONNECTED_WS;
      clearMicRingBuffer();
      clearSpkRingBuffer();
    }

    static unsigned long lastReconnectTry = 0;
    if (millis() - lastReconnectTry > 5000) // 5s
    {
      Serial.println("[WS] Trying reconnect...");
      client.close();
      delay(200);

      if (client.connect(websocket_server_host, websocket_server_port, websocket_server_path))
        Serial.println("[WS] Reconnected.");
      else
        Serial.println("[WS] Reconnect failed.");

      lastReconnectTry = millis();
    }

    delay(50);
    return;
  }

  // === 3️⃣ Timeout không data từ server ===
  if (millis() - lastReceivedTime > TIMEOUT_MS && (currentState == STATE_STREAMING || currentState == STATE_FREE))
  {
    Serial.println("[TIMEOUT] WS idle, sending ping...");
    if (!client.ping())
    {
      Serial.println("[TIMEOUT] WS dead -> Disconnect");
      currentState = STATE_DISCONNECTED_WS;
      client.close();
    }
    else
    {
      Serial.println("[TIMEOUT] WS ping sent.");
      currentState = STATE_FREE;
    }
    lastReceivedTime = millis();
  }

  // ======================================================
  // 4️⃣ UPSTREAM: Gửi audio từ MIC lên server (ADPCM)
  // ======================================================
  if ((currentState == STATE_STREAMING || currentState == STATE_FREE) && client.available())
  {
    const size_t CHUNK_SIZE = 512;

    if (mic_read_pos != mic_write_pos)
    {
      size_t available = (mic_write_pos >= mic_read_pos)
                             ? (mic_write_pos - mic_read_pos)
                             : (MIC_RING_BUFFER_SIZE - mic_read_pos);

      size_t send_size = min(CHUNK_SIZE, available);

      uint8_t send_buf[CHUNK_SIZE];
      memcpy(send_buf, &mic_ring_buffer[mic_read_pos], send_size);

      if (client.sendBinary((const char *)send_buf, send_size))
      {
        mic_read_pos = (mic_read_pos + send_size) % MIC_RING_BUFFER_SIZE;
        // Serial.printf("[WS-TX] %d bytes\n", send_size);
      }
      else
      {
        Serial.println("[WS-TX] ERROR -> DISCONNECTED_WS");
        currentState = STATE_DISCONNECTED_WS;
      }
    }
  }


  delay(10);
}
