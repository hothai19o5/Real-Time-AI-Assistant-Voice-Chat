#include <WiFi.h>
#include <WebSocketsClient.h>  // Thư viện WebSocketClient cho ESP32, cung cấp các hàm để kết nối và giao tiếp với WebSocket server như begin, onEvent, sendTXT, v.v.
#include "freertos/FreeRTOS.h" // Thư viện FreeRTOS cho ESP32, cung cấp các hàm để tạo task, hàng đợi, mutex, v.v. như xTaskCreatePinnedToCore, xQueueCreate, xSemaphoreCreateMutex, v.v.
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h" // Thư viện I2S cho ESP32, cung cấp các hàm để cấu hình và sử dụng giao thức I2S như i2s_driver_install, i2s_set_pin, i2s_set_clk, v.v.

// --- Cấu hình WiFi Hardcode---
// Nên sửa lại dùng WiFiManager để tự động kết nối với WiFi
const char *ssid = "B10509_2.4G";
const char *password = "509509509";

// --- Cấu hình Server Hardcode---
// Nên sửa lại để có thể config được thông tin này từ 1 WebServer
char *websockets_server_host = "192.168.1.22";
const uint16_t websockets_server_port = 8080;
const char *websockets_path = "/";

// --- Cấu hình I2S ---
#define I2S_WS 15 // Chân WS và LRC
#define I2S_SD_IN 32
#define I2S_SD_OUT 25
#define I2S_SCK 14        // Chân SCK và BCLK
#define SPEAKER_SD_PIN 26 // Chân Shutdown cho loa MAX98357A
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE (16000)                          // Tốc độ lấy mẫu, sử dụng 16kHz để giảm dữ liệu
#define I2S_BITS_PER_SAMPLE_RX I2S_BITS_PER_SAMPLE_32BIT // Số bit mỗi mẫu ghi âm INMP441
#define I2S_READ_LEN (1024 * 4)                          // Độ dài buffer khi đọc dữ liệu, mỗi lần đọc 4KB từ mic
#define I2S_BITS_PER_SAMPLE_TX I2S_BITS_PER_SAMPLE_16BIT // Số bit mỗi mẫu phát MAX98357A
#define SPEAKER_CHANNEL_FMT I2S_CHANNEL_FMT_ONLY_LEFT    // Chỉ sử dụng kênh trái cho MAX98357A
#define MIC_CHANNEL_FMT I2S_CHANNEL_FMT_ONLY_LEFT        // Chỉ sử dụng kênh trái cho mic INMP441
#define I2S_BUFFER_COUNT 8                               // Số lượng buffer DMA (Direct Memory Access) cho I2S, DMA là vùng bộ nhớ trung gian để lưu dữ liệu được truyền/nhận giữa thiết bị ngoại vi (như mic/loa) và RAM, mà không cần CPU can thiệp trực tiếp

#define RECORD_DURATION_MS 5000 // Thời gian ghi âm, cố định 5s, sẽ nâng cấp sau

#define PIN_BUTTON 4  // Chân nút nhấn ghi âm
#define LED_RECORD 17 // LED báo đang ghi âm
#define LED_PLAY 16   // LED báo đang phát

#define MIC_GAIN_FACTOR 4.0 // Hệ số khuếch đại microphone, khuếch đại để server xử lý dễ hơn

// Định nghĩa cấu trúc để gửi vào hàng đợi phát âm thanh
typedef struct
{
  uint8_t *data;
  size_t length;
} AudioChunk;

// --- Theo dõi trạng thái I2S hiện tại ---
typedef enum
{
  I2S_MODE_IDLE,
  I2S_MODE_MIC,
  I2S_MODE_SPEAKER
} current_i2s_mode_t;

bool buttonPressed = false; // Biến để theo dõi trạng thái nút nhấn
// Khi nhấn nút thì tín hiệu sẽ không chuyển từ Low -> High luôn mà sẽ thay đổi trạng thái từ Low -> High -> Low -> High -> Low
// Cách xử lý: Khi nhấn nút thì sẽ ghi nhận trạng thái nhấn nút, sau đó sẽ kiểm tra thời gian thay đổi trạng thái nút nhấn
// Nếu thời gian thay đổi trạng thái lớn hơn DEBOUNCE_TIME thì sẽ ghi nhận là nhấn nút
unsigned long currentTime;              // Thời gian hiện tại, dùng để kiểm tra thời gian thay đổi trạng thái nút nhấn
unsigned long lastButtonChangeTime = 0; // Thời gian thay đổi trạng thái nút nhấn, tính toán chống dội
#define DEBOUNCE_TIME 50                // Thời gian chống dội (50ms)

// Biến để theo dõi chế độ I2S hiện tại
// Mic sử dụng 32-bit, Speaker sử dụng 16-bit => không dùng chung được 1 lần khởi tạo I2S
// Mỗi lần muốn sử dụng cái nào thì cần khởi tạo lại I2S
volatile current_i2s_mode_t current_i2s_mode = I2S_MODE_IDLE;

// Mutex để bảo vệ việc thay đổi cấu hình I2S
// Mutex này sẽ đảm bảo rằng chỉ có một task có thể thay đổi cấu hình I2S tại một thời điểm
// Điều này rất quan trọng vì việc thay đổi cấu hình I2S có thể mất thời gian và nếu có nhiều task cùng thay đổi cấu hình
SemaphoreHandle_t i2s_mutex = NULL;

WebSocketsClient webSocket; // Khởi tạo WebSocket client

bool isRecording = false; // Biến để theo dõi trạng thái ghi âm

unsigned long recordingStartTime = 0; // Thời gian bắt đầu ghi âm

TaskHandle_t recordTaskHandle = NULL; // Task ghi âm

TaskHandle_t playAudioTaskHandle = NULL; // Task phát âm thanh

QueueHandle_t playAudioQueue = NULL; // Hàng đợi phát âm thanh

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length); // Khai báo hàm xử lý sự kiện WebSocket, xử lý các sự kiện connect, disconnect, message, bin, error, ping, pong

esp_err_t switch_i2s_mode(current_i2s_mode_t new_mode); // Khai báo hàm chuyển đổi chế độ I2S, chuyển đổi giữa các chế độ MIC, SPEAKER và IDLE

esp_err_t i2s_init(); // Khai báo hàm khởi tạo I2S driver, hàm này sẽ được gọi 1 lần duy nhất trong setup() để khởi tạo I2S driver và cấu hình ban đầu

esp_err_t configure_i2s_for_microphone(); // Khai báo hàm cấu hình I2S cho mic, hàm này sẽ được gọi khi chuyển sang chế độ MIC, sửa cấu hình I2S cho mic 32-bit

esp_err_t configure_i2s_for_speaker(); // Khai báo hàm cấu hình I2S cho loa, hàm này sẽ được gọi khi chuyển sang chế độ SPEAKER, sửa cấu hình I2S cho loa 16-bit

void recordAndSendTask(void *parameter); // Khai báo hàm ghi âm và gửi đi, hàm này sẽ được gọi khi nhấn nút ghi âm, sẽ tạo ra 1 task mới để ghi âm và gửi dữ liệu đi qua WebSocket

void playAudioTask(void *parameter); // Khai báo hàm phát âm thanh, hàm này sẽ được gọi khi nhận được dữ liệu âm thanh từ WebSocket, sẽ tạo ra 1 task mới để phát âm thanh

// --- Hàm xử lý sự kiện WebSocket ---
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.println("[WSc] Disconnected!");
    digitalWrite(LED_PLAY, LOW);
    digitalWrite(LED_RECORD, LOW);

    // Xử lý ngắt kết nối - đảm bảo tắt ghi âm nếu đang ghi
    isRecording = false;

    // Đặt về chế độ NONE khi mất kết nối
    switch_i2s_mode(I2S_MODE_IDLE);

    // Xóa queue để tránh phát lại dữ liệu cũ khi kết nối lại
    if (playAudioQueue != NULL)
    {
      // Xóa sạch các chunk trong queue để tránh rò rỉ bộ nhớ
      AudioChunk *chunk = NULL;
      while (xQueueReceive(playAudioQueue, &chunk, 0) == pdTRUE) // Hàm xQueueReceive() trong FreeRTOS là hàm để nhận dữ liệu từ một hàng đợi
      {
        if (chunk)
        {
          if (chunk->data)
            free(chunk->data);
          free(chunk);
        }
      }
      xQueueReset(playAudioQueue); // Đặt lại hàng đợi để xóa tất cả các chunk trong hàng đợi
    }
    break;

  case WStype_CONNECTED:
    Serial.printf("[WSc] Connected to url: %s\n", payload);
    break;

  case WStype_TEXT:                                         // Nhận dữ liệu dạng văn bản (lệnh) từ WebSocket
    if (strcmp((char *)payload, "AUDIO_STREAM_START") == 0) // Lệnh báo bắt đàu phát âm thanh
    {
      digitalWrite(LED_PLAY, HIGH);
    }
    else if (strcmp((char *)payload, "AUDIO_STREAM_END") == 0) // Lệnh báo phát hết âm thanh
    {
      digitalWrite(LED_PLAY, LOW);
    }
    break;

  case WStype_BIN: // Nhận dữ liệu dạng nhị phân (file âm thanh) từ WebSocket, dữ liệu nhận được là 1 file âm thanh .wav, cần bỏ header WAV đi
    if (playAudioQueue != NULL && length > 0)
    {
      static bool firstChunk = true; // Biến để theo dõi chunk đầu tiên, dùng để kiểm tra header WAV, nếu là true thì sẽ kiểm tra header WAV, nếu là false thì sẽ bỏ qua header WAV
      uint8_t *audioData = payload;
      size_t audioLength = length;

      // Kiểm tra header WAV ở chunk đầu tiên
      if (firstChunk && length > 44 && payload[0] == 'R' && payload[1] == 'I' && payload[2] == 'F' && payload[3] == 'F')
      {
        // Kiểm tra thông tin WAV header
        uint16_t bits_per_sample = *(uint16_t *)&payload[34];
        uint32_t sample_rate = *(uint32_t *)&payload[24];
        uint16_t channels = *(uint16_t *)&payload[22];

        if (bits_per_sample != 16 || sample_rate != I2S_SAMPLE_RATE || channels != 1)
        {
          Serial.println("WAV data is not in the correct format (require 16-bit, 16kHz, mono)!");
          Serial.printf("WAV Header - Sample Rate: %u Hz, Bits per Sample: %u, Channels: %u\n", sample_rate, bits_per_sample, channels);
          firstChunk = false; // Đánh dấu đã nhận header WAV không hợp lệ
          break;
        }
        // Bỏ qua header WAV (44 byte) và lấy dữ liệu âm thanh
        audioData = payload + 44;
        audioLength = length - 44;
        firstChunk = false; // Đánh dấu đã nhận header WAV
      }
      else if (firstChunk) // Chunk đầu tiên nhưng không phải WAV header
      {
        firstChunk = false;
      }

      // Cấp phát bộ nhớ cho cấu trúc AudioChunk
      AudioChunk *chunk = (AudioChunk *)malloc(sizeof(AudioChunk));
      if (!chunk)
      {
        Serial.println("Cannot allocate memory for AudioChunk!");
        break;
      }

      // Cấp phát bộ nhớ cho dữ liệu âm thanh và sao chép payload
      chunk->data = (uint8_t *)malloc(audioLength);
      if (!chunk->data)
      {
        Serial.println("Cannot allocate memory for chunk->data!");
        free(chunk);
        break;
      }

      memcpy(chunk->data, audioData, audioLength);
      chunk->length = audioLength;

      // Gửi vào queue để phát, tăng timeout để tránh bỏ qua chunk
      if (xQueueSend(playAudioQueue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) // pdMS_TO_TICKS(100) là thời gian timeout
      {
        Serial.println("Play audio queue full! Delete chunk.");
        free(chunk->data);
        free(chunk);
      }
    }
    break;
  // ... các case khác ...
  case WStype_ERROR:
  case WStype_FRAGMENT_TEXT_START:
  case WStype_FRAGMENT_BIN_START:
  case WStype_FRAGMENT:
  case WStype_FRAGMENT_FIN:
  case WStype_PING:
  case WStype_PONG:
    break;
  }
}

// --- Hàm chuyển đổi chế độ I2S loa 16bit và mic 32 bit ---
esp_err_t switch_i2s_mode(current_i2s_mode_t new_mode)
{
  if (xSemaphoreTake(i2s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
  {
    Serial.println("Failed to get I2S mutex. Cannot switch mode.");
    return ESP_FAIL;
  }

  // Nếu chế độ mới giống chế độ hiện tại, không cần làm gì
  if (current_i2s_mode == new_mode)
  {
    xSemaphoreGive(i2s_mutex);
    return ESP_OK;
  }

  // Cấu hình I2S theo chế độ mới
  esp_err_t result = ESP_OK;

  // Xử lý chuyển mode I2S ( loa - mic )
  switch (new_mode)
  {
  case I2S_MODE_MIC:
    result = configure_i2s_for_microphone();
    if (result == ESP_OK)
    {
      current_i2s_mode = I2S_MODE_MIC;
    }
    break;

  case I2S_MODE_SPEAKER:
    result = configure_i2s_for_speaker();
    if (result == ESP_OK)
    {
      current_i2s_mode = I2S_MODE_SPEAKER;
    }
    break;

  case I2S_MODE_IDLE:
    i2s_stop(I2S_PORT);
    i2s_zero_dma_buffer(I2S_PORT);
    digitalWrite(SPEAKER_SD_PIN, LOW);
    current_i2s_mode = I2S_MODE_IDLE;
    break;

  default:
    result = ESP_FAIL;
  }

  xSemaphoreGive(i2s_mutex);
  return result;
}

// Cài đặt I2S driver và cấu hình ban đầu, sau đó sẽ dùng các hàm configure_i2s_for_microphone() và configure_i2s_for_speaker() để thay đổi chế độ I2S
esp_err_t i2s_init()
{

  // Khởi tạo i2s config
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_RX, // Start with 32-bit (will change when needed)
      .channel_format = MIC_CHANNEL_FMT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = I2S_BUFFER_COUNT,
      .dma_buf_len = I2S_READ_LEN / I2S_BUFFER_COUNT,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0};

  // Nạp I2S driver
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to install I2S driver. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Bắt đầu ở None mode
  current_i2s_mode = I2S_MODE_IDLE;

  return ESP_OK;
}

// Cấu hình I2S cho Mic 32bit mà không cần uninstall driver
esp_err_t configure_i2s_for_microphone()
{

  // Dừng I2S
  esp_err_t err = i2s_stop(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to stop I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Tắt loa
  digitalWrite(SPEAKER_SD_PIN, LOW);

  // Cấu hình chân I2S cho mic
  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = -1, // Không dùng output khi thu âm
      .data_in_num = I2S_SD_IN};

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S pins for microphone. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Set I2S clock for microphone (32-bit)
  err = i2s_set_clk(I2S_PORT, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_RX, I2S_CHANNEL_MONO);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S clock for microphone. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Reset DMA buffer
  err = i2s_zero_dma_buffer(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to zero DMA buffer. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Start I2S again
  err = i2s_start(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to start I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}

// Cấu hình I2S cho loa 16bit mà không cần uninstall driver
esp_err_t configure_i2s_for_speaker()
{

  // Stop I2S
  esp_err_t err = i2s_stop(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to stop I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Configure pins for speaker
  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = I2S_SD_OUT,
      .data_in_num = -1 // Dùng loa thì không dùng input
  };

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S pins for speaker. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Set I2S clock for speaker (16-bit)
  err = i2s_set_clk(I2S_PORT, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_TX, I2S_CHANNEL_MONO);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S clock for speaker. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Reset DMA buffer
  err = i2s_zero_dma_buffer(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to zero DMA buffer. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Turn on speaker
  digitalWrite(SPEAKER_SD_PIN, HIGH);

  // Start I2S again
  err = i2s_start(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to start I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}

// --- Task ghi âm và gửi đi ---
void recordAndSendTask(void *parameter)
{
  Serial.println("Recording task started...");

  // Chuyển I2S sang chế độ mic
  if (switch_i2s_mode(I2S_MODE_MIC) != ESP_OK)
  {
    Serial.println("Failed to configure I2S for recording. Exiting task.");
    isRecording = false;
    recordTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int32_t *i2s_read_buffer = NULL;
  int16_t *pcm_send_buffer = NULL;

  size_t bytes_read = 0;
  size_t bytes_to_send = 0;

  // Cấp phát bộ đệm đọc (32-bit)
  i2s_read_buffer = (int32_t *)malloc(I2S_READ_LEN);
  if (!i2s_read_buffer)
  {
    Serial.println("Failed to allocate memory for I2S read buffer!");
    goto cleanup;
  }

  // Cấp phát bộ đệm gửi (16-bit)
  pcm_send_buffer = (int16_t *)malloc(I2S_READ_LEN / 2);
  if (!pcm_send_buffer)
  {
    Serial.println("Failed to allocate memory for PCM send buffer!");
    goto cleanup;
  }

  recordingStartTime = millis();
  Serial.println("Start Recording for 5 seconds...");
  digitalWrite(LED_RECORD, HIGH);

  // Ghi âm trong khoảng thời gian cố định là 5 giây
  while (isRecording && (millis() - recordingStartTime < RECORD_DURATION_MS))
  {
    esp_err_t read_result = i2s_read(I2S_PORT, i2s_read_buffer, I2S_READ_LEN, &bytes_read, pdMS_TO_TICKS(1000));

    if (read_result == ESP_OK && bytes_read > 0)
    {
      // Phần xử lý âm thanh giữ nguyên
      int samples_read = bytes_read / 4;
      bytes_to_send = samples_read * 2;

      for (int i = 0; i < samples_read; i++)
      {
        int32_t sample = i2s_read_buffer[i] >> 16;
        sample = (int32_t)(sample * MIC_GAIN_FACTOR);

        if (sample > 32767)
          sample = 32767;
        if (sample < -32768)
          sample = -32768;

        pcm_send_buffer[i] = (int16_t)sample;
      }

      if (webSocket.isConnected())
      {
        if (!webSocket.sendBIN((uint8_t *)pcm_send_buffer, bytes_to_send))
        {
          Serial.println("WebSocket sendBIN failed!");
        }
      }
      else
      {
        Serial.println("WebSocket disconnected during recording.");
      }
    }
    else if (read_result == ESP_ERR_TIMEOUT)
    {
      Serial.println("I2S Read Timeout!");
    }
    else
    {
      Serial.printf("I2S Read Error: %d (%s)\n", read_result, esp_err_to_name(read_result));
      isRecording = false;
    }
  }

cleanup:
  // Rest of cleanup code stays the same
  Serial.println("Recording finished after 5 seconds.");
  digitalWrite(LED_RECORD, LOW);
  isRecording = false;

  if (i2s_read_buffer)
    free(i2s_read_buffer);
  if (pcm_send_buffer)
    free(pcm_send_buffer);

  if (webSocket.isConnected())
  {
    webSocket.sendTXT("END_OF_STREAM");
    Serial.println("Sent END_OF_STREAM");
  }

  // Sau khi kết thúc ghi âm, chuyển về chế độ NONE để chuẩn bị cho lần tiếp theo
  switch_i2s_mode(I2S_MODE_IDLE);

  recordTaskHandle = NULL;
  vTaskDelete(NULL);
}

// --- Task phát âm thanh ---
void playAudioTask(void *parameter)
{
  AudioChunk *chunk = NULL;
  size_t bytes_written = 0;
  bool speaker_mode_active = false;
  unsigned long lastAudioTime = 0;   // Thời gian nhận audio gần nhất, nếu không nhận được âm thanh trong 1.5 giây thì sẽ chuyển về chế độ I2S IDLE
  unsigned long lastMemoryCheck = 0; // Thời gian kiểm tra bộ nhớ gần nhất, để quản lý bộ nhớ

  while (true)
  {
    // Kiểm tra trạng thái bộ nhớ mỗi 2 giây
    if (millis() - lastMemoryCheck > 2000)
    {
      uint32_t freeHeap = ESP.getFreeHeap(); // Lấy dung lượng bộ nhớ còn trống
      lastMemoryCheck = millis();

      // Nếu bộ nhớ còn lại dưới 20KB, thực hiện dọn dẹp bộ nhớ
      if (freeHeap < 20000)
      {
        Serial.println("Low memory detected! Forcing cleanup...");

        // Giải phóng bộ nhớ cho các chunk trong queue
        // Tạo một chunk tạm thời để nhận dữ liệu từ queue
        AudioChunk *tempChunk = NULL;
        int cleared = 0;

        // Keep at least 2 chunks for smooth playAudio, clear the rest
        int queueSize = uxQueueMessagesWaiting(playAudioQueue);
        if (queueSize > 2)
        {
          int toClear = queueSize - 2;
          Serial.printf("Queue too large (%d items), clearing %d items\n", queueSize, toClear);

          for (int i = 0; i < toClear; i++)
          {
            if (xQueueReceive(playAudioQueue, &tempChunk, 0) == pdTRUE)
            {
              if (tempChunk)
              {
                if (tempChunk->data)
                  free(tempChunk->data);
                free(tempChunk);
                cleared++;
              }
            }
          }
          Serial.printf("Cleared %d audio chunks from queue\n", cleared);
        }

        // Nếu bộ nhớ còn rất thấp, reset toàn bộ hệ thống âm thanh, chuyển sang chế độ I2S IDLE
        if (freeHeap < 10000)
        {
          Serial.println("CRITICAL MEMORY SHORTAGE! Resetting audio system...");

          // Duyệt và giải phóng TẤT CẢ các chunk trong queue trước khi reset
          AudioChunk *tempChunkToClear = NULL;
          int cleared_critical = 0;
          // Lặp cho đến khi queue rỗng
          while (xQueueReceive(playAudioQueue, &tempChunkToClear, 0) == pdTRUE)
          {
            if (tempChunkToClear)
            {
              if (tempChunkToClear->data)
                free(tempChunkToClear->data);
              free(tempChunkToClear);
              cleared_critical++;
            }
          }
          Serial.printf("Cleared %d audio chunks during critical memory cleanup.\n", cleared_critical);

          switch_i2s_mode(I2S_MODE_IDLE);
          digitalWrite(SPEAKER_SD_PIN, LOW); // Tắt loa
          speaker_mode_active = false;       // Đặt lại trạng thái loa

          delay(100);
        }
      }
    }

    if (xQueueReceive(playAudioQueue, &chunk, pdMS_TO_TICKS(1000)) == pdPASS)
    {
      // Có âm thanh cần phát - Cập nhật thời gian nhận audio gần nhất
      lastAudioTime = millis();

      if (chunk && chunk->data && chunk->length > 0)
      {
        // Chỉ chuyển sang chế độ loa khi cần phát âm thanh
        if (!speaker_mode_active)
        {
          if (switch_i2s_mode(I2S_MODE_SPEAKER) != ESP_OK)
          {
            Serial.println("Failed to configure I2S for playAudio. Skipping chunk.");
            free(chunk->data);
            free(chunk);
            chunk = NULL;
            continue;
          }
          speaker_mode_active = true;
        }

        // --- TĂNG GIẢM ÂM LƯỢNG ---
        // Dữ liệu là 16-bit PCM (2 byte mỗi mẫu)
        // int16_t *audio_samples = (int16_t *)chunk->data;
        // int samples_count = chunk->length / 2; // Số lượng mẫu 16-bit
        // // Điều chỉnh biên độ của mỗi mẫu âm thanh
        // for (int i = 0; i < samples_count; i++)
        // {
        //   audio_samples[i] = audio_samples[i] * 1; // Tăng giảm hệ số
        // }

        // Ghi dữ liệu ra I2S
        esp_err_t write_result = i2s_write(I2S_PORT, chunk->data, chunk->length, &bytes_written, pdMS_TO_TICKS(1000));
        if (write_result != ESP_OK)
        {
          Serial.printf("Lỗi ghi I2S: %d (%s), độ dài chunk: %d\n",
                        write_result, esp_err_to_name(write_result), chunk->length);
        }
        else if (bytes_written != chunk->length)
        {
          Serial.printf("Chỉ ghi được %d/%d bytes\n", bytes_written, chunk->length);
        }

        // Giải phóng bộ nhớ
        free(chunk->data);
        free(chunk);
        chunk = NULL;
      }
      else
      {
        Serial.println("Nhận được chunk không hợp lệ");
        if (chunk)
          free(chunk);
        chunk = NULL;
      }
    }
    else
    {
      // Không có dữ liệu nhận được trong 1 giây
      // Nếu đang ở chế độ speaker và đã 1.5 giây không nhận được âm thanh mới,
      // thì chuyển về trạng thái ban đầu để sẵn sàng cho ghi âm tiếp
      if (speaker_mode_active && (millis() - lastAudioTime > 1500))
      {
        speaker_mode_active = false;
        // Tắt loa và về trạng thái NONE để sẵn sàng ghi âm tiếp
        switch_i2s_mode(I2S_MODE_IDLE);
        digitalWrite(SPEAKER_SD_PIN, LOW); // Tắt loa
        // Xóa queue để đảm bảo không còn dữ liệu cũ
        xQueueReset(playAudioQueue);
      }
    }
  }
}

// --- Setup ---
void setup()
{
  Serial.begin(115200);

  // Cấu hình chân nút nhấn
  pinMode(PIN_BUTTON, INPUT_PULLUP); // để ở pullup vì nút nhấn nối đất, mặc định khi không nhấn là HIGH

  // Cấu hình các chân LED
  pinMode(LED_RECORD, OUTPUT);
  digitalWrite(LED_RECORD, LOW);
  pinMode(LED_PLAY, OUTPUT);
  digitalWrite(LED_PLAY, LOW);

  // Thêm cấu hình cho chân Shutdown của loa
  pinMode(SPEAKER_SD_PIN, OUTPUT);
  digitalWrite(SPEAKER_SD_PIN, LOW); // Mặc định tắt loa khi khởi động

  // Tạo mutex cho I2S, tránh việc đồng thời truy cập vào I2S driver từ nhiều task khác nhau
  // Mutex là một cơ chế đồng bộ hóa trong FreeRTOS, giúp đảm bảo rằng chỉ một task có thể truy cập vào một tài nguyên tại một thời điểm
  i2s_mutex = xSemaphoreCreateMutex();
  if (i2s_mutex == NULL)
  {
    Serial.println("Failed to create I2S mutex. Halting.");
    while (1)
      ;
  }

  // Khởi tạo I2S driver lần đầu tiên, các lần tiếp theo sẽ không cần gọi lại hàm này, chỉ chỉnh sửa các chế độ I2S
  // Chỉ cần gọi hàm này một lần trong setup, sau đó sẽ dùng các hàm configure_i2s_for_microphone() và configure_i2s_for_speaker() để thay đổi chế độ I2S
  if (i2s_init() != ESP_OK)
  {
    Serial.println("Failed to initialize I2S driver. Halting.");
    while (1)
      ;
  }

  // Kết nối WiFi - nên chuyển sang dùng WiFiManager để dễ dàng hơn
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Tạo hàng đợi Playback chứa con trỏ tới AudioChunk*
  playAudioQueue = xQueueCreate(16, sizeof(AudioChunk *)); // Tạo hàng đợi với kích thước 16 chunk
  if (playAudioQueue == NULL)
  {
    Serial.println("Failed to create playAudio queue. Halting.");
    while (1)
      ;
  }

  // Khởi tạo WebSocket
  // websockets_server_host = WiFi.localIP().toString(); // Lấy địa chỉ IP của server kết nối chung Wifi
  webSocket.begin(websockets_server_host, websockets_server_port, websockets_path);
  webSocket.onEvent(webSocketEvent);         // Đăng ký hàm xử lý sự kiện WebSocket
  webSocket.setReconnectInterval(5000);      // Thời gian thử lại kết nối nếu bị mất
  webSocket.enableHeartbeat(10000, 2000, 3); // Thời gian heartbeat 10 giây, timeout 2 giây, thử lại 3 lần, heartbeat sẽ tự động gửi ping/pong để kiểm tra kết nối

  // Chip ESP32 đa nhân, Sử dụng hàm này để tạo 1 task và ghim nó vào core 0, tránh trường hợp xung đột với các task khác
  // Hàm trong thư viện FreeRTOS, là một thư viện hệ điều hành thời gian thực cho ESP32, có thể tạo nhiều task chạy song song
  xTaskCreatePinnedToCore(
      playAudioTask, // Tên hàm xử lý task
      "PlaybackTask",
      8192, // Kích thước stack cho task (8192 bytes) 8KB
      NULL,
      5,                   // Mức độ ưu tiên của task (5 là mức độ ưu tiên trung bình)
      &playAudioTaskHandle, // Nếu tạo thành công task thì con trỏ này sẽ trỏ tới task đó
      0);                  // Core 0 (core 1 là core 1)
  if (playAudioTaskHandle == NULL)
  {
    Serial.println("Failed to create playAudio task. Halting.");
    while (1)
      ;
  }

  Serial.println("Press the button to start recording for 5 seconds.");
}

// --- Loop ---
void loop()
{
  webSocket.loop();

  // Kiểm tra trạng thái nút nhấn với chống dội
  bool currentButtonState = digitalRead(PIN_BUTTON) == LOW; // LOW khi nhấn (pull-up)
  currentTime = millis();                                   // Biến theo dõi thời gian hiện tại

  // Xử lý thay đổi trạng thái nút nhấn với chống dội
  if (currentTime - lastButtonChangeTime > DEBOUNCE_TIME)
  {
    // Phát hiện nhấn nút - bắt đầu ghi âm 5 giây
    if (currentButtonState && !buttonPressed && !isRecording && webSocket.isConnected())
    {
      buttonPressed = true;
      lastButtonChangeTime = currentTime;

      // Đảm bảo không có chế độ I2S nào đang hoạt động trước khi bắt đầu ghi âm
      if (current_i2s_mode != I2S_MODE_IDLE)
      {
        switch_i2s_mode(I2S_MODE_IDLE);
        delay(100); // Cho phép hệ thống ổn định
      }

      Serial.println("Button pressed. Starting recording for 5 seconds...");
      isRecording = true;

      // Nếu task ghi âm chưa được tạo, tạo task mới
      if (recordTaskHandle == NULL)
      {
        // Chip ESP32 đa nhân, Sử dụng hàm này để tạo 1 task và ghim nó vào core 1, tránh trường hợp xung đột với các task khác
        // Hàm trong thư viện FreeRTOS, là một thư viện hệ điều hành thời gian thực cho ESP32, có thể tạo nhiều task chạy song song
        xTaskCreatePinnedToCore(
            recordAndSendTask,
            "RecordSendTask",
            8192, // Kích thước stack cho task (8192 bytes) 8KB
            NULL,
            10, // Mức độ ưu tiên của task (10 là mức độ ưu tiên cao hơn)
            &recordTaskHandle,
            1); // Khởi tạo task tại core 1
        if (recordTaskHandle == NULL)
        {
          Serial.println("Failed to create recording task!");
          isRecording = false;
        }
      }
    }

    // Đặt lại trạng thái nút khi thả
    if (!currentButtonState && buttonPressed)
    {
      buttonPressed = false;
      lastButtonChangeTime = currentTime;
    }
  }
}
