#include <WiFi.h>
#include <WebSocketsClient.h>  // Thư viện WebSocketClient cho ESP32, cung cấp các hàm để kết nối và giao tiếp với WebSocket server như begin, onEvent, sendTXT, v.v.
#include "freertos/FreeRTOS.h" // Thư viện FreeRTOS cho ESP32, cung cấp các hàm để tạo task, hàng đợi, mutex, v.v. như xTaskCreatePinnedToCore, xQueueCreate, xSemaphoreCreateMutex, v.v.
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"  // Thư viện I2S cho ESP32, cung cấp các hàm để cấu hình và sử dụng giao thức I2S như i2s_driver_install, i2s_set_pin, i2s_set_clk, v.v.
#include <WebServer.h>   // Thư viện WebServer cho ESP32, cung cấp các hàm để tạo web server như on, begin, send, v.v.
#include <DNSServer.h>   // Thư viện DNSServer cho ESP32, cung cấp các hàm để tạo DNS server như start, stop, v.v.
#include <Preferences.h> // Thư viện Preferences cho ESP32, cung cấp các hàm để lưu trữ và truy xuất dữ liệu trong bộ nhớ flash như begin, putString, getString, v.v.
#include <TFT_eSPI.h>
#include <frame.h>

TFT_eSPI tft = TFT_eSPI();

Preferences preferences; // Khởi tạo đối tượng Preferences để lưu trữ và truy xuất dữ liệu trong bộ nhớ flash

String websockets_server_host; // Cấu hình từ Web Server
const uint16_t websockets_server_port = 8080;
const char *websockets_path = "/";

String ssid;                         // SSID của mạng WiFi, cấu hình từ Web Server, lưu trong bộ nhớ flash
String password;                     // Password của mạng WiFi, cấu hình từ Web Server, lưu trong bộ nhớ flash
const char *apSSID = "Flash Bot";    // Tên mạng WiFi Access Point (AP) khi không kết nối được WiFi, tên mạng là ESP32_Config
const byte DNS_PORT = 53;            // Cổng DNS, mặc định là 53, cổng này sẽ được sử dụng để tạo DNS server cho ESP32 khi ở chế độ Access Point (AP)

WebServer server(80); // Khởi tạo WebServer trên cổng 80, cổng này sẽ được sử dụng để tạo web server cho ESP32, cổng này sẽ được sử dụng để nhận thông tin cấu hình từ người dùng
DNSServer dns;        // Đối tượng dns để tạo DNS server cho ESP32, cổng này sẽ được sử dụng để chuyển hướng tất cả các request lạ về trang cấu hình của ESP32

bool wifiConnected = false;

// --- Cấu hình I2S ---
#define I2S_WS 15         // Chân WS và LRC
#define I2S_SD_IN 32      // Chân SD mic INMP441
#define I2S_SD_OUT 25     // Chân DIN của MAX98357A
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

#define RECORD_DURATION_MS 3000 // Thời gian ghi âm, cố định 5s, sẽ nâng cấp sau

#define PIN_BUTTON 4            // Chân nút nhấn ghi âm
#define LED_RECORD 17           // LED báo đang ghi âm
#define LED_PLAY 16             // LED báo đang phát
#define PIN_RESET_WIFI_BUTTON 5 // Chân nút nhấn reset WiFi

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

bool buttonPressed = false;      // Biến để theo dõi trạng thái nút nhấn
bool resetButtonPressed = false; // Biến để theo dõi trạng thái nút nhấn reset WiFi
// Khi nhấn nút thì tín hiệu sẽ không chuyển từ Low -> High luôn mà sẽ thay đổi trạng thái từ Low -> High -> Low -> High -> Low
// Cách xử lý: Khi nhấn nút thì sẽ ghi nhận trạng thái nhấn nút, sau đó sẽ kiểm tra thời gian thay đổi trạng thái nút nhấn
// Nếu thời gian thay đổi trạng thái lớn hơn DEBOUNCE_TIME thì sẽ ghi nhận là nhấn nút
unsigned long currentTime;                   // Thời gian hiện tại, dùng để kiểm tra thời gian thay đổi trạng thái nút nhấn
unsigned long lastButtonChangeTime = 0;      // Thời gian thay đổi trạng thái nút nhấn, tính toán chống dội
unsigned long lastResetButtonChangeTime = 0; // Thời gian thay đổi trạng thái nút nhấn reset WiFi, tính toán chống dội
unsigned long resetButtonPressStartTime = 0; // Thời gian bắt đầu nhấn nút reset WiFi, dùng để kiểm tra thời gian nhấn nút
#define DEBOUNCE_TIME 50                     // Thời gian chống dội (50ms)
#define RESET_BUTTON_HOLD_TIME 2000          // Thời gian nhấn nút reset WiFi (2s)

unsigned long lastFrameTime = 0;
uint8_t speakFrameIndex = 0;
uint8_t listenFrameIndex = 0;
uint8_t idleFrameIndex = 0;
bool isSpeaking = false;
bool isListening = false;

uint8_t eyeSpeak[15] = {1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 4, 3, 1, 1, 1};
uint8_t mouthSpeak[15] = {8, 9, 10, 9, 11, 9, 10, 8, 9, 10, 9, 11, 9, 8, 10};
uint8_t eyeListen[15] = {1, 2, 3, 4, 5, 4, 3, 2, 1, 6, 7, 6, 7, 6, 7};
uint8_t markListen[15] = {12, 13, 14, 13, 12, 13, 14, 15, 16, 15, 14, 13, 12, 13, 14};
uint8_t eyeIdle[22] = {1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 4, 3, 2, 1, 1, 1, 6, 6, 7, 7, 6, 6};

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

TaskHandle_t displayImageTaskHandle = NULL; // Task hiển thị ảnh

QueueHandle_t playAudioQueue = NULL; // Hàng đợi phát âm thanh

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length); // Khai báo hàm xử lý sự kiện WebSocket, xử lý các sự kiện connect, disconnect, message, bin, error, ping, pong

esp_err_t switch_i2s_mode(current_i2s_mode_t new_mode); // Khai báo hàm chuyển đổi chế độ I2S, chuyển đổi giữa các chế độ MIC, SPEAKER và IDLE

esp_err_t i2s_init(); // Khai báo hàm khởi tạo I2S driver, hàm này sẽ được gọi 1 lần duy nhất trong setup() để khởi tạo I2S driver và cấu hình ban đầu

esp_err_t configure_i2s_for_microphone(); // Khai báo hàm cấu hình I2S cho mic, hàm này sẽ được gọi khi chuyển sang chế độ MIC, sửa cấu hình I2S cho mic 32-bit

esp_err_t configure_i2s_for_speaker(); // Khai báo hàm cấu hình I2S cho loa, hàm này sẽ được gọi khi chuyển sang chế độ SPEAKER, sửa cấu hình I2S cho loa 16-bit

void recordAndSendTask(void *parameter); // Khai báo hàm ghi âm và gửi đi, hàm này sẽ được gọi khi nhấn nút ghi âm, sẽ tạo ra 1 task mới để ghi âm và gửi dữ liệu đi qua WebSocket

void playAudioTask(void *parameter); // Khai báo hàm phát âm thanh, hàm này sẽ được gọi khi nhận được dữ liệu âm thanh từ WebSocket, sẽ tạo ra 1 task mới để phát âm thanh

/**
 * @brief Hàm reset cấu hình WiFi, xóa tất cả các thông tin đã lưu trong bộ nhớ flash, sau đó khởi động lại ESP32
 * Vì websocket_server_host thường thay đổi mà WiFi không thay đổi
 */
void resetWiFiSettings()
{
  preferences.begin("wifi", false); // Mở file cấu hình wifi trong bộ nhớ flash, ở chế độ false để ghi
  preferences.clear();              // Xóa tất cả các thông tin đã lưu trong file cấu hình wifi
  preferences.end();                // Đóng file cấu hình wifi trong bộ nhớ flash
  ESP.restart();                    // Khởi động lại ESP32 để áp dụng cấu hình mới
}

/**
 * @brief Hàm xử lý trang chính của web server, sẽ hiển thị trang cấu hình cho người dùng
 */
void handleRoot()
{
  String page = R"rawliteral(
  <!DOCTYPE html>
  <html lang="en">

  <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Settings</title>
      <style>
          /* Reset default margins and padding */
          * {
              margin: 0;
              padding: 0;
              box-sizing: border-box;
          }

          /* Body styling */
          body {
              font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
              background-color: #f4f7fa;
              color: #333;
              line-height: 1.6;
              display: flex;
              justify-content: center;
              align-items: center;
              min-height: 100vh;
              padding: 20px;
          }

          /* Main container */
          .container {
              background-color: #fff;
              padding: 30px;
              border-radius: 10px;
              box-shadow: 0 4px 10px rgba(0, 0, 0, 0.1);
              max-width: 500px;
              width: 100%;
          }

          /* Heading */
          h1 {
              font-size: 2rem;
              color: #2c3e50;
              margin-bottom: 20px;
              text-align: center;
          }

          /* Form styling */
          form {
              display: flex;
              flex-direction: column;
              gap: 15px;
          }

          /* Input fields */
          input[type="text"],
          input[type="password"] {
              padding: 10px;
              font-size: 1rem;
              border: 1px solid #ddd;
              border-radius: 5px;
              width: 100%;
              transition: border-color 0.3s ease;
          }

          input[type="text"]:focus,
          input[type="password"]:focus {
              border-color: #2196F3;
              outline: none;
              box-shadow: 0 0 5px rgba(33, 150, 243, 0.3);
          }

          /* Labels */
          label {
              font-size: 1rem;
              color: #555;
              margin-bottom: 5px;
          }

          /* Switch styling (unchanged) */
          .switch {
              position: relative;
              display: inline-block;
              width: 60px;
              height: 34px;
          }

          .switch input {
              opacity: 0;
              width: 0;
              height: 0;
          }

          .slider {
              position: absolute;
              cursor: pointer;
              top: 0;
              left: 0;
              right: 0;
              bottom: 0;
              background-color: #ccc;
              transition: .4s;
          }

          .slider:before {
              position: absolute;
              content: "";
              height: 26px;
              width: 26px;
              left: 4px;
              bottom: 4px;
              background-color: white;
              transition: .4s;
          }

          input:checked+.slider {
              background-color: #2196F3;
          }

          input:focus+.slider {
              box-shadow: 0 0 1px #2196F3;
          }

          input:checked+.slider:before {
              transform: translateX(26px);
          }

          .slider.round {
              border-radius: 34px;
          }

          .slider.round:before {
              border-radius: 50%;
          }

          /* Submit button */
          input[type="submit"] {
              background-color: #2196F3;
              color: #fff;
              padding: 12px;
              border: none;
              border-radius: 5px;
              font-size: 1rem;
              cursor: pointer;
              transition: background-color 0.3s ease;
              margin-top: 10px;
          }

          input[type="submit"]:hover {
              background-color: #1976D2;
          }

          /* Form row for better alignment */
          .form-row {
              display: flex;
              flex-direction: column;
              gap: 5px;
          }

          /* Checkbox row */
          .checkbox-row {
              display: flex;
              align-items: center;
              gap: 10px;
              margin-top: 0px;
          }

          /* Toggle switch row */
          .toggle-row {
              display: flex;
              align-items: center;
              gap: 10px;
          }

          /* Responsive design */
          @media (max-width: 600px) {
              .container {
                  padding: 20px;
              }

              h1 {
                  font-size: 1.5rem;
              }
          }
      </style>
  </head>

  <body>
      <div class="container">
          <h1 style="font-size: 32px; color:#2196F3">ESP32 Configs</h1>
          <form action="/submit" method="POST">
              <div class="form-row">
                  <label for="wifi">WiFi</label>
                  <input type="text" id="wifi" name="ssid" required>
              </div>
              <div class="form-row">
                  <label for="password">Password</label>
                  <input type="password" id="password" name="password" required>
              </div>
              <div class="checkbox-row">
                  <input type="checkbox" style="padding: 5px; margin-left: 5px; width: 16px; height: 16px;" id="showPassword" onclick="myFunction()">
                  <label for="showPassword">Show Password</label>
              </div>
              <div class="form-row">
                  <label for="ip_address">IP Address Server</label>
                  <input type="text" id="ip_address" name="ip_address" required>
              </div>
              <!-- <div class="form-row">
                  <label for="location">Location</label>
                  <input type="text" id="location" name="Location" required>
              </div> -->
              <!-- <div class="toggle-row">
                  <span>Show Temperature in Celsius</span>
                  <label class="switch">
                      <input type="checkbox">
                      <span class="slider round"></span>
                  </label>
              </div> -->
              <input type="submit" value="Submit">
          </form>
      </div>
  </body>

  <script>
      function myFunction() {
          var x = document.getElementById("password");
          if (x.type === "password") {
              x.type = "text";
          } else {
              x.type = "password";
          }
      }
  </script>

  </html>
  )rawliteral";
  server.send(200, "text/html", page);
}

/**
 * * Hàm xử lý khi người dùng gửi thông tin cấu hình từ trang web, sẽ nhận thông tin SSID, Password và IP Address từ người dùng
 */
void handleSubmit()
{
  /**
   * Hàm arg(const char* name) trong WebServer là hàm để lấy thông tin từ request POST, ở đây là lấy thông tin SSID, Password và IP Address từ người dùng
   */
  ssid = server.arg("ssid");                         // Nhận thông tin SSID từ người dùng
  password = server.arg("password");                 // Nhận thông tin Password từ người dùng
  websockets_server_host = server.arg("ip_address"); // Nhận thông tin địa chỉ IP của Server từ người dùng

  /**
   * Hàm send(int code, const char* content_type, const char* content) trong WebServer là hàm để gửi trang thông báo kết nối thành công
   * @param {int} code là mã trạng thái HTTP, 200 là OK
   * @param {const char*} content_type là kiểu nội dung, ở đây là "text/html"
   * @param {const char*} content là nội dung trang thông báo kết nối thành công
   */
  server.send(200, "text/html", "<h1 style='font-size: 48px; color:#2196F3; margin: 48px;'>Connecting...</h1>");

  /**
   * Hàm begin() trong Preferences là hàm để khởi tạo đối tượng Preferences, mở file cấu hình wifi trong bộ nhớ flash
   * @param {const char*} namespace là tên file cấu hình wifi, có thể là bất kỳ tên nào, ở đây là "wifi"
   * @param {bool} readOnly là tham số để chỉ định chế độ mở file, nếu là true thì chỉ mở file để đọc, nếu là false thì mở file để ghi
   */
  preferences.begin("wifi", false);
  /**
   * Hàm putString(const char* key, const char* value) trong Preferences là hàm để lưu trữ dữ liệu vào file cấu hình wifi trong bộ nhớ flash
   * @param {const char*} key là tên khóa để lưu trữ dữ liệu, có thể là bất kỳ tên nào, ở đây là "ssid", "password", "server"
   * @param {const char*} value là giá trị cần lưu trữ, ở đây là ssid, password, websockets_server_host
   */
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("server", websockets_server_host);
  // Hàm end() trong Preferences là hàm để đóng file cấu hình wifi trong bộ nhớ flash
  preferences.end();

  delay(1000);

  ESP.restart(); // Khởi động lại ESP32 để áp dụng cấu hình mới
}

/**
 * Hàm startAP() sẽ được gọi khi không kết nối được WiFi, sẽ tạo một mạng WiFi Access Point (AP) với tên là ESP32_Config
 * Hàm này sẽ tạo một web server để người dùng có thể cấu hình thông tin cho ESP32 bao gồm thông tin về WiFi và địa chỉ IP của server
 * Hàm này sẽ được gọi trong hàm setup() khi không kết nối được WiFi
 */
void startAP()
{
  Serial.println("Không kết nối WiFi, bật Access Point...");
  WiFi.mode(WIFI_AP); // Chế độ WiFi Access Point (AP) để tạo mạng WiFi riêng, có các mode như WIFI_STA (chế độ client), WIFI_AP_STA (chế độ client và AP cùng lúc), WIFI_AP (chế độ AP)
  /**
   * Hàm softAP(const char* ssid, const char* password) trong WiFi là hàm để tạo mạng WiFi Access Point (AP)
   * @param {const char*} ssid là tên mạng WiFi, có thể là bất kỳ tên nào, ở đây là apSSID
   * @param {const char*} password là mật khẩu mạng WiFi, có thể là bất kỳ tên nào, ở đây là apSSID, nếu không có mật khẩu thì để trống
   */
  WiFi.softAP(apSSID);              // Tạo mạng WiFi với SSID là apSSID: ESP32_Config
  IPAddress myIP = WiFi.softAPIP(); // Lấy địa chỉ IP của ESP32 khi ở chế độ Access Point (AP)

  /**
   * Hàm start(uint16_t port, const char* hostname, IPAddress ip) trong DNSServer là hàm để khởi động DNS server
   * @param {uint16_t} port là cổng DNS server, mặc định là 53
   * @param {const char*} hostname là tên miền của DNS server, có thể là bất kỳ tên nào, ở đây là "*"
   * @param {IPAddress} ip là địa chỉ IP của ESP32 khi ở chế độ Access Point (AP), địa chỉ IP này sẽ được sử dụng để chuyển hướng tất cả các request lạ về trang cấu hình của ESP32
   */
  dns.start(DNS_PORT, "*", myIP);

  /**
   * Hàm on(const char* uri, WebServer::THandlerFunction handler) trong WebServer là hàm để đăng ký hàm xử lý cho request GET tới trang chính của web server
   * @param {const char*} uri là đường dẫn của trang web, ở đây là "/"
   * @param {WebServer::THandlerFunction} handler là hàm xử lý cho request GET tới trang chính của web server, ở đây là handleRoot
   */
  server.on("/", handleRoot);
  /**
   * Hàm on(const char* uri, HTTPMethod method, WebServer::THandlerFunction handler) trong WebServer là hàm để đăng ký hàm xử lý cho request POST tới trang cấu hình của web server
   * @param {const char*} uri là đường dẫn của trang web, ở đây là "/submit"
   * @param {HTTPMethod} method là phương thức HTTP, ở đây là HTTP_POST, nếu không có thì mặc định là GET
   * @param {WebServer::THandlerFunction} handler là hàm xử lý cho request POST tới trang cấu hình của web server, ở đây là handleSubmit
   */
  server.on("/submit", HTTP_POST, handleSubmit);

  /**
   * Hàm onNotFound(WebServer::THandlerFunction handler) trong WebServer là hàm để đăng ký hàm xử lý cho request không tìm thấy trang web
   */
  server.onNotFound([]()
                    {
    /**
     * Hàm sendHeader(const char* name, const char* value, bool first) trong WebServer là hàm để gửi header cho response, header để chuyển hướng về trang chính của web server
     * @param {const char*} name là tên header, ở đây là "Location"
     * @param {const char*} value là giá trị header, ở đây là "/" để chuyển hướng về trang chính của web server
     * @param {bool} first là tham số để chỉ định header đầu tiên hay không, nếu là true thì là header đầu tiên, nếu là false thì không phải, header đầu tiên là Location, header thứ 2 là Content-Type
     */
    server.sendHeader("Location", "/", true);
    /**
     * Hàm send(int code, const char* content_type, const char* content) trong WebServer là hàm để gửi trang thông báo không tìm thấy trang web, với mã trạng thái 302 thì trang web sẽ tự động chuyển hướng về trang chính của web server
     * @param {int} code là mã trạng thái HTTP, 302 là chuyển hướng, chuyển hướng tới trang chính của web server vì đã gửi header Location
     * @param {const char*} content_type là kiểu nội dung, ở đây là "text/plain"
     * @param {const char*} content là nội dung trang thông báo không tìm thấy trang web
     */
    server.send(302, "text/plain", ""); });

  // Khởi động web server trên cổng 80
  server.begin();
}

/**
 * Hàm xử lý sự kiện WebSocket, Hàm này sẽ được gọi khi có sự kiện xảy ra trên WebSocket
 * @param {WStype_t} type là kiểu sự kiện, có thể là WStype_CONNECTED, WStype_DISCONNECTED, WStype_TEXT, WStype_BIN, WStype_ERROR, WStype_PING, WStype_PONG
 * @param {uint8_t*} payload là dữ liệu nhận được từ WebSocket, có thể là văn bản hoặc nhị phân
 * @param {size_t} length là độ dài dữ liệu nhận được từ WebSocket
 */
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
      /**
       * Hàm BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait) trong FreeRTOS là hàm để nhận dữ liệu từ một hàng đợi
       * @param {QueueHandle_t} xQueue là con trỏ tới hàng đợi cần nhận dữ liệu
       * @param {void*} pvBuffer là con trỏ tới bộ nhớ để lưu dữ liệu nhận được từ hàng đợi
       * @param {TickType_t} xTicksToWait là thời gian chờ để nhận dữ liệu từ hàng đợi
       * @return {BaseType_t} pdTRUE nếu nhận dữ liệu thành công, pdFALSE nếu không nhận được dữ liệu trong thời gian chờ
       * Nếu chunk không NULL thì giải phóng bộ nhớ đã cấp phát cho chunk và dữ liệu trong chunk
       * Cuối cùng là đặt lại hàng đợi để xóa tất cả các chunk trong hàng đợi
       */
      while (xQueueReceive(playAudioQueue, &chunk, 0) == pdTRUE) // Hàm xQueueReceive() trong FreeRTOS là hàm để nhận dữ liệu từ một hàng đợi
      {
        if (chunk)
        {
          if (chunk->data)
          {
            free(chunk->data); // Giải phóng bộ nhớ đã cấp phát cho dữ liệu trong chunk
          }
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
      speakFrameIndex = 0;
      lastFrameTime = millis();
      digitalWrite(LED_PLAY, HIGH);
    }
    else if (strcmp((char *)payload, "AUDIO_STREAM_END") == 0) // Lệnh báo phát hết âm thanh
    {
      digitalWrite(LED_PLAY, LOW);
      isSpeaking = false;
      tft.fillScreen(TFT_BLACK);
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
        // Kiểm tra thông tin WAV header, có cấu trúc WAV header cố định rồi
        uint16_t bits_per_sample = *(uint16_t *)&payload[34];
        uint32_t sample_rate = *(uint32_t *)&payload[24];
        uint16_t channels = *(uint16_t *)&payload[22];

        // Phát âm thanh qua MAX98357A, chỉ hỗ trợ 16-bit, 16kHz, mono
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

      // Cấp phát bộ nhớ cho dữ liệu âm thanh
      chunk->data = (uint8_t *)malloc(audioLength);
      if (!chunk->data)
      {
        Serial.println("Cannot allocate memory for chunk->data!");
        free(chunk);
        break;
      }

      // Sao chép dữ liệu âm thanh vào chunk
      memcpy(chunk->data, audioData, audioLength);
      chunk->length = audioLength;

      // Gửi vào queue để phát, tăng timeout để tránh bỏ qua chunk
      // Hàm xQueueSend() trong FreeRTOS là hàm để gửi dữ liệu vào hàng đợi, tương tự như hàm xQueueReceive() nhưng ngược lại
      // pdMS_TO_TICKS(100) là thời gian timeout để gửi dữ liệu vào hàng đợi, nếu không gửi được trong thời gian này tức là hàng đợi đầy.
      if (xQueueSend(playAudioQueue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE)
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

/**
 * Hàm chuyển đổi chế độ I2S giữa loa MAX98357A 16 bit và mic INMP441 32 bit
 * @param {current_i2s_mode_t} new_mode là chế độ mới cần chuyển đổi, có thể là I2S_MODE_MIC, I2S_MODE_SPEAKER hoặc I2S_MODE_IDLE
 * @return {esp_err_t} ESP_OK nếu chuyển đổi thành công, ESP_FAIL nếu không thành công
 */
esp_err_t switch_i2s_mode(current_i2s_mode_t new_mode)
{
  /**
   * Hàm BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait) trong FreeRTOS là hàm để lấy mutex (semaphore) để bảo vệ việc thay đổi cấu hình I2S, không cho phép nhiều task cùng thay đổi cấu hình I2S tại một thời điểm
   * @param {SemaphoreHandle_t} xSemaphore là con trỏ tới mutex cần lấy
   * @param {TickType_t} xTicksToWait là thời gian chờ để lấy mutex, nếu không lấy được mutex trong thời gian này thì sẽ trả về pdFALSE
   * @return {BaseType_t} pdTRUE nếu lấy được mutex, pdFALSE nếu không lấy được mutex trong thời gian chờ
   */
  if (xSemaphoreTake(i2s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
  {
    Serial.println("Failed to get I2S mutex. Cannot switch mode.");
    return ESP_FAIL;
  }

  // Nếu chế độ mới giống chế độ hiện tại, không cần làm gì
  if (current_i2s_mode == new_mode)
  {
    /**
     * Hàm xSemaphoreGive(SemaphoreHandle_t xSemaphore) trong FreeRTOS là hàm để giải phóng mutex (semaphore) đã lấy trước đó
     */
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

/**
 * Hàm khởi tạo I2S driver, hàm này sẽ được gọi 1 lần duy nhất trong setup() để khởi tạo I2S driver và cấu hình ban đầu
 * @return {esp_err_t} ESP_OK nếu khởi tạo thành công, ESP_FAIL nếu không thành công
 */
esp_err_t i2s_init()
{

  // Cấu hình I2S driver
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX), // Chế độ Master, ESP32 sẽ điều khiển xung clock và Word Select, hỗ trợ cả thu (RX) và phát (TX)
      .sample_rate = I2S_SAMPLE_RATE,                                    // Tốc độ lấy mẫu (16kHz)
      .bits_per_sample = I2S_BITS_PER_SAMPLE_RX,                         // Số bit mỗi mẫu (bắt đầu với 32-bit, sẽ thay đổi khi cần vì MAX98357A chỉ hỗ trợ 16-bit)
      .channel_format = MIC_CHANNEL_FMT,                                 // Định dạng kênh, sử dụng mono
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,                 // Giao thức truyền thông I2S tiêu chuẩn
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,                          // Cấp độ ngắt (mức 1)
      .dma_buf_count = I2S_BUFFER_COUNT,                                 // Số lượng buffer DMA, 8 buffer DMA sẽ được sử dụng để truyền dữ liệu giữa I2S và RAM
      .dma_buf_len = I2S_READ_LEN / I2S_BUFFER_COUNT,                    // Độ dài mỗi buffer DMA, 512 byte cho mỗi buffer DMA
      .use_apll = false,                                                 // Không sử dụng APLL (Audio PLL), APLL là một bộ tạo xung đồng hồ âm thanh, giúp giảm độ nhiễu và tăng độ chính xác của tín hiệu âm thanh
      .tx_desc_auto_clear = true,                                        // Tự động xóa mô tả TX khi buffer trống, giúp giảm độ trễ khi phát âm thanh
      .fixed_mclk = 0};                                                  // Không sử dụng MCLK cố định, MCLK là xung đồng hồ chính cho I2S, thường được sử dụng cho các thiết bị âm thanh bên ngoài như DAC (Digital to Analog Converter) hoặc ADC (Analog to Digital Converter)

  /**
   * Hàm esp_err_t i2s_driver_install(i2s_port_t i2s_num, const i2s_config_t *i2s_config, int intr_alloc_flags, void *intr_handle) trong ESP-IDF là hàm để cài đặt driver I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần cài đặt, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @param {const i2s_config_t*} i2s_config là con trỏ tới cấu hình I2S đã được định nghĩa ở trên
   * @param {int} intr_alloc_flags là cờ cấp phát ngắt
   * @param {void*} intr_handle là con trỏ tới hàm xử lý ngắt, có thể là NULL nếu không sử dụng ngắt
   * @return {esp_err_t} ESP_OK nếu cài đặt thành công, ESP_FAIL nếu không thành công
   */
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to install I2S driver. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Bắt đầu ở IDLE mode
  current_i2s_mode = I2S_MODE_IDLE;

  return ESP_OK;
}

/**
 * Hàm cấu hình I2S cho mic, hàm này sẽ được gọi khi chuyển sang chế độ MIC
 * @return {esp_err_t} ESP_OK nếu cấu hình thành công, ESP_FAIL nếu không thành công
 */
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

  /**
   * Hàm esp_err_t i2s_set_pin(i2s_port_t i2s_num, const i2s_pin_config_t *pin_config) trong ESP-IDF là hàm để cấu hình chân I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần cấu hình, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @param {const i2s_pin_config_t*} pin_config là con trỏ tới cấu hình chân I2S đã được định nghĩa ở trên
   * @return {esp_err_t} ESP_OK nếu cấu hình thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S pins for microphone. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  /**
   * Hàm esp_err_t i2s_set_clk(i2s_port_t i2s_num, uint32_t rate, i2s_bits_per_sample_t bits_per_sample, i2s_channel_fmt_t channel_format) trong ESP-IDF là hàm để cấu hình tốc độ lấy mẫu và số bit mỗi mẫu cho I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần cấu hình, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @param {uint32_t} rate là tốc độ lấy mẫu, có thể là 8000, 16000, 32000, 44100, 48000, 96000, 192000. Với INMP441 thì chỉ hỗ trợ 8kHz, 16kHz, 32kHz, 48kHz. Dùng 16kHz
   * @param {i2s_bits_per_sample_t} bits_per_sample là số bit mỗi mẫu, có thể là I2S_BITS_PER_SAMPLE_16BIT, I2S_BITS_PER_SAMPLE_24BIT, I2S_BITS_PER_SAMPLE_32BIT. Với INMP441 thì chỉ hỗ trợ 16-bit và 32-bit. Dùng 32-bit để ghi âm.
   * @param {i2s_channel_fmt_t} channel_format là định dạng kênh, có thể là I2S_CHANNEL_FMT_ONLY_LEFT, I2S_CHANNEL_FMT_ONLY_RIGHT, I2S_CHANNEL_FMT_RIGHT_LEFT, I2S_CHANNEL_FMT_ALL_RIGHT, I2S_CHANNEL_FMT_ALL_LEFT
   * @return {esp_err_t} ESP_OK nếu cấu hình thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_set_clk(I2S_PORT, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_RX, I2S_CHANNEL_MONO);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S clock for microphone. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  /**
   * Hàm esp_err_t i2s_zero_dma_buffer(i2s_port_t i2s_num) trong ESP-IDF là hàm để xóa bộ đệm DMA của I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần xóa bộ đệm DMA, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @return {esp_err_t} ESP_OK nếu xóa bộ đệm DMA thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_zero_dma_buffer(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to zero DMA buffer. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  /**
   * Hàm esp_err_t i2s_start(i2s_port_t i2s_num) trong ESP-IDF là hàm để bắt đầu I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần bắt đầu, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @return {esp_err_t} ESP_OK nếu bắt đầu I2S thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_start(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to start I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}

/**
 * Hàm cấu hình I2S cho loa, hàm này sẽ được gọi khi chuyển sang chế độ SPEAKER
 * @return {esp_err_t} ESP_OK nếu cấu hình thành công, ESP_FAIL nếu không thành công
 */
esp_err_t configure_i2s_for_speaker()
{

  // Dừng I2S
  esp_err_t err = i2s_stop(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to stop I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Cấu hình chân I2S cho loa
  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = I2S_SD_OUT,
      .data_in_num = -1 // Dùng loa thì không dùng input
  };

  /**
   * Hàm esp_err_t i2s_set_pin(i2s_port_t i2s_num, const i2s_pin_config_t *pin_config) trong ESP-IDF là hàm để cấu hình chân I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần cấu hình, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @param {const i2s_pin_config_t*} pin_config là con trỏ tới cấu hình chân I2S đã được định nghĩa ở trên
   * @return {esp_err_t} ESP_OK nếu cấu hình thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S pins for speaker. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  /**
   * Hàm esp_err_t i2s_set_clk(i2s_port_t i2s_num, uint32_t rate, i2s_bits_per_sample_t bits_per_sample, i2s_channel_fmt_t channel_format) trong ESP-IDF là hàm để cấu hình tốc độ lấy mẫu và số bit mỗi mẫu cho I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần cấu hình, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @param {uint32_t} rate là tốc độ lấy mẫu, có thể là 8000, 16000, 32000, 44100, 48000, 96000, 192000. Với MAX98357A thì chỉ hỗ trợ 16kHz
   * @param {i2s_bits_per_sample_t} bits_per_sample là số bit mỗi mẫu, có thể là I2S_BITS_PER_SAMPLE_16BIT, I2S_BITS_PER_SAMPLE_24BIT, I2S_BITS_PER_SAMPLE_32BIT. Với MAX98357A thì chỉ hỗ trợ 16-bit
   * @param {i2s_channel_fmt_t} channel_format là định dạng kênh, có thể là I2S_CHANNEL_FMT_ONLY_LEFT, I2S_CHANNEL_FMT_ONLY_RIGHT, I2S_CHANNEL_FMT_RIGHT_LEFT, I2S_CHANNEL_FMT_ALL_RIGHT, I2S_CHANNEL_FMT_ALL_LEFT
   * @return {esp_err_t} ESP_OK nếu cấu hình thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_set_clk(I2S_PORT, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_TX, I2S_CHANNEL_MONO);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to set I2S clock for speaker. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  /**
   * Hàm esp_err_t i2s_zero_dma_buffer(i2s_port_t i2s_num) trong ESP-IDF là hàm để xóa bộ đệm DMA của I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần xóa bộ đệm DMA, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @return {esp_err_t} ESP_OK nếu xóa bộ đệm DMA thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_zero_dma_buffer(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to zero DMA buffer. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  // Bật loa
  digitalWrite(SPEAKER_SD_PIN, HIGH);

  /**
   * Hàm esp_err_t i2s_start(i2s_port_t i2s_num) trong ESP-IDF là hàm để bắt đầu I2S
   * @param {i2s_port_t} i2s_num là cổng I2S cần bắt đầu, có thể là I2S_NUM_0 hoặc I2S_NUM_1
   * @return {esp_err_t} ESP_OK nếu bắt đầu I2S thành công, ESP_FAIL nếu không thành công
   */
  err = i2s_start(I2S_PORT);
  if (err != ESP_OK)
  {
    Serial.printf("Failed to start I2S. Error: %d (%s)\n", err, esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}

/**
 * Hàm ghi âm và gửi dữ liệu âm thanh qua WebSocket
 * @note Hàm này sẽ được gọi trong một task riêng biệt để không làm chậm quá trình ghi âm
 * @param {void*} parameter là tham số truyền vào task, không sử dụng trong hàm này
 */
void recordAndSendTask(void *parameter)
{
  isListening = true;
  listenFrameIndex = 0;
  lastFrameTime = millis();
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

  int32_t *i2s_read_buffer = NULL; // Bộ đệm đọc I2S 32-bit
  int16_t *pcm_send_buffer = NULL; // Bộ đệm gửi PCM 16-bit

  size_t bytes_read = 0;    // Số byte đã đọc từ I2S
  size_t bytes_to_send = 0; // Số byte sẽ gửi qua WebSocket

  // Cấp phát bộ đệm đọc (32-bit)
  i2s_read_buffer = (int32_t *)malloc(I2S_READ_LEN);
  // Nếu không cấp phát được bộ đệm thì in ra thông báo lỗi và thoát khỏi task
  if (!i2s_read_buffer)
  {
    Serial.println("Failed to allocate memory for I2S read buffer!");
    goto cleanup; // Thoát khỏi task nếu không cấp phát được bộ đệm
  }

  // Cấp phát bộ đệm gửi (16-bit)
  pcm_send_buffer = (int16_t *)malloc(I2S_READ_LEN / 2);
  // Nếu không cấp phát được bộ đệm thì in ra thông báo lỗi và thoát khỏi task
  if (!pcm_send_buffer)
  {
    Serial.println("Failed to allocate memory for PCM send buffer!");
    goto cleanup; // Thoát khỏi task nếu không cấp phát được bộ đệm
  }

  recordingStartTime = millis(); // Lưu thời gian bắt đầu ghi âm
  Serial.println("Start Recording for 5 seconds...");
  digitalWrite(LED_RECORD, HIGH);

  // Ghi âm trong khoảng thời gian cố định là 5 giây
  while (isRecording && (millis() - recordingStartTime < RECORD_DURATION_MS))
  {
    /**
     * Hàm esp_err_t i2s_read(i2s_port_t i2s_num, void *buffer, size_t bytes_to_read, size_t *bytes_read, TickType_t ticks_to_wait) trong ESP-IDF là hàm để đọc dữ liệu từ I2S
     * @param {i2s_port_t} i2s_num là cổng I2S cần đọc, có thể là I2S_NUM_0 hoặc I2S_NUM_1
     * @param {void*} buffer là con trỏ tới bộ đệm để lưu dữ liệu đọc được
     * @param {size_t} bytes_to_read là số byte cần đọc từ I2S
     * @param {size_t*} bytes_read là con trỏ tới biến lưu số byte đã đọc từ I2S
     * @param {TickType_t} ticks_to_wait là thời gian chờ để đọc dữ liệu từ I2S, nếu không đọc được trong thời gian này thì sẽ trả về ESP_ERR_TIMEOUT
     * @return {esp_err_t} ESP_OK nếu đọc thành công, ESP_FAIL nếu không thành công
     */
    esp_err_t read_result = i2s_read(I2S_PORT, i2s_read_buffer, I2S_READ_LEN, &bytes_read, pdMS_TO_TICKS(1000));

    if (read_result == ESP_OK && bytes_read > 0)
    {
      int samples_read = bytes_read / 4; // biến samples_read là số mẫu đã đọc từ I2S, mỗi mẫu 32-bit (4 byte)
      bytes_to_send = samples_read * 2;  // Số byte sẽ gửi qua WebSocket, mỗi mẫu 16-bit (2 byte)

      for (int i = 0; i < samples_read; i++)
      {
        int32_t sample = i2s_read_buffer[i] >> 16;    // Chỉ lấy 16 bit cao của mẫu 32-bit
        sample = (int32_t)(sample * MIC_GAIN_FACTOR); // Nhân với hệ số khuếch đại mic để tăng độ nhạy

        // Giới hạn giá trị mẫu trong khoảng -32768 đến 32767 (16-bit signed int)
        if (sample > 32767)
          sample = 32767;
        if (sample < -32768)
          sample = -32768;

        pcm_send_buffer[i] = (int16_t)sample; // Đưa mẫu đã khuếch đại vào bộ đệm gửi
      }

      if (webSocket.isConnected())
      {
        /**
         * Hàm sendBIN(const uint8_t* data, size_t len) trong WebSocketClient là hàm để gửi dữ liệu nhị phân qua WebSocket
         * @param {const uint8_t*} data là con trỏ tới dữ liệu cần gửi
         * @param {size_t} len là độ dài dữ liệu cần gửi
         * @return {bool} true nếu gửi thành công, false nếu không thành công
         */
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

  isListening = false;
  tft.fillScreen(TFT_BLACK);

cleanup:
  // Giải phóng bộ đệm và dừng ghi âm
  Serial.println("Recording finished after 5 seconds.");
  digitalWrite(LED_RECORD, LOW);
  isRecording = false;

  if (i2s_read_buffer) // Giải phóng bộ đệm đọc I2S
    free(i2s_read_buffer);
  if (pcm_send_buffer) // Giải phóng bộ đệm gửi PCM
    free(pcm_send_buffer);

  if (webSocket.isConnected()) // Gửi thông báo kết thúc gửi dữ liệu qua WebSocket
  {
    webSocket.sendTXT("END_OF_STREAM");
    Serial.println("Sent END_OF_STREAM");
  }

  // Sau khi kết thúc ghi âm, chuyển về chế độ IDLE để chuẩn bị cho lần tiếp theo
  switch_i2s_mode(I2S_MODE_IDLE);

  recordTaskHandle = NULL;
  vTaskDelete(NULL);
}

/**
 * Hàm phát âm thanh từ queue
 * @note Hàm này sẽ được gọi trong một task riêng biệt để không làm chậm quá trình phát âm thanh
 * @param {void*} parameter là tham số truyền vào task, không sử dụng trong hàm này
 */
void playAudioTask(void *parameter)
{
  AudioChunk *chunk = NULL;          // Biến chứa chunk âm thanh cần phát
  size_t bytes_written = 0;          // Số byte đã ghi vào I2S
  bool speaker_mode_active = false;  // Trạng thái loa đang hoạt động hay không
  unsigned long lastAudioTime = 0;   // Thời gian nhận audio gần nhất, nếu không nhận được âm thanh trong 1.5 giây thì sẽ chuyển về chế độ I2S IDLE
  unsigned long lastMemoryCheck = 0; // Thời gian kiểm tra bộ nhớ gần nhất, để quản lý bộ nhớ

  while (true)
  {
    // Kiểm tra trạng thái bộ nhớ mỗi 2 giây
    if (millis() - lastMemoryCheck > 2000)
    {
      /**
       * Hàm uint32_t ESP.getFreeHeap() trong ESP-IDF là hàm để lấy dung lượng bộ nhớ heap còn trống
       * @return {uint32_t} Dung lượng bộ nhớ heap còn trống (byte)
       */
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
          isSpeaking = false;

          delay(100);
        }
      }
    }

    if (xQueueReceive(playAudioQueue, &chunk, pdMS_TO_TICKS(1000)) == pdPASS)
    {
      isSpeaking = true;
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
        int16_t *audio_samples = (int16_t *)chunk->data;
        int samples_count = chunk->length / 2; // Số lượng mẫu 16-bit
        // Điều chỉnh biên độ của mỗi mẫu âm thanh
        for (int i = 0; i < samples_count; i++)
        {
          audio_samples[i] = audio_samples[i] * 0.8; // Tăng giảm hệ số
        }

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
      if (speaker_mode_active && (millis() - lastAudioTime > 1000))
      {
        speaker_mode_active = false;
        // Tắt loa và về trạng thái NONE để sẵn sàng ghi âm tiếp
        switch_i2s_mode(I2S_MODE_IDLE);
        digitalWrite(SPEAKER_SD_PIN, LOW); // Tắt loa
        // Xóa queue để đảm bảo không còn dữ liệu cũ
        xQueueReset(playAudioQueue);
        isSpeaking = false;
      }
    }
  }
}

/**
 * Hàm kết nối tới WebSocket server
 * @note Hàm này sẽ được gọi khi khởi động ESP32 và kết nối thành công tới WiFi
 */
void initializeWebSocket()
{
  webSocket.begin(websockets_server_host, websockets_server_port, websockets_path);
  webSocket.onEvent(webSocketEvent);         // Đăng ký hàm xử lý sự kiện WebSocket
  webSocket.setReconnectInterval(5000);      // Thời gian thử lại kết nối nếu bị mất
  webSocket.enableHeartbeat(10000, 2000, 3); // Thời gian heartbeat 10 giây, timeout 2 giây, thử lại 3 lần, heartbeat sẽ tự động gửi ping/pong để kiểm tra kết nối
}

void updateDisplaySpeak()
{
  if (!isSpeaking)
    return;

  unsigned long now = millis();
  if (now - lastFrameTime >= 200)
  {
    lastFrameTime = now;

    tft.pushImage(20, 60, 90, 90, myBitmapArray[eyeSpeak[speakFrameIndex] - 1]);
    tft.pushImage(125, 60, 90, 90, myBitmapArray[eyeSpeak[speakFrameIndex] - 1]);
    tft.pushImage(80, 150, 80, 80, myBitmapArray[mouthSpeak[speakFrameIndex] - 1]);

    speakFrameIndex++;
    if (speakFrameIndex >= 15)
    {
      speakFrameIndex = 0;
    }
  }
}

void updateDisplayIdle() {
  if (isSpeaking || isListening) {
    return;
  }

  unsigned long now = millis();
  if (now - lastFrameTime >= 200)
  {
    lastFrameTime = now;
    // tft.fillScreen(TFT_BLACK);
    tft.pushImage(20, 60, 90, 90, myBitmapArray[eyeIdle[idleFrameIndex] - 1]);
    tft.pushImage(125, 60, 90, 90, myBitmapArray[eyeIdle[idleFrameIndex] - 1]);
    idleFrameIndex++;
    if (idleFrameIndex >= 22)
    {
      idleFrameIndex = 0;
    }
  }
}

void updateDisplayListen()
{
  if (!isListening)
    return;

  unsigned long now = millis();
  if (now - lastFrameTime >= 200)
  {
    lastFrameTime = now;

    tft.pushImage(165, 0, 60, 60, myBitmapArray[markListen[listenFrameIndex] - 1]);
    tft.pushImage(20, 60, 90, 90, myBitmapArray[eyeListen[listenFrameIndex] - 1]);
    tft.pushImage(125, 60, 90, 90, myBitmapArray[eyeListen[listenFrameIndex] - 1]);

    listenFrameIndex++;
    if (listenFrameIndex >= 15)
    {
      listenFrameIndex = 0;
    }
  }
}

/**
 * Hàm setup() được gọi khi khởi động ESP32
 * @note Hàm này sẽ được gọi một lần duy nhất khi khởi động ESP32
 * @note Hàm này sẽ cấu hình WiFi, WebSocket, I2S và các chân GPIO
 */
void setup()
{
  Serial.begin(115200); // Khởi động Serial với tốc độ 115200 bps

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  preferences.begin("wifi", true); // Khởi động Preferences để lưu trữ thông tin WiFi với trạng thái chỉ đọc
  // Lấy thông tin WiFi từ Preferences
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  // Lấy thông tin địa chỉ IP Server từ Preferences
  websockets_server_host = preferences.getString("server", "");
  preferences.end(); // Đóng Preferences

  if (ssid.length() > 0 && password.length() > 0)
  {
    WiFi.mode(WIFI_STA); // Chế độ STA (Client)
    // Hàm WiFi.begin(const char* ssid, const char* password) trong ESP32 là hàm để kết nối tới WiFi, ssid và password đang là String
    // Chuyển đổi sang kiểu char* bằng cách dùng c_str()
    WiFi.begin(ssid.c_str(), password.c_str());

    int attempts = 0; // Biến đếm số lần thử kết nối
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      wifiConnected = true;
      if (websockets_server_host.length() > 0)
      {
        initializeWebSocket(); // Khởi tạo WebSocket nếu có địa chỉ server
      }
      else
      {
        Serial.println("\nKhông có địa chỉ server, không khởi tạo WebSocket");
      }
    }
    else
    {
      Serial.println("\nKết nối thất bại, chuyển sang chế độ AP");
      startAP(); // Chuyển sang chế độ AP nếu không kết nối được WiFi
    }
  }
  else
  {
    Serial.println("Không có thông tin WiFi, chuyển sang chế độ AP");
    startAP(); // Chuyển sang chế độ AP nếu không có thông tin WiFi
  }

  // Cấu hình chân nút nhấn
  pinMode(PIN_BUTTON, INPUT_PULLUP);            // để ở pullup vì nút nhấn nối đất, mặc định khi không nhấn là HIGH
  pinMode(PIN_RESET_WIFI_BUTTON, INPUT_PULLUP); // để ở pullup vì nút nhấn nối đất, mặc định khi không nhấn là HIGH

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

  // Tạo hàng đợi Playback chứa con trỏ tới AudioChunk*
  playAudioQueue = xQueueCreate(32, sizeof(AudioChunk *)); // Tạo hàng đợi với kích thước 32 chunk, nếu dùng 16chunk sẽ không đủ.
  if (playAudioQueue == NULL)
  {
    Serial.println("Failed to create playAudio queue. Halting.");
    while (1)
      ;
  }

  // Chip ESP32 đa nhân, Sử dụng hàm này để tạo 1 task và ghim nó vào core 0, tránh trường hợp xung đột với các task khác
  // Hàm trong thư viện FreeRTOS, là một thư viện hệ điều hành thời gian thực cho ESP32, có thể tạo nhiều task chạy song song
  xTaskCreatePinnedToCore(
      playAudioTask, // Tên hàm xử lý task
      "PlaybackTask",
      8192, // Kích thước stack cho task (8192 bytes) 8KB
      NULL,
      5,                    // Mức độ ưu tiên của task (5 là mức độ ưu tiên trung bình)
      &playAudioTaskHandle, // Nếu tạo thành công task thì con trỏ này sẽ trỏ tới task đó
      0);                   // Core 0 (core 1 là core 1)
  if (playAudioTaskHandle == NULL)
  {
    Serial.println("Failed to create playAudio task. Halting.");
    while (1)
      ;
  }
}

// --- Loop ---
void loop()
{
  updateDisplaySpeak();
  updateDisplayListen();
  updateDisplayIdle();
  
  while (!wifiConnected)
  {
    /**
     * Hàm dns.processNextRequest() trong ESP32 là hàm để xử lý các yêu cầu DNS trong chế độ AP
     * @note Hàm này sẽ được gọi trong chế độ AP để xử lý các yêu cầu DNS từ client
     */
    dns.processNextRequest();
    /**
     * Hàm server.handleClient() trong ESP32 là hàm để xử lý các yêu cầu HTTP từ client
     * @note Hàm này sẽ được gọi trong chế độ AP để xử lý các yêu cầu HTTP từ client
     */
    server.handleClient();
  }

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

  bool currentResetButtonState = digitalRead(PIN_RESET_WIFI_BUTTON) == LOW; // LOW khi nhấn (pull-up)
  currentTime = millis();
  // Kiểm tra nút nhấn reset WiFi
  if (currentTime - lastResetButtonChangeTime > DEBOUNCE_TIME)
  {
    if (currentResetButtonState && !resetButtonPressed) // Nếu nhấn nút reset Config và chưa nhấn trước đó
    {
      resetButtonPressed = true;               // Đánh dấu nút đã được nhấn, để đo thời gian nhấn
      resetButtonPressStartTime = currentTime; // Bắt đầu tính thời gian nhấn nút
      lastResetButtonChangeTime = currentTime; // Cập nhật thời gian thay đổi trạng thái nút nhấn, để chống dội
    }

    // Nếu giữ nút nhấn quá thời gian quy định, thực hiện reset Config
    if (currentResetButtonState && resetButtonPressed &&
        (currentTime - resetButtonPressStartTime >= RESET_BUTTON_HOLD_TIME))
    {

      // Nháy đèn LED để báo hiệu đang reset WiFi
      for (int i = 0; i < 10; i++)
      {
        digitalWrite(LED_RECORD, HIGH);
        digitalWrite(LED_PLAY, HIGH);
        delay(100);
        digitalWrite(LED_RECORD, LOW);
        digitalWrite(LED_PLAY, LOW);
        delay(100);
      }

      resetWiFiSettings(); // Hàm này sẽ xóa thông tin Config trong Preferences và khởi động lại ESP32
    }
    // Nếu thả nút nhấn, đặt lại trạng thái nút nhấn
    if (!currentResetButtonState && resetButtonPressed)
    {
      resetButtonPressed = false;
      lastResetButtonChangeTime = currentTime;
    }
  }
}
