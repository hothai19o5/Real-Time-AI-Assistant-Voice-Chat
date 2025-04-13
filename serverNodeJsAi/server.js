require('dotenv').config(); // Load biến môi trường từ file .env
const WebSocket = require('ws');
const vosk = require('vosk');
const fs = require('fs');
const { GoogleGenerativeAI } = require("@google/generative-ai");
const { Orca } = require('@picovoice/orca-node');


// --- Cấu hình ---
const PORT = 8080; // Port server lắng nghe
const VOSK_MODEL_PATH = "C:/Users/Hotha/Work Space/Code/RealTimeAiAssistantVoiceChat/vosk-model-small-en-us-0.15"; // Đường dẫn đến model Vosk đã tải
const VOSK_SAMPLE_RATE = 16000; // Phải khớp với ESP32 và model Vosk
const PICOVOICE_ACCESS_KEY = process.env.PICOVOICE_ACCESS_KEY; // Khóa truy cập Picovoice từ biến môi trường

// --- Kiểm tra Model Vosk ---
if (!fs.existsSync(VOSK_MODEL_PATH)) {
    console.error(`Vosk model not found at path: ${VOSK_MODEL_PATH}`);
    console.error("Please download a Vosk model and place it in the specified directory.");
    process.exit(1); // Thoát nếu không tìm thấy model
}

// --- Khởi tạo Vosk ---
vosk.setLogLevel(-1); // Tắt log chi tiết của Vosk
const model = new vosk.Model(VOSK_MODEL_PATH);

// --- Khởi tạo Picovoice Orca ---
let orca = new Orca(PICOVOICE_ACCESS_KEY);

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

// Add this function before the WebSocket connection handling

function cleanMarkdownFormatting(text) {
    // Remove bold/italic formatting
    text = text.replace(/\*\*/g, '');  // Remove **bold**
    text = text.replace(/\*/g, '');    // Remove *italic*
    text = text.replace(/\_\_/g, '');  // Remove __bold__
    text = text.replace(/\_/g, '');    // Remove _italic_

    // Remove code formatting
    text = text.replace(/```[\s\S]*?```/g, ''); // Remove code blocks
    text = text.replace(/`([^`]+)`/g, '$1');    // Remove inline code

    // Replace bullet points with proper spacing
    text = text.replace(/^\s*[\*\-]\s+/gm, ', ');

    // Replace numbered lists
    text = text.replace(/^\s*\d+\.\s+/gm, ', ');

    // Remove excess whitespace
    text = text.replace(/\n+/g, ' ');
    text = text.replace(/\s+/g, ' ');

    return text.trim();
}

// Thêm hàm này phía trên, sau hàm cleanMarkdownFormatting()

// Hàm chia nhỏ dữ liệu âm thanh thành các chunk
function chunkAudioData(audioBuffer, chunkSize = 1024) {
    const chunks = [];
    let offset = 0;

    while (offset < audioBuffer.length) {
        const end = Math.min(offset + chunkSize, audioBuffer.length);
        chunks.push(audioBuffer.slice(offset, end));
        offset = end;
    }

    return chunks;
}

// --- Xử lý kết nối Client ---
wss.on('connection', (ws) => {
    console.log('Client connected');

    // Mở Orca stream khi kết nối WebSocket
    const orcaStream = orca.streamOpen();

    // Tạo bộ nhận dạng Vosk riêng cho mỗi client
    const recognizer = new vosk.Recognizer({ model: model, sampleRate: VOSK_SAMPLE_RATE });
    console.log('Vosk Recognizer created for client.');

    let audioChunks = []; // Lưu từng chunk nhận được

    ws.on('message', async (message) => {
        // Kiểm tra xem message là binary (âm thanh) hay text (điều khiển)
        if (Buffer.isBuffer(message) && message.length == 2048) {
            audioChunks.push(message);
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

                try {

                    // Tiếp tục với nhận dạng
                    recognizer.reset();
                    recognizer.acceptWaveform(fullAudioBuffer);
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
                            let geminiText = response.text();
                            console.log(`Raw Gemini Response: "${geminiText}"`);

                            // Clean markdown formatting before sending to TTS
                            geminiText = cleanMarkdownFormatting(geminiText);
                            console.log(`Cleaned Gemini Response: "${geminiText}"`);

                            if (ws.readyState === WebSocket.OPEN) {
                                ws.send(geminiText);

                                // Send a signal to ESP32 that audio stream is starting
                                ws.send("AUDIO_STREAM_START");

                                // Thay đổi phần xử lý dữ liệu PCM từ TTS

                                // Get raw PCM from Orca TTS
                                const pcm = orcaStream.synthesize(geminiText);

                                if (pcm !== null) {
                                    // Thêm đoạn này để debug dữ liệu PCM gốc
                                    console.log(`Dữ liệu PCM gốc: ${pcm.length} mẫu, từ ${pcm[0]} đến ${pcm[pcm.length - 1]}`);

                                    // Chuyển đổi Int16Array thành Buffer đúng cách
                                    const pcmBuffer = Buffer.from(pcm.buffer, pcm.byteOffset, pcm.byteLength);

                                    // Convert PCM to WAV format - đảm bảo header đúng
                                    const wavHeaderBuffer = createWavHeader(pcmBuffer.length, {
                                        numChannels: 1,
                                        sampleRate: 16000,
                                        bitsPerSample: 16
                                    });

                                    // Concatenate header and PCM data
                                    const wavData = Buffer.concat([wavHeaderBuffer, pcmBuffer]);

                                    // Debug: in thông tin chi tiết về file WAV
                                    console.log(`WAV header: ${wavHeaderBuffer.length} bytes`);
                                    console.log(`WAV data: ${pcmBuffer.length} bytes`);
                                    console.log(`Tổng kích thước WAV: ${wavData.length} bytes`);

                                    // Lưu file để debug
                                    const debugOutputFile = `tts_output_${Date.now()}.wav`;
                                    fs.writeFileSync(debugOutputFile, wavData);

                                    // Chia thành chunks nhỏ hơn để dễ xử lý
                                    const audioChunks = chunkAudioData(wavData, 2048);
                                    console.log(`Đã chia âm thanh thành ${audioChunks.length} phần`);

                                    // Gửi từng chunk với khoảng thời gian nhỏ giữa các lần gửi
                                    const sendChunks = async () => {
                                        // Gửi chunk đầu tiên có header WAV
                                        for (let i = 0; i < audioChunks.length; i++) {
                                            if (ws.readyState !== WebSocket.OPEN) {
                                                console.log("Mất kết nối WebSocket trong quá trình gửi!");
                                                break;
                                            }

                                            ws.send(audioChunks[i]);

                                            // Đợi một khoảng thời gian nhỏ giữa các chunk để tránh tắc nghẽn
                                            await new Promise(resolve => setTimeout(resolve, 10));
                                        }

                                        // Báo hiệu kết thúc luồng âm thanh
                                        if (ws.readyState === WebSocket.OPEN) {
                                            ws.send("AUDIO_STREAM_END");
                                            console.log("Đã gửi xong dữ liệu âm thanh.");
                                        }
                                    };

                                    // Bắt đầu quá trình gửi
                                    sendChunks();
                                } else {
                                    console.log("Orca TTS trả về dữ liệu âm thanh null");
                                }

                                console.log("Sent Gemini response and audio back to client.");
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
        const flushedPcm = orcaStream.flush();
        if (flushedPcm !== null) {
            // Gửi dữ liệu PCM còn lại cho client
            ws.send(flushedPcm);
            console.log(`Flushed thêm ${flushedPcm.length} mẫu PCM.`);
        }
        orcaStream.close(); // Đóng stream
        orca.release(); // Giải phóng tài nguyên Orca

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
