import dotenv from 'dotenv';
dotenv.config();        // Load biến môi trường từ file .env
import { WebSocket, WebSocketServer } from 'ws';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { GoogleGenerativeAI } from "@google/generative-ai";
// import { Orca } from '@picovoice/orca-node';
import axios from 'axios';
import FormData from 'form-data';

// --- Cấu hình ---
const PORT = 8080;                                              // Port server lắng nghe
const GEMINI_API_KEY = process.env.GEMINI_API_KEY;              // Khóa API Gemini từ biến môi trường
const PHOWHISPER_SERVICE_URL = 'http://localhost:5000/transcribe'; // Định nghĩa URL cho PhoWhisper service
// Cấu hình FPT TTS API
const FPT_TTS_API_KEY = process.env.FPT_TTS_API_KEY;            // Khóa API TTS từ biến môi trường
const FPT_TTS_VOICE = process.env.FPT_TTS_VOICE;
const WEATHER_API_KEY = process.env.WEATHER_API_KEY;            // Khóa API thời tiết từ biến môi trường

// --- Khởi tạo FPT TTS ---
if(!FPT_TTS_API_KEY) {
    console.error("FPT_TTS_API_KEY not found in environment variables.");
    console.error("Please create a .env file with your FPT TTS API key.");
    process.exit(1);
}

// --- Khởi tạo Gemini ---
if (!GEMINI_API_KEY) {
    console.error("GEMINI_API_KEY not found in environment variables.");
    console.error("Please create a .env file with your Gemini API key.");
    process.exit(1);
}
const genAI = new GoogleGenerativeAI(GEMINI_API_KEY);
const geminiModel = genAI.getGenerativeModel({ model: "gemini-2.0-flash" });

// --- Tạo WebSocket Server ---
const wss = new WebSocketServer({
    port: PORT,
    perMessageDeflate: false // Tắt nén dữ liệu, giảm độ trễ, giảm tải CPU 
});
console.log(`WebSocket server started on port ${PORT}`);

// --- Hàm xử lý định dạng Markdown trong phản hồi của Gemini ---
function cleanMarkdownFormatting(text) {
    // Xóa định dạng in đậm, nghiêng
    text = text.replace(/\*\*/g, '');  // Remove **bold**
    text = text.replace(/\*/g, '');    // Remove *italic*
    text = text.replace(/\_\_/g, '');  // Remove __bold__
    text = text.replace(/\_/g, '');    // Remove _italic_

    // Xóa các định dạng code
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

// Hàm chia nhỏ dữ liệu âm thanh thành các chunk
function chunkAudioData(audioBuffer, chunkSize = 2048) {
    const chunks = [];
    let offset = 0;

    while (offset < audioBuffer.length) {
        const end = Math.min(offset + chunkSize, audioBuffer.length);
        chunks.push(audioBuffer.slice(offset, end));
        offset = end;
    }

    return chunks;
}

// Hàm tạo header WAV
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

// Function to detect commands in recognized text
function detectCommand(text) {
    // Normalize text to lowercase for easier matching
    const normalizedText = text.toLowerCase().trim();
    
    // Music command detection
    if (normalizedText.includes('bật bài hát') || normalizedText.includes('phát bài hát')) {
        const songName = extractSongName(normalizedText);
        return { 
            type: 'MUSIC', 
            songName: songName 
        };
    }
    
    // Weather command detection
    if (normalizedText.includes('thời tiết hôm nay')) {
        return { 
            type: 'WEATHER',
            location: extractLocation(normalizedText) || 'Hà Nội' // Default location
        };
    }

    // Time command detection
    if (normalizedText.includes('mấy giờ rồi')) {
        return { 
            type: 'TIME' 
        };
    }
    
    // No command detected
    return { type: 'NORMAL' };
}

// Extract song name from command
function extractSongName(text) {
    // Pattern to match "bật bài hát X" or "phát bài hát X"
    const musicRegex = /(bật|phát) bài hát\s+(.+)/i;
    const match = text.match(musicRegex);
    
    if (match && match[2]) {
        return match[2].trim();
    }
    return null;
}

// Extract location from weather query
function extractLocation(text) {
    // Pattern to match "thời tiết ở X" or "thời tiết tại X"
    const weatherRegex = /thời tiết\s+(ở|tại)?\s*(.+)/i;
    const match = text.match(weatherRegex);
    
    if (match && match[2]) {
        return match[2].trim();
    }
    return null;
}

// Handle music playback
async function handleMusicCommand(songName, ws) {
    try {
        const response = `Đang phát bài hát ${songName}. Xin vui lòng đợi một chút.`;
        await sendTTSResponse(response, ws);

        // Đường dẫn đến thư mục music
        const musicDir = "C:/Users/Hotha/Work Space/Code/RealTimeAiAssistantVoiceChat/music";

        // Lọc chỉ lấy file .wav
        const files = fs.readdirSync(musicDir)
            .filter(file => file.toLowerCase().endsWith('.wav'));

        // Chọn ngẫu nhiên một file
        const musicFilePath = path.join(musicDir, files[0]);

        console.log(`Đang phát file nhạc: ${files[0]}`);
        
        // Đọc file âm thanh
        const audioBuffer = fs.readFileSync(musicFilePath);
        
        // Gửi thông báo bắt đầu phát nhạc
        ws.send("AUDIO_STREAM_START");
        
        // Chia thành các chunk và gửi qua WebSocket
        const audioChunks = chunkAudioData(audioBuffer, 2048);
        
        for (let i = 0; i < audioChunks.length; i++) {
            if (ws.readyState !== WebSocket.OPEN) break;
            ws.send(audioChunks[i]);
            // Tốc độ gửi nhanh hơn so với TTS để phát nhạc
            await new Promise(resolve => setTimeout(resolve, 20));
        }
        
        // Kết thúc stream
        if (ws.readyState === WebSocket.OPEN) {
            const silenceBuffer = Buffer.alloc(1600, 0); // 50ms of silence
            ws.send(silenceBuffer);
            await new Promise(resolve => setTimeout(resolve, 100));
            ws.send("AUDIO_STREAM_END");
            
            // Thông báo kết thúc phát nhạc
            const musicInfo = `Đã phát bài hát ${path.basename(randomFile, '.wav')}.`;
            await sendTTSResponse(musicInfo, ws);
        }
        
    } catch (error) {
        console.error("Error handling music command:", error);
        await sendTTSResponse("Xin lỗi, không thể phát bài hát này. Vui lòng thử lại.", ws);
    }
}

// Handle weather information
async function handleWeatherCommand(location, ws) {
    try {
        const weatherApiKey = WEATHER_API_KEY;
        const weatherResponse = await fetchWeatherData(location, weatherApiKey);
        
        await sendTTSResponse(weatherResponse, ws);
        
    } catch (error) {
        console.error("Error handling weather command:", error);
        await sendTTSResponse("Xin lỗi, không thể lấy thông tin thời tiết. Vui lòng thử lại sau.", ws);
    }
}

// Handle time information
async function handleTimeCommand(ws) {
    try {
        // Lấy thời gian hiện tại
        const now = new Date();
        
        // Định dạng giờ theo kiểu Việt Nam
        const hours = now.getHours();
        const minutes = now.getMinutes();
        
        // Tạo chuỗi phản hồi
        const timeResponse = `Bây giờ là ${hours} giờ ${minutes} phút.`;
        
        await sendTTSResponse(timeResponse, ws);
        
    } catch (error) {
        console.error("Error handling time command:", error);
        await sendTTSResponse("Xin lỗi, không thể xác định thời gian hiện tại.", ws);
    }
}

// Fetch weather data from API
async function fetchWeatherData(location, apiKey) {
    try {
        // Using WeatherAPI.com instead of OpenWeatherMap
        const response = await axios.get(`https://api.weatherapi.com/v1/current.json`, {
            params: {
                q: location,
                key: apiKey,
                lang: 'vi'
            }
        });
        
        const data = response.data;
        return `Thời tiết hiện tại ở ${data.location.name}: ${data.current.condition.text}. 
                Nhiệt độ ${Math.round(data.current.temp_c)} độ xê, 
                Độ ẩm ${data.current.humidity} phần trăm, 
                Tốc độ gió ${data.current.wind_kph * 0.277778} mét trên giây.`;
    } catch (error) {
        console.error("Weather API error:", error);
        throw new Error("Không thể truy cập thông tin thời tiết");
    }
}

// Helper function to send TTS response using FPT API
async function sendTTSResponse(text, ws) {
    if (ws.readyState !== WebSocket.OPEN) return;
    
    try {
        ws.send("AUDIO_STREAM_START");
        
        // Lấy API key và voice từ biến môi trường
        const apiKey = FPT_TTS_API_KEY;
        const voice = FPT_TTS_VOICE;
        
        // Chuẩn bị header với format=wav để nhận WAV thay vì MP3
        const headers = {
            'api_key': apiKey,
            'voice': voice,
            'format': 'wav', 
            'Cache-Control': 'no-cache'
        };
        
        console.log("Calling FPT TTS API...");
        
        // Gọi API FPT để chuyển text thành speech
        const response = await axios.post(
            'https://api.fpt.ai/hmi/tts/v5',
            text,
            { headers }
        );
        
        console.log("FPT TTS API Response:", response.data);
        
        if (response.data.error === 0) {
            const audioUrl = response.data.async;
            
            // Chờ một chút để đảm bảo file đã được tạo xong
            await new Promise(resolve => setTimeout(resolve, 5000));
            
            // Tải file WAV từ URL
            console.log("Downloading WAV audio from:", audioUrl);
            const audioResponse = await axios.get(audioUrl, { responseType: 'arraybuffer' });
            const audioBuffer = Buffer.from(audioResponse.data);
            console.log(`Received audio data: ${audioBuffer.length} bytes`);
            
            // Chia thành các chunk và gửi qua WebSocket
            const audioChunks = chunkAudioData(audioBuffer, 2048);
            
            for (let i = 0; i < audioChunks.length; i++) {
                if (ws.readyState !== WebSocket.OPEN) break;
                ws.send(audioChunks[i]);
                await new Promise(resolve => setTimeout(resolve, 50));
            }
            
            // Kết thúc stream
            if (ws.readyState === WebSocket.OPEN) {
                const silenceBuffer = Buffer.alloc(1600, 0); // 50ms of silence
                ws.send(silenceBuffer);
                await new Promise(resolve => setTimeout(resolve, 100));
                ws.send("AUDIO_STREAM_END");
            }
        } else {
            console.error("FPT TTS API Error:", response.data);
            throw new Error(`FPT API error: ${response.data.message || 'Unknown error'}`);
        }
    } catch (error) {
        console.error("Error sending TTS response:", error);
        
        // Gửi thông báo lỗi nếu ws vẫn mở
        if (ws.readyState === WebSocket.OPEN) {
            ws.send("AUDIO_STREAM_END");
            ws.send(`Error generating speech: ${error.message}`);
        }
    }
}

// Hàm gọi PhoWhisper service
async function transcribeAudio(audioBuffer) {
    try {
        // Tạo WAV header
        const wavHeaderBuffer = createWavHeader(audioBuffer.length, {
            numChannels: 1,
            sampleRate: 16000,
            bitsPerSample: 16
        });
        
        // Tạo file WAV đầy đủ
        const wavData = Buffer.concat([wavHeaderBuffer, audioBuffer]);
        
        console.log(`Sending audio to PhoWhisper: ${wavData.length} bytes`);
        
        // Tạo form data để gửi file
        const formData = new FormData();
        formData.append('audio', Buffer.from(wavData), {
            filename: 'audio.wav',
            contentType: 'audio/wav'
        });
        
        try {
            // Gọi API Python với timeout dài hơn
            const response = await axios.post(PHOWHISPER_SERVICE_URL, formData, {
                headers: {
                    ...formData.getHeaders()
                },
                maxContentLength: Infinity,
                maxBodyLength: Infinity,
                timeout: 30000  // 30 giây timeout
            });
            
            return response.data;
        } catch (axiosError) {
            if (axiosError.response) {
                // Máy chủ đã phản hồi với mã lỗi
                console.error("PhoWhisper service error details:", 
                              axiosError.response.status,
                              axiosError.response.data);
            }
            throw axiosError;
        }
    } catch (error) {
        console.error("Error calling PhoWhisper service:", error.message);
        throw error;
    }
}

// --- Xử lý kết nối Client ---
wss.on('connection', (ws) => {
    console.log('Client connected');

    let audioChunks = []; // Lưu từng chunk nhận được

    ws.on('message', async (message) => {
        // Kiểm tra xem message là binary (âm thanh) hay text (điều khiển)
        if (Buffer.isBuffer(message) && message.length == 2048) {
            audioChunks.push(message);
            // Không cần gửi cho Vosk nữa
        } else {
            const textMessage = message.toString();
            console.log('Received control message:', textMessage);

            if (textMessage === 'END_OF_STREAM') {
                // Ghép toàn bộ buffer lại
                const fullAudioBuffer = Buffer.concat(audioChunks);
                console.log(`Final audio size: ${fullAudioBuffer.length} bytes`);

                try {
                    // Gọi PhoWhisper service thay vì Vosk
                    const transcriptionResult = await transcribeAudio(fullAudioBuffer);
                    console.log("PhoWhisper Result:", transcriptionResult);
                    
                    if (transcriptionResult.success && transcriptionResult.text) {
                        const recognizedText = transcriptionResult.text;
                        console.log(`Recognized Text: "${recognizedText}"`);
                        
                        // Phần còn lại của code giữ nguyên
                        const commandResult = detectCommand(recognizedText);
                        
                        if (commandResult.type === 'MUSIC') {
                            console.log(`Music command detected for song: "${commandResult.songName}"`);
                            await handleMusicCommand(commandResult.songName, ws);
                        } 
                        else if (commandResult.type === 'WEATHER') {
                            console.log(`Weather command detected for location: "${commandResult.location}"`);
                            await handleWeatherCommand(commandResult.location, ws);
                        }
                        else if (commandResult.type === 'TIME') {
                            console.log(`Time command detected`);
                            await handleTimeCommand(ws);
                        }
                        else {
                            // No specific command detected, process with Gemini as before
                            try {
                                console.log("Sending text to Gemini...");
                                const result = await geminiModel.generateContent(recognizedText);
                                const response = result.response;
                                let geminiText = response.text();
                                console.log(`Raw Gemini Response: "${geminiText}"`);

                                // Clean markdown formatting before sending to TTS
                                geminiText = cleanMarkdownFormatting(geminiText);
                                // console.log(`Cleaned Gemini Response: "${geminiText}"`);

                                // Continue with existing TTS logic...
                                if (ws.readyState === WebSocket.OPEN) {
                                    // await sendTTSResponse("oke i'm ready to respone you", ws);
                                    await sendTTSResponse(geminiText, ws);
                                    console.log("Sent Gemini response and audio back to client.");
                                } else {
                                    console.log("Client disconnected before sending Gemini response.");
                                }
                            } catch (error) {
                                console.error("Error calling Gemini API:", error);
                                if (ws.readyState === WebSocket.OPEN) {
                                    ws.send(`Error processing request: ${error.message || 'Gemini API error'}`);
                                }
                            }
                        }
                    } else {
                        console.log("Empty transcription or error from PhoWhisper service");
                        if (ws.readyState === WebSocket.OPEN) {
                            sendTTSResponse("Xin lỗi, tôi không nghe rõ. Vui lòng thử lại.", ws);
                        }
                    }
                } catch (error) {
                    console.error("Error processing audio:", error);
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send(`Error processing audio: ${error.message || 'Unknown error'}`);
                    }
                } finally {
                    // Đặt lại mảng audioChunks để giải phóng bộ nhớ
                    audioChunks = [];
                    console.log("Audio chunks array reset");
                }
            }
        }
    });

    ws.on('close', () => {
    
        console.log('Client disconnected');
        
        // Đảm bảo giải phóng bộ nhớ
        audioChunks = [];
        console.log('Audio chunks cleared.');
    });
    
    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
        // Đảm bảo giải phóng bộ nhớ
        audioChunks = [];
        console.log('Audio chunks cleared due to error.');
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

        // Đảm bảo server đã đóng hoàn toàn
        setTimeout(() => {
            console.log("Exiting process...");
            process.exit(0);
        }, 1000); // Delay nhỏ để đảm bảo việc đóng kết nối hoàn tất
    });
});
