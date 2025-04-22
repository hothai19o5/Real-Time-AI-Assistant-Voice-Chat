# 🤖 Real-Time AI Voice Chat using ESP32 & WebSocket

![ESP32](https://img.shields.io/badge/ESP32-RealTime-blue)
![Node.js](https://img.shields.io/badge/Node.js-Server-yellowgreen)
![Python](https://img.shields.io/badge/Python-Server-gray)
![PhoWhisper](https://img.shields.io/badge/PhoWhisper-STT-red)
![Gemini](https://img.shields.io/badge/Gemini-AI-lightgrey)
![FPT](https://img.shields.io/badge/FPT-TTS-blueviolet)

## 🧠 Mô tả dự án

**Real-Time AI Voice Chat** là một hệ thống giao tiếp hai chiều bằng giọng nói giữa con người và trí tuệ nhân tạo. Dự án sử dụng:

- ESP32 (thu & phát âm thanh)
- WebSocket (giao tiếp thời gian thực)
- PhoWhisper (speech-to-text)
- Gemini (AI chatbot)
- FPT API (text-to-speech)

Toàn bộ quá trình diễn ra hoàn toàn **real-time**.

---

## 🏗️ Kiến trúc hệ thống

### 📡 ESP32
- Ghi âm bằng **INMP441**
- Phát âm thanh bằng **MAX98357A**
- Gửi và nhận dữ liệu âm thanh qua **WebSocket**

### 🖥️ Server (Node.js)
- Nhận dữ liệu âm thanh từ ESP32 qua WebSocket
- Chuyển giọng nói thành văn bản bằng PhoWhisper STT
- Nhận dạng đó là lệnh hay là câu hỏi
- Nếu là các lệnh Hỏi giờ, Xem thời tiết, Phát nhạc thì Server sẽ xử lý
- Gửi câu hỏi text tới Gemini (Google AI) để nhận phản hồi
- Chuyển phản hồi thành giọng nói với FPT API
- Gửi lại âm thanh về ESP32 để phát ra loa

---

## 📁 Cấu trúc thư mục
    real-time-voice-chat
    ├── serverNodeJsAi/
    │   ├── node_modules
    │   ├── .env
    │   ├── package-lock.json
    │   ├── package.json
    │   └── server.js                   # WebSocket server
    ├── phowhisper_service/
    │   ├── models/                     # Speech-to-Text model
    │   ├── app.py                      # Speech-to-Text
    │   └── requirements.txt                   
    ├── aiAssistantVoice/
    │   └── aiAssistantVoice.ino        # Esp32 C++
    ├── .gitignore
    └── README.md
---

## 🚀 Hướng dẫn cài đặt

### 🔧 Server (Node.js)

Cài các dependency:
```bash
cd .\serverNodeJsAi\
npm install
cd .\phowhisper_service\
pip install -r requirements.txt
```

Thiết lập API key cho Gemini, FPT TTS và Weather

Tạo file .env:
```ini
GEMINI_API_KEY=your_gemini_api_key
FPT_TTS_API_KEY=your_fpt_tts_api_key
WEATHER_API_KEY=your_weather_api_key
```
Chạy server:
```bash
node .\server.js
python app.py
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

    Continue Updating
...
