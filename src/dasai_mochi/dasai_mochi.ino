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
char websocket_server_host[40] = "192.168.50.115"; // Default IP
const uint16_t websocket_server_port = 8000;
const char* websocket_server_path = "/ws";

// --- Chân cắm I2S (THEO SƠ ĐỒ MỚI ĐÃ SỬA LỖI) ---
#define I2S_MIC_SERIAL_CLOCK    26
#define I2S_MIC_WORD_SELECT     25
#define I2S_MIC_SERIAL_DATA     33

#define I2S_SPEAKER_SERIAL_CLOCK 18
#define I2S_SPEAKER_WORD_SELECT  23
#define I2S_SPEAKER_SERIAL_DATA  19

// --- Cài đặt I2S ---
#define I2S_SAMPLE_RATE         16000
#define I2S_BITS_PER_SAMPLE     I2S_BITS_PER_SAMPLE_16BIT
#define I2S_MIC_PORT            I2S_NUM_0
#define I2S_SPEAKER_PORT        I2S_NUM_1
#define I2S_READ_CHUNK_SIZE     1024

// ==== ESP32 setup for your TFT ====
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4     
#define TFT_BL   -1    // No pin connected

// --- Cấu hình Âm thanh & Animation ---
#define SPEAKER_GAIN            8.0f
#define PLAYBACK_BUFFER_SIZE    4096
#define ANIMATION_FRAME_DELAY_MS 50

// ===============================================================
// 2. BIẾN TOÀN CỤC VÀ KHAI BÁO
// ===============================================================

// --- Biến cho WebSocket & Âm thanh ---
using namespace websockets;
WebsocketsClient client;
enum State { STATE_STREAMING, STATE_WAITING, STATE_PLAYING_RESPONSE };
volatile State currentState = STATE_STREAMING;
byte i2s_read_buffer[I2S_READ_CHUNK_SIZE];
byte playback_buffer[PLAYBACK_BUFFER_SIZE];
size_t playback_buffer_fill = 0;

// --- Biến cho Màn hình & Animation ---
TFT_eSPI tft = TFT_eSPI();
typedef struct _VideoInfo {
  const uint8_t* const* frames;
  const uint16_t* frames_size;
  uint16_t num_frames;
} VideoInfo;

// Include emotion definitions and video list
#include "..\..\include\emotion.h"
// Declare emotionList as extern
extern VideoInfo* emotionList[];
// Declare animationList as extern
extern VideoInfo* animationList[];
// emotion variable to track current emotion state
volatile uint8_t emotion = EMOTION_NEUTRAL;
// Working flag
volatile bool working = true;
// Working timer handle
TimerHandle_t working_timer = NULL;

// ===============================================================
// 3. CÁC HÀM CHO MÀN HÌNH (ANIMATION)
// (This section is unchanged)
// ===============================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Callback to reset working flag
void working_timer_callback(TimerHandle_t xTimer) {
  // Reset working flag when timer expires
  Serial.println("Working timer expired");
  if (xTimer != NULL && emotion != EMOTION_STUNNED && currentState == STATE_STREAMING) {
    working = false;
  }
}



// Task to handle display animation
void display_task(void *pvParameters) {
  uint16_t current_frame = 0;
  uint8_t current_emotion = EMOTION_NEUTRAL;
  uint8_t current_animation = 0;
  while(true) {
    // Check if working display emotion
    if (working) {
      // Read the current emotion safely
      current_emotion = emotion;

      // Check frame bounds
      if (current_frame >= emotionList[current_emotion]->num_frames) {
        current_frame = 0;
      }
      // Get JPEG data for the current frame
      const uint8_t* jpg_data = (const uint8_t*)pgm_read_ptr(&emotionList[current_emotion]->frames[current_frame]);
      // Get size of the current frame
      uint16_t jpg_size = pgm_read_word(&emotionList[current_emotion]->frames_size[current_frame]);
      // Draw the JPEG image
      TJpgDec.drawJpg(0, 0, jpg_data, jpg_size);
    } 
    // If not working, show random animations
    else {
      // Check frame bounds for animation. If exceeded, reset and pick new animation
      if(current_frame >= animationList[current_animation]->num_frames) {
        current_frame = 0;
        current_animation = random(0, sizeof(animationList) / sizeof(animationList[0]));
      }
      // Get JPEG data for the current animation frame
      const uint8_t* jpg_data = (const uint8_t*)pgm_read_ptr(&animationList[current_animation]->frames[current_frame]);
      // Get size of the current animation frame
      uint16_t jpg_size = pgm_read_word(&animationList[current_animation]->frames_size[current_frame]);
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
void setup_i2s_input() {
    Serial.println("Configuring I2S Input (Microphone)...");
    i2s_config_t i2s_mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256
    };
    i2s_pin_config_t i2s_mic_pins = {
        .bck_io_num = I2S_MIC_SERIAL_CLOCK,
        .ws_io_num = I2S_MIC_WORD_SELECT,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SERIAL_DATA
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins));
}

// I2S Output Setup Function
void setup_i2s_output() {
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
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t i2s_speaker_pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_SPEAKER_SERIAL_CLOCK,
        .ws_io_num = I2S_SPEAKER_WORD_SELECT,
        .data_out_num = I2S_SPEAKER_SERIAL_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE        
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_SPEAKER_PORT, &i2s_speaker_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_SPEAKER_PORT, &i2s_speaker_pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_SPEAKER_PORT));
}

// WebSocket Event Handlers
void onWebsocketEvent(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Websocket connection opened.");
        currentState = STATE_STREAMING;
        //current_video_index = 0;
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Websocket connection closed.");
    }
}

// WebSocket Message Handler
void onWebsocketMessage(WebsocketsMessage message) {
    // --- Handling Text Messages ---
    if (message.isText()) {
      
        String text_msg = String(message.c_str());
        Serial.printf("Server sent text: %s\n", text_msg.c_str());
        
        if (text_msg == "PROCESSING_START") {
            Serial.println("Server is processing. Pausing mic.");
            currentState = STATE_WAITING;
            //current_video_index = 1;
        } 
        else if (text_msg == "TTS_END") {
            Serial.println("End of TTS. Returning to streaming mode.");
            if (playback_buffer_fill > 0) {
                size_t bytes_written = 0;
                
                i2s_write(I2S_SPEAKER_PORT, playback_buffer, playback_buffer_fill, &bytes_written, portMAX_DELAY);
                playback_buffer_fill = 0;
            }
            currentState = STATE_STREAMING;
            emotion = EMOTION_NEUTRAL;
            // Restart working timer
            if (working_timer != NULL) {
                xTimerStart(working_timer, 0);
            }
        }     
        else {
            // Assume any other text message is emotion details
            Serial.println("Received emotion details from server.");
            if (text_msg == "00") {
                emotion = EMOTION_NEUTRAL;
            } else if (text_msg == "01") {
                emotion = EMOTION_HAPPY;
            } else if (text_msg == "10") {
                emotion = EMOTION_SAD;
            } else {
                Serial.println("Unknown emotion code received.");
            }
            working = true;
        }

    // --- Handling Binary Audio Data ---
    } else if (message.isBinary()) {
        if (currentState != STATE_PLAYING_RESPONSE) {
            Serial.println("Receiving audio from server, pausing mic and starting playback...");
            currentState = STATE_PLAYING_RESPONSE;
            i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
            playback_buffer_fill = 0;
        }
        size_t len = message.length();
        int16_t temp_write_buffer[len / sizeof(int16_t)];
        memcpy(temp_write_buffer, message.c_str(), len);
        for (int i = 0; i < len / sizeof(int16_t); i++) {
          float amplified = temp_write_buffer[i] * SPEAKER_GAIN;
          if (amplified > 32767) amplified = 32767;
          if (amplified < -32768) amplified = -32768;
          temp_write_buffer[i] = (int16_t)amplified;
        }
        if (playback_buffer_fill + len <= PLAYBACK_BUFFER_SIZE) {
            memcpy(playback_buffer + playback_buffer_fill, temp_write_buffer, len);
            playback_buffer_fill += len;
        }
        const size_t FLUSH_THRESHOLD = 2048;
        if (playback_buffer_fill >= FLUSH_THRESHOLD) {
            size_t bytes_written = 0;
            i2s_write(I2S_SPEAKER_PORT, playback_buffer, playback_buffer_fill, &bytes_written, portMAX_DELAY);
            playback_buffer_fill = 0;
        }
    }
}

void audio_processing_task(void *pvParameters) {
  size_t bytes_read;
  while (true) {
    // Only read and send audio when in streaming state
    if (currentState == STATE_STREAMING) {
        // Read audio data from I2S microphone
        i2s_read(I2S_MIC_PORT, i2s_read_buffer, I2S_READ_CHUNK_SIZE, &bytes_read, portMAX_DELAY);
        if (bytes_read == I2S_READ_CHUNK_SIZE && client.available()) {
            client.sendBinary((const char*)i2s_read_buffer, bytes_read);
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// ===============================================================
// 5. SETUP & LOOP CHÍNH
// ===============================================================

void setup() {
  Serial.begin(115200);

  // --- Initialize Display Early to show WiFi status ---
  tft.begin();
  tft.setRotation(2);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.drawString("Connecting to WiFi", 15, 100);

  // ==================== MODIFIED: WiFiManager Setup ====================
  WiFiManager wm;

  // ADDED: Create a custom parameter for the server IP
  // Arguments: ID, Label, Default Value, Max Length
  WiFiManagerParameter custom_server_ip("server_ip", "WebSocket Server IP", websocket_server_host, 40);
  
  // ADDED: Add the custom parameter to the WiFiManager portal
  wm.addParameter(&custom_server_ip);

  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("VoiceAssistant-Config")) {
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
  strcpy(websocket_server_host, custom_server_ip.getValue());
  
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

  // Create the working timer
  working_timer = xTimerCreate("Working Timer", pdMS_TO_TICKS(10000), pdFALSE, (void*)0, working_timer_callback);
  xTimerStart(working_timer, 0);

  if (working_timer == NULL) {
    Serial.println("Failed to create working timer.");
  } else {
    Serial.println("Working timer created.");
    xTimerStart(working_timer, 0);
  }

  Serial.println("==============================================");
  Serial.println(" Voice Assistant Client with Mochi UI Ready");
  Serial.println("==============================================");
}

void loop() {
  client.poll();

  if (!client.available() && currentState != STATE_PLAYING_RESPONSE && currentState != STATE_WAITING) {
    // If disconnected, set emotion to STUNNED
    emotion = EMOTION_STUNNED;
    working = true;
    // Reset current video index if not playing or waiting
    Serial.println("WebSocket disconnected. Reconnecting...");
    // Attempt to reconnect to the configured server IP
    if (!client.connect(websocket_server_host, websocket_server_port, websocket_server_path)) {
      Serial.println("Reconnect attempt failed.");
      delay(2000);
    }
    // If reconnected, set emotion back to NEUTRAL
    else {
      Serial.println("Reconnected to WebSocket server.");
      emotion = EMOTION_NEUTRAL;
      // Restart working timer
      if (working_timer != NULL) {
        xTimerStart(working_timer, 0);
      }
    }

  }
  delay(10);
}