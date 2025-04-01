#include <WiFi.h>
#include <WebSocketsClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"

// --- Cấu hình WiFi ---
const char* ssid = "B10509_2.4G";
const char* password = "509509509";

// --- Cấu hình Server ---
const char* websockets_server_host = "192.168.1.6"; 
const uint16_t websockets_server_port = 8080;
const char* websockets_path = "/";

// --- Cấu hình I2S ---
#define I2S_WS            15
#define I2S_SD            32
#define I2S_SCK           14
#define I2S_PORT          I2S_NUM_0
#define I2S_SAMPLE_RATE   (16000)
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT
#define I2S_READ_LEN      (1024 * 4)
#define I2S_BUFFER_COUNT  8

// --- Cấu hình Ghi âm ---
#define RECORD_DURATION_MS 5000 // 5 giây

// --- Biến Global ---
WebSocketsClient webSocket;
bool isRecording = false;
unsigned long recordingStartTime = 0;
TaskHandle_t recordTaskHandle = NULL;

// --- Hàm xử lý WebSocket ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_DISCONNECTED) Serial.println("[WSc] Disconnected!");
  else if (type == WStype_CONNECTED) Serial.println("[WSc] Connected!");
  else if (type == WStype_TEXT) Serial.printf("[WSc] Received: %s\n", payload);
}

// --- Hàm cấu hình I2S ---
// --- Hàm cấu hình I2S ---
void i2s_install() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_BUFFER_COUNT,
    .dma_buf_len = I2S_READ_LEN / I2S_BUFFER_COUNT,
    .use_apll = false,
    // .fixed_mclk = -1 // Dòng này có thể không cần thiết, hoặc đặt là 0 nếu bạn chắc chắn
    // Tốt hơn là để driver tự quyết định hoặc tắt hẳn bằng pin_config
  };

  // Gọi install MỘT LẦN và kiểm tra kết quả
  esp_err_t install_result = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (install_result != ESP_OK) {
      Serial.printf("Failed to install I2S driver. Error: %d (%s)\n", install_result, esp_err_to_name(install_result));
      while (true);
  }
  Serial.println("I2S Driver installed.");

  // Cấu hình chân, đảm bảo có mclk_io_num
  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,          // Chân Bit Clock (BCLK)
      .ws_io_num = I2S_WS,           // Chân Word Select (LRCL)
      .data_out_num = I2S_PIN_NO_CHANGE, // Không dùng data out cho RX
      .data_in_num = I2S_SD        // Chân Serial Data (DIN)!
  };

  // Gọi set_pin MỘT LẦN và kiểm tra kết quả
  esp_err_t set_pin_result = i2s_set_pin(I2S_PORT, &pin_config);
  if (set_pin_result != ESP_OK) {
      Serial.printf("Failed to set I2S pins. Error: %d (%s)\n", set_pin_result, esp_err_to_name(set_pin_result));
      // Nếu set pin lỗi, nên gỡ driver trước khi dừng
      i2s_driver_uninstall(I2S_PORT);
      while (true);
  }
  Serial.println("I2S Pins configured.");

  // Gọi start MỘT LẦN sau khi install và set pin thành công
  esp_err_t start_result = i2s_start(I2S_PORT);
   if (start_result != ESP_OK) {
      Serial.printf("Failed to start I2S. Error: %d (%s)\n", start_result, esp_err_to_name(start_result));
       // Nếu start lỗi, nên gỡ driver trước khi dừng
      i2s_driver_uninstall(I2S_PORT);
      while (true);
  }
   Serial.println("I2S Started.");
}

// --- Task ghi âm ---
void recordAndSendTask(void * parameter) {
  Serial.println("Recording...");
  int32_t *i2s_read_buffer = (int32_t*) malloc(I2S_READ_LEN);
  recordingStartTime = millis();
  size_t bytes_read = 0;

  while (isRecording && (millis() - recordingStartTime < RECORD_DURATION_MS)) {
    i2s_read(I2S_PORT, i2s_read_buffer, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
    int16_t *pcm_buffer = (int16_t*) malloc(bytes_read / 2);
    for (int i = 0; i < bytes_read / 4; i++) {
      pcm_buffer[i] = (int16_t)(i2s_read_buffer[i] >> 16);
    }
    if (webSocket.isConnected()) {
      Serial.printf("Sending %d bytes\n", bytes_read / 2);
      webSocket.sendBIN((uint8_t*)pcm_buffer, bytes_read / 2);
    }
    free(pcm_buffer);
  }
  Serial.println("Recording finished.");
  isRecording = false;
  free(i2s_read_buffer);
  delay(500);
  webSocket.sendTXT("END_OF_STREAM");
  Serial.println("SendTXT end of stream");
  recordTaskHandle = NULL;
  vTaskDelete(NULL);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("WiFi connected.");
  i2s_install();
  webSocket.begin(websockets_server_host, websockets_server_port, websockets_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

// --- Loop ---
void loop() {
  webSocket.loop();
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 's' && !isRecording && webSocket.isConnected()) {
      Serial.println("Start recording!");
      isRecording = true;
      xTaskCreatePinnedToCore(recordAndSendTask, "RecordSendTask", 8192, NULL, 1, &recordTaskHandle, 1);
    }
  }
}
