#include <WiFi.h>
#include <WebSocketsClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"  // Thêm thư viện Queue
#include "driver/i2s.h"
#include "esp_idf_version.h"  // Để kiểm tra phiên bản IDF (cho i2s_driver_install)

// --- Cấu hình WiFi ---
const char* ssid = "B10509_2.4G";
const char* password = "509509509";

// --- Cấu hình Server ---
const char* websockets_server_host = "192.168.1.22";
const uint16_t websockets_server_port = 8080;
const char* websockets_path = "/";

// --- Cấu hình I2S ---
#define I2S_WS 15
#define I2S_SD_IN 32
#define I2S_SD_OUT 25  // !!! CHÂN MỚI: Chân Serial Data OUT (Tới MAX98357A) !!! - Chọn chân phù hợp
#define I2S_SCK 14
#define SPEAKER_SD_PIN 26 // Chân Shutdown cho loa MAX98357A
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE (16000)
#define I2S_BITS_PER_SAMPLE_RX I2S_BITS_PER_SAMPLE_32BIT
#define I2S_READ_LEN (1024 * 4)
#define I2S_BITS_PER_SAMPLE_TX I2S_BITS_PER_SAMPLE_16BIT
#define SPEAKER_CHANNEL_FMT I2S_CHANNEL_FMT_ONLY_LEFT
#define MIC_CHANNEL_FMT I2S_CHANNEL_FMT_ONLY_LEFT
#define I2S_BUFFER_COUNT 8

// --- Cấu hình Ghi âm ---
#define RECORD_DURATION_MS 5000

// --- Cấu hình Playback ---
#define PLAYBACK_QUEUE_LENGTH 10
#define PLAYBACK_QUEUE_ITEM_SIZE (I2S_READ_LEN / 2)  // Kích thước tối đa ước tính
#define PLAYBACK_TASK_STACK_SIZE 4096

#define LED_RECORD 17
#define LED_PLAY 16  // !!! LED MỚI: LED báo đang phát (Tùy chọn) !!! - Chọn chân phù hợp
#define MIC_GAIN_FACTOR 4.0  // Hệ số khuếch đại microphone, điều chỉnh theo nhu cầu

// *** SỬA 1: Di chuyển định nghĩa AudioChunk lên đây ***
// Định nghĩa cấu trúc để gửi vào Queue Playback
typedef struct {
  uint8_t* data;
  size_t length;
} AudioChunk;

// --- Theo dõi trạng thái I2S hiện tại ---
typedef enum {
  I2S_MODE_NONE,
  I2S_MODE_MIC,
  I2S_MODE_SPEAKER
} current_i2s_mode_t;

// Biến để theo dõi chế độ I2S hiện tại
volatile current_i2s_mode_t current_i2s_mode = I2S_MODE_NONE;

// Mutex để bảo vệ việc thay đổi cấu hình I2S
SemaphoreHandle_t i2s_mutex = NULL;

// --- Biến Global ---
WebSocketsClient webSocket;
bool isRecording = false;
unsigned long recordingStartTime = 0;
TaskHandle_t recordTaskHandle = NULL;
TaskHandle_t playbackTaskHandle = NULL;
QueueHandle_t playbackQueue = NULL;

// --- Hàm xử lý WebSocket ---
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WSc] Disconnected!");
      digitalWrite(LED_PLAY, LOW);
      // Cân nhắc việc xóa queue playback khi mất kết nối để tránh phát dữ liệu cũ
      if (playbackQueue != NULL) {
        xQueueReset(playbackQueue);  // Xóa sạch queue
                                     // Lưu ý: Việc này không giải phóng bộ nhớ của các chunk đã cấp phát trong queue!
                                     // Để giải phóng bộ nhớ, cần lặp qua queue và free trước khi reset, hoặc
                                     // chấp nhận rò rỉ nhỏ khi disconnect đột ngột. Reset là cách đơn giản nhất.
      }
      break;
    case WStype_CONNECTED:
      Serial.printf("[WSc] Connected to url: %s\n", payload);
      break;
    case WStype_TEXT:
      Serial.printf("[WSc] Received text: %s\n", payload);
      if (strcmp((char*)payload, "AUDIO_STREAM_START") == 0) {
        digitalWrite(LED_PLAY, HIGH);
        Serial.println("Playback starting command received.");
      } else if (strcmp((char*)payload, "AUDIO_STREAM_END") == 0) {
        digitalWrite(LED_PLAY, LOW);
        Serial.println("Playback ending command received.");
      } else {
        // This is regular text (like Gemini response)
        Serial.println("Received text response:");
        Serial.println((char*)payload);
      }
      break;
    case WStype_BIN:
      if (playbackQueue != NULL && length > 0) {
        static bool firstChunk = true;
        uint8_t* audioData = payload;
        size_t audioLength = length;

        // Kiểm tra header WAV ở chunk đầu tiên
        if (firstChunk && length > 44 && payload[0] == 'R' && payload[1] == 'I' && payload[2] == 'F' && payload[3] == 'F') {
          // Kiểm tra thông tin WAV header
          uint16_t bits_per_sample = *(uint16_t*)&payload[34];
          uint32_t sample_rate = *(uint32_t*)&payload[24];
          uint16_t channels = *(uint16_t*)&payload[22];
          Serial.printf("WAV Header - Sample Rate: %u Hz, Bits per Sample: %u, Channels: %u\n",
                        sample_rate, bits_per_sample, channels);
          if (bits_per_sample != 16 || sample_rate != I2S_SAMPLE_RATE || channels != 1) {
            Serial.println("Dữ liệu WAV không đúng định dạng (yêu cầu 16-bit, 16kHz, mono)!");
            firstChunk = false;
            break;
          }
          Serial.println("Phát hiện header WAV, bỏ qua 44 bytes đầu tiên");
          audioData = payload + 44;
          audioLength = length - 44;
          firstChunk = false;
        } else if (firstChunk) {
          // Chunk đầu tiên nhưng không phải WAV header
          firstChunk = false;
        }

        // Cấp phát bộ nhớ cho cấu trúc AudioChunk
        AudioChunk* chunk = (AudioChunk*)malloc(sizeof(AudioChunk));
        if (!chunk) {
          Serial.println("Không thể cấp phát bộ nhớ cho AudioChunk!");
          break;
        }

        // Cấp phát bộ nhớ cho dữ liệu âm thanh và sao chép payload
        chunk->data = (uint8_t*)malloc(audioLength);
        if (!chunk->data) {
          Serial.println("Không thể cấp phát bộ nhớ cho dữ liệu âm thanh!");
          free(chunk);
          break;
        }

        memcpy(chunk->data, audioData, audioLength);
        chunk->length = audioLength;

        // Gửi vào queue để phát, tăng timeout để tránh bỏ qua chunk
        if (xQueueSend(playbackQueue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
          Serial.println("Hàng đợi phát lại đầy! Loại bỏ chunk.");
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

// --- Hàm cấu hình I2S ---
esp_err_t i2s_install() {
  // (Giữ nguyên nội dung hàm i2s_install như trước)
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_RX,  // Đặt là 32bit để đọc INMP441
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
#else
    .channel_format = MIC_CHANNEL_FMT,
#endif
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_BUFFER_COUNT,
    .dma_buf_len = I2S_READ_LEN / I2S_BUFFER_COUNT,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to install I2S driver. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }
  Serial.println("I2S Driver installed.");

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_SD_OUT,
    .data_in_num = I2S_SD_IN
  };

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S pins. Error: %d (%s)\n", err, esp_err_to_name(err));
    i2s_driver_uninstall(I2S_PORT);
    return err;
  }
  Serial.println("I2S Pins configured.");

  err = i2s_set_sample_rates(I2S_PORT, I2S_SAMPLE_RATE);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S sample rate. Error: %d (%s)\n", err, esp_err_to_name(err));
    i2s_driver_uninstall(I2S_PORT);
    return err;
  }
  Serial.println("I2S Sample Rate set.");

  i2s_zero_dma_buffer(I2S_PORT);

  return ESP_OK;
}

// --- Hàm hủy cài đặt I2S ---
esp_err_t i2s_uninstall() {
  esp_err_t err = i2s_driver_uninstall(I2S_PORT);
  if (err != ESP_OK) {
    Serial.printf("Failed to uninstall I2S driver. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }
  Serial.println("I2S Driver uninstalled.");
  return ESP_OK;
}

// --- Hàm cấu hình I2S cho microphone (32-bit) ---
esp_err_t i2s_install_mic() {

  // Tắt loa khi chuyển sang chế độ microphone
  digitalWrite(SPEAKER_SD_PIN, LOW);  // Đặt chân SD ở mức LOW để tắt loa

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_RX,  // 32-bit cho INMP441
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
#else
    .channel_format = MIC_CHANNEL_FMT,
#endif
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_BUFFER_COUNT,
    .dma_buf_len = I2S_READ_LEN / I2S_BUFFER_COUNT,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to install I2S driver for mic. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }
  Serial.println("I2S Driver for mic installed.");

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,  // Không dùng output khi thu âm
    .data_in_num = I2S_SD_IN
  };

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S pins for mic. Error: %d (%s)\n", err, esp_err_to_name(err));
    i2s_driver_uninstall(I2S_PORT);
    return err;
  }
  Serial.println("I2S Pins for mic configured.");

  i2s_zero_dma_buffer(I2S_PORT);
  return ESP_OK;
}

// --- Hàm cấu hình I2S cho loa (16-bit) ---
esp_err_t i2s_install_speaker() {

  // Bật loa khi chuyển sang chế độ speaker
  digitalWrite(SPEAKER_SD_PIN, HIGH);  // Đặt chân SD ở mức HIGH để bật loa
  
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_TX,  // 16-bit cho MAX98357A
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
#else
    .channel_format = SPEAKER_CHANNEL_FMT,
#endif
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_BUFFER_COUNT,
    .dma_buf_len = I2S_READ_LEN / I2S_BUFFER_COUNT,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to install I2S driver for speaker. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }
  Serial.println("I2S Driver for speaker installed.");

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_SD_OUT,
    .data_in_num = -1  // Không dùng input khi phát âm thanh
  };

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S pins for speaker. Error: %d (%s)\n", err, esp_err_to_name(err));
    i2s_driver_uninstall(I2S_PORT);
    return err;
  }
  Serial.println("I2S Pins for speaker configured.");

  i2s_zero_dma_buffer(I2S_PORT);
  return ESP_OK;
}

// --- Hàm chuyển đổi chế độ I2S ---
esp_err_t switch_i2s_mode(current_i2s_mode_t new_mode) {
  if (xSemaphoreTake(i2s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("Failed to get I2S mutex. Cannot switch mode.");
    return ESP_FAIL;
  }
  
  // Nếu chế độ mới giống chế độ hiện tại, không cần làm gì
  if (current_i2s_mode == new_mode) {
    xSemaphoreGive(i2s_mutex);
    return ESP_OK;
  }
  
  // Hủy cài đặt I2S hiện tại (nếu có)
  if (current_i2s_mode != I2S_MODE_NONE) {
    esp_err_t err = i2s_uninstall();
    if (err != ESP_OK) {
      xSemaphoreGive(i2s_mutex);
      return err;
    }
  }
  
  // Cấu hình I2S theo chế độ mới
  esp_err_t result = ESP_OK;
  switch (new_mode) {
    case I2S_MODE_MIC:
      result = i2s_install_mic();
      if (result == ESP_OK) {
        current_i2s_mode = I2S_MODE_MIC;
        Serial.println("I2S switched to microphone mode (32-bit)");
      }
      break;
      
    case I2S_MODE_SPEAKER:
      result = i2s_install_speaker();
      if (result == ESP_OK) {
        current_i2s_mode = I2S_MODE_SPEAKER;
        Serial.println("I2S switched to speaker mode (16-bit)");
      }
      break;
      
    case I2S_MODE_NONE:
      current_i2s_mode = I2S_MODE_NONE;
      Serial.println("I2S switched to none mode");
      break;
      
    default:
      Serial.println("Unknown I2S mode requested");
      result = ESP_FAIL;
  }
  
  xSemaphoreGive(i2s_mutex);
  return result;
}

// --- Task ghi âm và gửi đi ---
void recordAndSendTask(void* parameter) {
  Serial.println("Recording task started...");
  
  // Chuyển I2S sang chế độ mic
  if (switch_i2s_mode(I2S_MODE_MIC) != ESP_OK) {
    Serial.println("Failed to configure I2S for recording. Exiting task.");
    isRecording = false;
    recordTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }
  
  int32_t* i2s_read_buffer = NULL;
  int16_t* pcm_send_buffer = NULL;

  // *** SỬA 2: Di chuyển khai báo biến lên đây ***
  size_t bytes_read = 0;
  size_t bytes_to_send = 0;

  // Cấp phát bộ đệm đọc (32-bit)
  i2s_read_buffer = (int32_t*)malloc(I2S_READ_LEN);
  if (!i2s_read_buffer) {
    Serial.println("Failed to allocate memory for I2S read buffer!");
    goto cleanup;  // Giờ nhảy sẽ không cắt ngang khởi tạo bytes_read/bytes_to_send
  }

  // Cấp phát bộ đệm gửi (16-bit)
  pcm_send_buffer = (int16_t*)malloc(I2S_READ_LEN / 2);
  if (!pcm_send_buffer) {
    Serial.println("Failed to allocate memory for PCM send buffer!");
    goto cleanup;  // Giờ nhảy sẽ không cắt ngang khởi tạo bytes_read/bytes_to_send
  }

  recordingStartTime = millis();
  Serial.println("Start Recording...");
  digitalWrite(LED_RECORD, HIGH);

  while (isRecording && (millis() - recordingStartTime < RECORD_DURATION_MS)) {
    esp_err_t read_result = i2s_read(I2S_PORT, i2s_read_buffer, I2S_READ_LEN, &bytes_read, pdMS_TO_TICKS(1000));

    if (read_result == ESP_OK && bytes_read > 0) {
      int samples_read = bytes_read / 4;
      bytes_to_send = samples_read * 2;

      // Áp dụng khuếch đại khi chuyển đổi từ 32-bit sang 16-bit
      for (int i = 0; i < samples_read; i++) {
        // Lấy giá trị 16-bit (MSB từ mẫu 32-bit)
        int32_t sample = i2s_read_buffer[i] >> 16;
        
        // Áp dụng khuếch đại với bảo vệ chống clipping
        sample = (int32_t)(sample * MIC_GAIN_FACTOR);
        
        // Giới hạn giá trị trong phạm vi 16-bit signed
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        
        pcm_send_buffer[i] = (int16_t)sample;
      }

      if (webSocket.isConnected()) {
        if (!webSocket.sendBIN((uint8_t*)pcm_send_buffer, bytes_to_send)) {
          Serial.println("WebSocket sendBIN failed!");
        }
      } else {
        Serial.println("WebSocket disconnected during recording.");
      }
    } else if (read_result == ESP_ERR_TIMEOUT) {
      Serial.println("I2S Read Timeout!");
    } else {
      Serial.printf("I2S Read Error: %d (%s)\n", read_result, esp_err_to_name(read_result));
      isRecording = false;
    }
  }

cleanup:  // Nhãn để nhảy tới khi có lỗi cấp phát hoặc kết thúc
  Serial.println("Recording finished.");
  digitalWrite(LED_RECORD, LOW);
  isRecording = false;

  if (i2s_read_buffer) free(i2s_read_buffer);
  if (pcm_send_buffer) free(pcm_send_buffer);

  if (webSocket.isConnected()) {
    webSocket.sendTXT("END_OF_STREAM");
    Serial.println("Sent END_OF_STREAM");
  }

  Serial.println("Recording task exiting.");
  recordTaskHandle = NULL;
  vTaskDelete(NULL);
}


// --- Task phát âm thanh ---
void playbackTask(void* parameter) {
  Serial.println("Playback task started...");
  AudioChunk* chunk = NULL;
  size_t bytes_written = 0;
  bool speaker_mode_active = false;

  while (true) {
    if (xQueueReceive(playbackQueue, &chunk, portMAX_DELAY) == pdPASS) {
      if (chunk && chunk->data && chunk->length > 0) {
        // Chỉ chuyển sang chế độ loa khi cần phát âm thanh
        if (!speaker_mode_active) {
          if (switch_i2s_mode(I2S_MODE_SPEAKER) != ESP_OK) {
            Serial.println("Failed to configure I2S for playback. Skipping chunk.");
            free(chunk->data);
            free(chunk);
            chunk = NULL;
            continue;
          }
          speaker_mode_active = true;
          Serial.println("Speaker mode activated");
        }

        // --- GIẢM ÂM LƯỢNG 50% ---
        // Giả định dữ liệu là 16-bit PCM (2 byte mỗi mẫu)
        int16_t* audio_samples = (int16_t*)chunk->data;
        int samples_count = chunk->length / 2; // Số lượng mẫu 16-bit
        
        // Điều chỉnh biên độ của mỗi mẫu âm thanh
        for (int i = 0; i < samples_count; i++) {
          audio_samples[i] = audio_samples[i] * 0.05; // Giảm amplitude xuống 10%
        }
        
        // Ghi dữ liệu ra I2S
        esp_err_t write_result = i2s_write(I2S_PORT, chunk->data, chunk->length, &bytes_written, pdMS_TO_TICKS(1000));
        if (write_result != ESP_OK) {
          Serial.printf("Lỗi ghi I2S: %d (%s), độ dài chunk: %d\n",
                        write_result, esp_err_to_name(write_result), chunk->length);
        } else if (bytes_written != chunk->length) {
          Serial.printf("Chỉ ghi được %d/%d bytes\n", bytes_written, chunk->length);
        } else {
          Serial.printf("Played %d bytes of audio\n", bytes_written);
        }

        // Giải phóng bộ nhớ
        free(chunk->data);
        free(chunk);
        chunk = NULL;
      } else {
        Serial.println("Nhận được chunk không hợp lệ");
        if (chunk) free(chunk);
        chunk = NULL;
      }
    }
  }
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP32 Audio Record & Playback via WebSocket ---");

  pinMode(LED_RECORD, OUTPUT);
  digitalWrite(LED_RECORD, LOW);
  pinMode(LED_PLAY, OUTPUT);
  digitalWrite(LED_PLAY, LOW);

  // Thêm cấu hình cho chân Shutdown của loa
  pinMode(SPEAKER_SD_PIN, OUTPUT);
  digitalWrite(SPEAKER_SD_PIN, LOW);  // Mặc định tắt loa khi khởi động

  // Tạo mutex cho I2S
  i2s_mutex = xSemaphoreCreateMutex();
  if (i2s_mutex == NULL) {
    Serial.println("Failed to create I2S mutex. Halting.");
    while (1);
  }

  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Không khởi tạo I2S ở đây nữa, mỗi task sẽ cấu hình khi cần

  // Serial.println("Initializing I2S...");
  // if (i2s_install() != ESP_OK) {
  //   Serial.println("I2S initialization failed. Halting.");
  //   while (1)
  //     ;
  // }
  // Serial.println("I2S Initialized Successfully.");

  // Tạo hàng đợi Playback chứa con trỏ tới AudioChunk*
  playbackQueue = xQueueCreate(PLAYBACK_QUEUE_LENGTH, sizeof(AudioChunk*));
  if (playbackQueue == NULL) {
    Serial.println("Failed to create playback queue. Halting.");
    while (1)
      ;
  }
  Serial.println("Playback queue created.");

  webSocket.begin(websockets_server_host, websockets_server_port, websockets_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);

  Serial.println("Creating Playback Task...");
  xTaskCreatePinnedToCore(
    playbackTask, "PlaybackTask", PLAYBACK_TASK_STACK_SIZE, NULL, 5, &playbackTaskHandle, 0);
  if (playbackTaskHandle == NULL) {
    Serial.println("Failed to create playback task. Halting.");
    while (1)
      ;
  }
  Serial.println("Playback Task Created on Core 0.");

  Serial.println("Setup complete. Waiting for WebSocket connection and commands.");
  Serial.println("Press 's' in Serial Monitor to start recording for 5 seconds.");
}

// --- Loop ---
void loop() {
  webSocket.loop();

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 's' && !isRecording && webSocket.isConnected()) {
      if (recordTaskHandle == NULL) {
        Serial.println("Command 's' received. Starting recording...");
        isRecording = true;
        xTaskCreatePinnedToCore(
          recordAndSendTask, "RecordSendTask", 8192, NULL, 10, &recordTaskHandle, 1);
        if (recordTaskHandle == NULL) {
          Serial.println("Failed to create recording task!");
          isRecording = false;
        } else {
          Serial.println("Recording Task Created on Core 1.");
        }
      } else {
        Serial.println("Recording task is already running.");
      }
    } else if (c == 's' && !webSocket.isConnected()) {
      Serial.println("WebSocket not connected. Cannot start recording.");
    } else if (c == 's' && isRecording) {
      Serial.println("Already recording.");
    }
  }
}
