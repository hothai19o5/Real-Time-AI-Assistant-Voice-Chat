require('dotenv').config(); // Load biến môi trường từ file .env
const WebSocket = require('ws');
const vosk = require('vosk');
const fs = require('fs');
const { GoogleGenerativeAI } = require("@google/generative-ai");

// --- Cấu hình ---
const PORT = 8080; // Port server lắng nghe
const VOSK_MODEL_PATH = "C:/Users/Hotha/Work Space/Code/Arduino/vosk-model-small-en-us-0.15"; // Đường dẫn đến model Vosk đã tải
const VOSK_SAMPLE_RATE = 16000; // Phải khớp với ESP32 và model Vosk

// --- Kiểm tra Model Vosk ---
if (!fs.existsSync(VOSK_MODEL_PATH)) {
    console.error(`Vosk model not found at path: ${VOSK_MODEL_PATH}`);
    console.error("Please download a Vosk model and place it in the specified directory.");
    process.exit(1); // Thoát nếu không tìm thấy model
}

// --- Khởi tạo Vosk ---
vosk.setLogLevel(-1); // Tắt log chi tiết của Vosk
const model = new vosk.Model(VOSK_MODEL_PATH);

// --- Khởi tạo Gemini ---
const geminiApiKey = process.env.GEMINI_API_KEY;
if (!geminiApiKey) {
    console.error("GEMINI_API_KEY not found in environment variables.");
    console.error("Please create a .env file with your Gemini API key.");
    process.exit(1);
}
const genAI = new GoogleGenerativeAI(geminiApiKey);
const geminiModel = genAI.getGenerativeModel({ model: "gemini-2.0-flash" }); // Hoặc model khác phù hợp

// --- Tạo WebSocket Server ---
const wss = new WebSocket.Server({ 
    port: PORT,
    perMessageDeflate: false // <-- Thêm dòng này để tắt nén 
});
console.log(`WebSocket server started on port ${PORT}`);

// --- Xử lý kết nối Client ---
wss.on('connection', (ws) => {
    console.log('Client connected');

    // Tạo bộ nhận dạng Vosk riêng cho mỗi client
    const recognizer = new vosk.Recognizer({ model: model, sampleRate: VOSK_SAMPLE_RATE });
    console.log('Vosk Recognizer created for client.');

    let audioChunks = []; // Lưu từng chunk nhận được

    ws.on('message', async (message) => {
        // Kiểm tra xem message là binary (âm thanh) hay text (điều khiển)
        console.log(`Received data: ${message.length} bytes`);
        if (Buffer.isBuffer(message) && message.length == 2048) {
            audioChunks.push(message);
            console.log(`Stored chunk: ${message.length} bytes, total: ${audioChunks.reduce((sum, chunk) => sum + chunk.length, 0)} bytes`);
            // Đây là dữ liệu âm thanh (PCM 16-bit)
            console.log(`Received audio chunk: ${message.length} bytes`);
            recognizer.acceptWaveform(message);
            // Có thể lấy kết quả tạm thời nếu cần:
            // console.log("Partial Result:", recognizer.partialResult());
            // in ra chunk âm thanh nhận được
            // console.log(`Audio chunk: ${message.toString('base64')}`); // Chuyển đổi sang base64 nếu cần
        } else {
            const textMessage = message.toString();
            console.log('Received control message:', textMessage);

            if (textMessage === 'END_OF_STREAM') {
                // Ghép toàn bộ buffer lại
                const fullAudioBuffer = Buffer.concat(audioChunks);
                console.log(`Final audio size: ${fullAudioBuffer.length} bytes`);
                // Lưu ra file kiểm tra nếu cần
                // fs.writeFileSync(`received_audio_${Date.now()}.wav`, fullAudioBuffer);
                // Client đã gửi xong âm thanh
                const finalResult = recognizer.finalResult();
                const recognizedText = finalResult.text;
                console.log("Vosk Final Result:", finalResult);

                if (recognizedText && recognizedText.trim().length > 0) {
                    console.log(`Recognized Text: "${recognizedText}"`);

                    // Gửi text tới Gemini
                    try {
                        console.log("Sending text to Gemini...");
                        const result = await geminiModel.generateContent(recognizedText);
                        const response = await result.response;
                        const geminiText = response.text();
                        console.log(`Gemini Response: "${geminiText}"`);

                        // Gửi phản hồi Gemini về ESP32
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(geminiText);
                            console.log("Sent Gemini response back to client.");
                        } else {
                            console.log("Client disconnected before sending Gemini response.");
                        }
                    } catch (error) {
                        console.error("Error calling Gemini API:", error);
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(`Error processing request: ${error.message || 'Gemini API error'}`);
                        }
                    }
                } else {
                    console.log("Vosk recognized empty text.");
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send("Sorry, I didn't catch that."); // Gửi phản hồi mặc định
                    }
                }
                // Có thể reset recognizer nếu muốn client gửi tiếp mà không cần kết nối lại
                // recognizer.reset();
            } else {
                console.log("Received unknown text message:", textMessage);
            }
        }
    });

    ws.on('close', () => {
        console.log('Client disconnected');
        // Giải phóng tài nguyên của recognizer khi client ngắt kết nối
        recognizer.free();
        console.log('Vosk Recognizer freed.');
    });

    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
        // Đảm bảo giải phóng recognizer nếu có lỗi xảy ra
        recognizer.free();
        console.log('Vosk Recognizer freed due to error.');
    });
});

// Xử lý tín hiệu đóng server (Ctrl+C)
process.on('SIGINT', () => {
    console.log("\nShutting down server...");

    // Đảm bảo rằng tất cả các kết nối WebSocket đều được đóng
    wss.clients.forEach(client => {
        client.close(() => {
            console.log("Client connection closed.");
        });
    });

    // Đóng WebSocket server
    wss.close(() => {
        console.log("WebSocket server closed.");
        
        // Giải phóng model Vosk
        model.free();
        console.log("Vosk model freed.");

        // Đảm bảo server đã đóng hoàn toàn
        setTimeout(() => {
            console.log("Exiting process...");
            process.exit(0);
        }, 1000); // Delay nhỏ để đảm bảo việc đóng kết nối hoàn tất
    });
});
