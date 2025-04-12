require('dotenv').config(); // Load biến môi trường từ file .env
const WebSocket = require('ws');
const vosk = require('vosk');
const fs = require('fs');
const { GoogleGenerativeAI } = require("@google/generative-ai");

// --- Cấu hình ---
const PORT = 8080; // Port server lắng nghe
const VOSK_MODEL_PATH = "C:/Users/Hotha/Work Space/Code/RealTimeAiAssistantVoiceChat/vosk-model-small-en-us-0.15"; // Đường dẫn đến model Vosk đã tải
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

        } else {
            const textMessage = message.toString();
            console.log('Received control message:', textMessage);

            if (textMessage === 'END_OF_STREAM') {
                // Ghép toàn bộ buffer lại
                const fullAudioBuffer = Buffer.concat(audioChunks);
                console.log(`Final audio size: ${fullAudioBuffer.length} bytes`);

                function createWavHeader(dataLength, options = {}) {
                    const numChannels = options.numChannels || 1;
                    const sampleRate = options.sampleRate || 16000;
                    const bitsPerSample = options.bitsPerSample || 16;
                    const byteRate = sampleRate * numChannels * bitsPerSample / 8;
                    const blockAlign = numChannels * bitsPerSample / 8;
                    const dataSize = dataLength;
                    const chunkSize = 36 + dataSize;
                
                    const buffer = Buffer.alloc(44);
                    buffer.write('RIFF', 0);                 // ChunkID
                    buffer.writeUInt32LE(chunkSize, 4);      // ChunkSize
                    buffer.write('WAVE', 8);                 // Format
                    buffer.write('fmt ', 12);                // Subchunk1ID
                    buffer.writeUInt32LE(16, 16);            // Subchunk1Size (PCM)
                    buffer.writeUInt16LE(1, 20);             // AudioFormat (PCM = 1)
                    buffer.writeUInt16LE(numChannels, 22);   // NumChannels
                    buffer.writeUInt32LE(sampleRate, 24);    // SampleRate
                    buffer.writeUInt32LE(byteRate, 28);      // ByteRate
                    buffer.writeUInt16LE(blockAlign, 32);    // BlockAlign
                    buffer.writeUInt16LE(bitsPerSample, 34); // BitsPerSample
                    buffer.write('data', 36);                // Subchunk2ID
                    buffer.writeUInt32LE(dataSize, 40);      // Subchunk2Size
                
                    return buffer;
                }
                

                // --- Ghi file WAV để kiểm tra âm thanh thô từ ESP32 ---
                const wavHeaderBuffer = createWavHeader(fullAudioBuffer.length, {
                    numChannels: 1,
                    sampleRate: VOSK_SAMPLE_RATE,
                    bitsPerSample: 16
                });
                
                const wavData = Buffer.concat([
                    wavHeaderBuffer,
                    fullAudioBuffer
                ]);                

                const outputFile = `received_audio_${Date.now()}.wav`;
                fs.writeFileSync(outputFile, wavData);
                console.log(`Saved received audio to: ${outputFile}`);

                // Client đã gửi xong âm thanh
                const finalResult = recognizer.finalResult();

                const tmp = require('tmp');
                const sox = require('sox-audio');
                const { Readable } = require('stream');

                // Gộp toàn bộ âm thanh lại
                console.log(`Final audio size: ${fullAudioBuffer.length} bytes`);

                // Tạo file tạm WAV đầu vào
                const tmpInputFile = tmp.tmpNameSync({ postfix: '.wav' });
                const tmpOutputFile = tmp.tmpNameSync({ postfix: '.wav' });

                // Tạo WAV header cho file tạm đầu vào
                const tmpInputWavHeaderBuffer = createWavHeader(fullAudioBuffer.length, {
                    numChannels: 1,
                    sampleRate: VOSK_SAMPLE_RATE,
                    bitsPerSample: 16
                });

                // Ghi file WAV đầy đủ (header + data) vào file tạm
                const tmpInputWavData = Buffer.concat([
                    tmpInputWavHeaderBuffer,
                    fullAudioBuffer
                ]);

                console.log(`Ghi file WAV tạm với header (${tmpInputWavHeaderBuffer.length} bytes) + data (${fullAudioBuffer.length} bytes)`);
                fs.writeFileSync(tmpInputFile, tmpInputWavData);
                console.log(`Đã ghi file tạm thành công: ${tmpInputFile}`);

                // Debug: Lưu thêm file tạm để kiểm tra
                const debugInputFile = `debug_input_${Date.now()}.wav`;
                fs.writeFileSync(debugInputFile, tmpInputWavData);
                console.log(`Đã lưu file debug input: ${debugInputFile}`);

                // Xử lý bằng sox: noise reduction, resample, normalize
                const { exec, execSync } = require('child_process');

                // Phương án thay thế nếu không có SoX
                let processedBuffer;

                try {
                    // Kiểm tra xem SoX có được cài đặt không
                    try {
                        execSync('sox --version', { stdio: 'ignore' });
                        // SoX được cài đặt, sử dụng để xử lý
                        const soxCommand = `sox ${tmpInputFile} -r 16000 -b 16 -c 1 -e signed-integer -L ${tmpOutputFile}`;
                        execSync(soxCommand);
                        processedBuffer = fs.readFileSync(tmpOutputFile);
                        console.log("Xử lý âm thanh thành công với SoX");
                        
                        // Lưu file âm thanh đã xử lý để so sánh
                        const processedWavHeaderBuffer = createWavHeader(processedBuffer.length, {
                            numChannels: 1,
                            sampleRate: VOSK_SAMPLE_RATE,
                            bitsPerSample: 16
                        });
                        
                        const processedWavData = Buffer.concat([
                            processedWavHeaderBuffer,
                            processedBuffer
                        ]);
                        
                        const processedOutputFile = `processed_audio_${Date.now()}.wav`;
                        fs.writeFileSync(processedOutputFile, processedWavData);
                        console.log(`Lưu âm thanh đã xử lý vào: ${processedOutputFile}`);
                    } catch (e) {
                        console.error("Lỗi khi xử lý với SoX:", e.message);
                        console.log("SoX không có sẵn. Sử dụng âm thanh chưa xử lý...");
                        processedBuffer = fullAudioBuffer;
                    }
                    
                    // Tiếp tục với nhận dạng
                    recognizer.reset();
                    recognizer.acceptWaveform(processedBuffer);
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
                        } finally {
                            // Xóa tệp tạm sau khi xử lý xong
                            try {
                                fs.unlinkSync(tmpInputFile);
                                fs.unlinkSync(tmpOutputFile);
                                console.log("Temporary files deleted successfully");
                            } catch (err) {
                                console.error("Error deleting temporary files:", err);
                            }
                            
                            // Đặt lại mảng audioChunks để giải phóng bộ nhớ
                            audioChunks = [];
                            console.log("Audio chunks array reset");
                        }
                    } else {
                        console.log("Vosk recognized empty text.");
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send("Xin lỗi, tôi không nghe rõ. Vui lòng thử lại."); // Gửi phản hồi mặc định bằng tiếng Việt
                        }
                        // Đặt lại mảng audioChunks ngay cả khi không nhận dạng được
                        audioChunks = [];
                    }
                }
                catch (error) {
                    console.error("Error processing audio:", error);
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send(`Error processing audio: ${error.message || 'Unknown error'}`);
                    }
                } finally {
                    // Giải phóng tài nguyên của recognizer
                    recognizer.free();
                    console.log('Vosk Recognizer freed after processing.');
                }

            } else {
                console.log("Received unknown text message:", textMessage);
            }
        }
    });

    ws.on('close', () => {
        console.log('Client disconnected');
        // Giải phóng tài nguyên của recognizer khi client ngắt kết nối
        recognizer.free();
        // Đảm bảo giải phóng bộ nhớ
        audioChunks = [];
        console.log('Vosk Recognizer freed and audio chunks cleared.');
    });

    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
        // Đảm bảo giải phóng recognizer nếu có lỗi xảy ra
        recognizer.free();
        // Đảm bảo giải phóng bộ nhớ
        audioChunks = [];
        console.log('Vosk Recognizer freed and audio chunks cleared due to error.');
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
