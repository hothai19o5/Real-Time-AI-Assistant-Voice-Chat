# 🎙️ Real-Time AI Voice Chat using ESP32 & WebSocket

![ESP32](https://img.shields.io/badge/ESP32-RealTime-blue)
![Node.js](https://img.shields.io/badge/Node.js-Server-yellowgreen)
![Vosk](https://img.shields.io/badge/Vosk-STT-red)
![Gemini](https://img.shields.io/badge/Gemini-AI-lightgrey)
![Orca](https://img.shields.io/badge/Orca-TTS-blueviolet)

## 🧠 Mô tả dự án

**Real-Time AI Voice Chat** là một hệ thống giao tiếp hai chiều bằng giọng nói giữa con người và trí tuệ nhân tạo. Dự án sử dụng:

- ESP32 (thu & phát âm thanh)
- WebSocket (giao tiếp thời gian thực)
- Vosk (speech-to-text)
- Gemini (AI chatbot)
- Orca của Picovoice (text-to-speech)

Toàn bộ quá trình diễn ra hoàn toàn **real-time**.

---

## 🏗️ Kiến trúc hệ thống

### 📡 ESP32
- Ghi âm bằng **INMP441**
- Phát âm thanh bằng **MAX98357A**
- Gửi và nhận dữ liệu âm thanh qua **WebSocket**

### 🖥️ Server (Node.js)
- Nhận dữ liệu âm thanh từ ESP32 qua WebSocket
- Chuyển giọng nói thành văn bản bằng Vosk STT
- Gửi văn bản tới Gemini (Google AI) để nhận phản hồi
- Chuyển phản hồi thành giọng nói với Orca (Picovoice)
- Gửi lại âm thanh về ESP32 để phát ra loa

---

## 📁 Cấu trúc thư mục
    real-time-voice-chat
    ├── serverNodeJsAi/
    │   ├── .env
    │   ├── package-lock.json
    │   ├── package.json
    │   └── server.js                   # WebSocket server
    ├── aiAssistantVoice/
    │   └── aiAssistantVoice.ino        # Esp32
    ├── vosk-model-small-en-us-0.15/    # Text-to-Speech module
    ├── vosk-model-vn-0.4/              # Text-to-Speech module
    └── README.md
---

## 🚀 Hướng dẫn cài đặt

### 🔧 Server (Node.js)

Cài các dependency:
```bash
cd .\serverNodeJsAi\
npm install
```
Cài đặt mô hình Vosk:

Tải mô hình tiếng Việt (hoặc English):
https://alphacephei.com/vosk/models

Thiết lập API key cho Gemini và Picovoice (Orca)

Tạo file .env:
```ini
GEMINI_API_KEY=your_gemini_api_key
PICOVOICE_ACCESS_KEY=your_orca_api_key
```
Chạy server:
```bash
node .\server.js
```
### 📲 ESP32
Cài đặt các thư viện cần thiết

Sử dụng Arduino IDE

    Wifi.h
    WebSocketsClient.h
    freertos/FreeRTOS.h
    freertos/task.h
    driver/i2s.h

Kết nối phần cứng:

...

