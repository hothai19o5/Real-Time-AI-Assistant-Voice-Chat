import dotenv from 'dotenv';
dotenv.config();        // Load biến môi trường từ file .env
import { WebSocket, WebSocketServer } from 'ws';    // Import WebSocket từ thư viện ws
import fs from 'fs';            // Import fs để làm việc với file hệ thống
import path from 'path';        // Import path để xử lý đường dẫn file
import { GoogleGenerativeAI } from "@google/generative-ai";         // Import Google Generative AI SDK
import axios from 'axios';      // Import axios để thực hiện các yêu cầu HTTP
import FormData from 'form-data';       // Import FormData để gửi dữ liệu dạng form

// --- Cấu hình ---
const PORT = 8080;                                              // Port server lắng nghe
const GEMINI_API_KEY = process.env.GEMINI_API_KEY;              // Khóa API Gemini từ biến môi trường
const PHOWHISPER_SERVICE_URL = 'http://localhost:5000/transcribe'; // Định nghĩa URL cho PhoWhisper service
// Cấu hình FPT TTS API
const FPT_TTS_API_KEY = process.env.FPT_TTS_API_KEY;            // Khóa API TTS từ biến môi trường
const FPT_TTS_VOICE = process.env.FPT_TTS_VOICE;
const WEATHER_API_KEY = process.env.WEATHER_API_KEY;            // Khóa API thời tiết từ biến môi trường

// Kiểm tra API Key
if (!FPT_TTS_API_KEY) {
    console.error("FPT_TTS_API_KEY not found in environment variables.");
    console.error("Please create a .env file with your FPT TTS API key.");
    process.exit(1);
}
if (!GEMINI_API_KEY) {
    console.error("GEMINI_API_KEY not found in environment variables.");
    console.error("Please create a .env file with your Gemini API key.");
    process.exit(1);
}
if (!WEATHER_API_KEY) {
    console.error("WEATHER_API_KEY not found in environment variables.");
    console.error("Please create a .env file with your Weather API key.");
    process.exit(1);
}
// Map các địa danh tiếng Việt sang tiếng Anh cho WeatherAPI
const locationMap = {
    "Hà Nội": "Hanoi", "Hồ Chí Minh": "Ho Chi Minh City", "Đà Nẵng": "Da Nang", "Cần Thơ": "Can Tho", "Hải Phòng": "Hai Phong", "Huế": "Hue", "Biên Hòa": "Bien Hoa", "Nha Trang": "Nha Trang", "Vũng Tàu": "Vung Tau", "Quảng Ninh": "Quang Ninh", "Thanh Hóa": "Thanh Hoa", "Nghệ An": "Nghe An", "Hà Tĩnh": "Ha Tinh", "Thái Nguyên": "Thai Nguyen", "Cà Mau": "Ca Mau", "Bắc Ninh": "Bac Ninh", "Nam Định": "Nam Dinh", "Bến Tre": "Ben Tre", "Long An": "Long An", "Kiên Giang": "Kien Giang", "Đồng Nai": "Dong Nai", "Hưng Yên": "Hung Yen", "Thái Bình": "Thai Binh", "Lạng Sơn": "Lang Son", "Lào Cai": "Lao Cai", "Hà Giang": "Ha Giang", "Điện Biên": "Dien Bien", "Kon Tum": "Kon Tum", "Gia Lai": "Gia Lai", "Đắk Lắk": "Dak Lak", "Đắk Nông": "Dak Nong", "Ninh Thuận": "Ninh Thuan", "Bình Thuận": "Binh Thuan", "Bắc Giang": "Bac Giang", "Hà Nam": "Ha Nam", "Quảng Bình": "Quang Binh", "Quảng Trị": "Quang Tri", "Thừa Thiên Huế": "Thua Thien Hue", "Sóc Trăng": "Soc Trang", "Trà Vinh": "Tra Vinh", "Hậu Giang": "Hau Giang", "Bạc Liêu": "Bac Lieu", "Đồng Tháp": "Dong Thap", "An Giang": "An Giang", "Tiền Giang": "Tien Giang", "Vĩnh Long": "Vinh Long", "Cao Bằng": "Cao Bang", "Tuyên Quang": "Tuyen Quang", "Yên Bái": "Yen Bai", "Phú Thọ": "Phu Tho", "Lai Châu": "Lai Chau", "Sơn La": "Son La", "Hòa Bình": "Hoa Binh", "Ninh Bình": "Ninh Binh",
};

const __dirname = path.dirname(new URL(import.meta.url).pathname); // Khởi tạo __dirname
// Khởi tạo Gemini
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

// Hàm phát hiện lệnh từ TEXT
function detectCommand(text) {
    // Chuyển text về chữ thường và loại bỏ khoảng trắng đầu cuối
    const normalizedText = text.toLowerCase().trim();

    // Lọc lệnh bật nhạc
    if (normalizedText.includes('bật bài hát') || normalizedText.includes('phát bài hát')
        || normalizedText.includes('mở bài hát') || normalizedText.includes('mở nhạc')
        || normalizedText.includes('phát nhạc') || normalizedText.includes('bật nhạc')) {
        const songName = extractSongName(normalizedText) || 'ngẫu nhiên';        // Lọc tên bài hát từ lệnh, nếu không có thì mặc định là 'ngẫu nhiên'
        return {
            type: 'MUSIC',
            songName: songName
        };
    }

    // Lọc lệnh xem thời tiết
    if (normalizedText.includes('thời tiết hôm nay')) {
        return {
            type: 'WEATHER',
            location: extractLocation(normalizedText) || 'Hà Nội' // Lọc địa điểm từ lệnh, nếu không có thì mặc định là 'Hà Nội'
        };
    }

    // Lọc lệnh xem thời gian
    if (normalizedText.includes('mấy giờ rồi') || normalizedText.includes('giờ hiện tại') ||
        normalizedText.includes('thời gian hiện tại') || normalizedText.includes('hiện tại là mấy giờ')) {
        return {
            type: 'TIME'
        };
    }

    // Không phát hiện ra lệnh thì là 1 câu hỏi bình thường, gửi cho Gemini xử lý
    return { type: 'NORMAL' };
}

// Lọc tên bài hát từ lệnh
function extractSongName(text) {
    // Lọc theo mẫu "bật bài hát X" hoặc "phát bài hát X" hoặc "mở bài hát X"
    // Biểu thức chính quy (regex) để tìm tên bài hát
    const musicRegex = /(bật|phát|mở) bài hát\s+(.+)/i;
    const match = text.match(musicRegex);

    if (match && match[2]) {
        return match[2].trim();
    }
    return null;
}

// Lọc địa điểm từ lệnh
function extractLocation(text) {
    // Lọc theo mẫu "thời tiết hôm nay ở X" hoặc "thời tiết hôm nay tại X"
    const weatherRegex = /thời tiết hôm nay\s+(ở|tại)?\s*(.+)/i;
    const match = text.match(weatherRegex);

    if (match && match[2]) {
        return match[2].trim();
    }
    return null;
}

// Hàm phát nhạc từ file âm thanh
async function playSoundFile(filePath, ws) {
    try {
        if (!fs.existsSync(filePath)) { // Kiểm tra file có tồn tại không
            throw new Error(`File not found: ${filePath}`);
        }

        const fileName = path.basename(filePath);   // Lấy tên file từ đường dẫn
        console.log(`Playing audio file: ${fileName}`);

        const audioData = fs.readFileSync(filePath);    // Đọc file âm thanh vào buffer

        ws.send("AUDIO_STREAM_START");

        const audioChunks = chunkAudioData(audioData, 2048);    // Chia nhỏ buffer thành các chunk
        const totalChunks = audioChunks.length;

        console.log(`Audio file size: ${audioData.length} bytes, split into ${totalChunks} chunks`);
        for (let i = 0; i < totalChunks; i++) { // Gửi từng chunk âm thanh
            if (ws.readyState !== WebSocket.OPEN) { // Nếu WebSocket đã đóng thì dừng lại
                console.log("WebSocket connection closed during playback, aborting");
                return false;
            }

            ws.send(audioChunks[i]);

            if (i % 100 === 0 || i === totalChunks - 1) {   // Log tiến độ phát file âm thanh
                const progress = Math.floor((i / totalChunks) * 100);
                console.log(`Playback progress: ${progress}% (${i}/${totalChunks} chunks)`);
            }

            if (i % 50 === 0) { // Nếu đã gửi 50 chunk thì tạm dừng lâu hơn một chút
                await new Promise(resolve => setTimeout(resolve, 100));
            } else {
                await new Promise(resolve => setTimeout(resolve, 30));
            }
        }

        if (ws.readyState === WebSocket.OPEN) {
            console.log("Sending silence and ending playback");
            const silenceBuffer = Buffer.alloc(1600, 0); // 50ms trống âm thanh, toàn bit 0
            ws.send(silenceBuffer);
            await new Promise(resolve => setTimeout(resolve, 300)); // Đợi một chút để gửi xong
            ws.send("AUDIO_STREAM_END");
        }

        console.log(`Completed playback of ${fileName}`);
        return true;
    } catch (error) {
        console.error(`Error playing sound file: ${error.message}`);
        throw error;
    }
}

// Hàm chuẩn hóa tên file âm thanh về không dấu và viết thường vd: "Nhac hay.wav" -> "nhac_hay.wav"
function normalizeFileName(songName) {
    return songName
        .normalize("NFD")                       // Tách dấu
        .replace(/[\u0300-\u036f]/g, "")        // Xóa dấu
        .toLowerCase()                          // Viết thường
        .replace(/\s+/g, "_")                   // Thay khoảng trắng bằng "_"
        .replace(/[^\w_]/g, "") + ".wav";       // Xóa ký tự không hợp lệ và thêm đuôi
}

// Hàm xử lý phát nhạc tới ESP32
async function handleMusicCommand(songName, ws) {
    try {
        // Đường dẫn đến thư mục music
        const musicDir = path.join(__dirname, '../music');

        // Lọc tất cả file .wav trong thư mục nhạc
        const files = fs.readdirSync(musicDir).filter(file =>
            file.toLowerCase().endsWith('.wav')
        );

        if (files.length === 0) {   // Nếu không có file nhạc nào trong thư mục music
            playSoundFile(path.join(__dirname, '../sound/khong_tim_thay_file_nhac.wav'), ws); // Phát âm thanh thông báo không có nhạc
            return;
        }

        if (songName && songName !== 'ngẫu nhiên') {     // Nếu có tên bài hát cụ thể
            const fileName = normalizeFileName(songName);   // Chuyển đổi tên bài hát về không dấu và viết thường
            const filePath = path.join(musicDir, fileName); // Tạo đường dẫn file
            if (fs.existsSync(filePath)) {  // Kiểm tra file có tồn tại không
                console.log(`Playing specific song: ${fileName}`);
                await playSoundFile(filePath, ws);
                return;
            } else {
                console.log(`Song not found: ${fileName}`);
                playSoundFile(path.join(__dirname, '../sound/khong_tim_thay_file_nhac.wav'), ws); // Phát âm thanh thông báo không tìm thấy bài hát
                return;
            }
        }

        // Chọn ngẫu nhiên một file trong thư mục nhạc
        const randomIndex = Math.floor(Math.random() * files.length);
        const selectedFile = files[randomIndex];
        const musicFilePath = path.join(musicDir, selectedFile);

        await playSoundFile(musicFilePath, ws);

    } catch (error) {
        console.error("Error handling music command:", error);
        playSoundFile(path.join(__dirname, '../sound/loi_phat_nhac.wav'), ws); // Phát âm thanh lỗi
    }
}

// Hàm xứ lý gửi thông tin thời tiết tới ESP32
async function handleWeatherCommand(location, ws) {
    try {
        const weatherApiKey = WEATHER_API_KEY;
        const weatherResponse = await fetchWeatherData(location, weatherApiKey);  // Gọi hàm lấy thông tin thời tiết

        await sendTTSResponse(weatherResponse, ws);

    } catch (error) {
        console.error("Error handling weather command:", error);
        playSoundFile(path.join(__dirname, '../sound/khong_lay_duoc_thoi_tiet.wav'), ws); // Phát âm thanh lỗi
    }
}

// Hàm xử lý gửi thông tin thời gian tới ESP32
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
        playSoundFile(path.join(__dirname, '../sound/khong_xem_duoc_thoi_gian.wav'), ws); // Phát âm thanh lỗi
    }
}

// Hàm lấy thông tin thời tiết từ WeatherAPI.com
async function fetchWeatherData(location, apiKey) {
    const queryLocation = locationMap[location];
    try {
        // Using WeatherAPI.com instead of OpenWeatherMap
        const response = await axios.get(`https://api.weatherapi.com/v1/current.json`, {
            params: {
                q: queryLocation,
                key: apiKey,
                lang: 'vi'
            }
        });

        const data = response.data;

        const dataUV = data.current.uv < 3 ? "Thấp, An toàn" :
            data.current.uv < 6 ? "Trung bình, Cẩn thận khi ở ngoài trời lâu" :
                data.current.uv < 8 ? "Cao, Nên che chắn, dùng kem chống nắng" :
                    data.current.uv < 11 ? "Rất cao, Hạn chế ra ngoài vào giờ nắng gắt" :
                        "Nguy hiểm cực độ, Tránh tiếp xúc trực tiếp với ánh nắng";


        return `Thời tiết hiện tại ở ${location} là ${data.current.condition.text}. 
                Nhiệt độ ${Math.round(data.current.temp_c)} độ xê,
                Cảm giác như ${Math.round(data.current.feelslike_c)} độ xê, 
                Chỉ số u vê ${dataUV},
                Độ ẩm ${Math.round(data.current.humidity)} phần trăm,
                Tầm nhìn ${Math.round(data.current.vis_km)} ki lô mét,
                Áp suất ${Math.round(data.current.pressure_mb)} mi li ba, 
                Tốc độ gió ${Math.round(data.current.wind_kph * 0.277778)} mét trên giây.`;
    } catch (error) {
        console.error("Weather API error:", error);
        throw new Error("Không thể truy cập thông tin thời tiết");
    }
}

// Hàm gửi phản hồi TTS tới ESP32 sử dụng FPT TTS API
async function sendTTSResponse(text, ws) {
    if (ws.readyState !== WebSocket.OPEN) return;   // Kiểm tra trạng thái kết nối WebSocket

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

        if (response.data.error === 0) {    // Nếu không có lỗi từ API
            const audioUrl = response.data.async;   // Lấy URL của file âm thanh đã tạo

            // Chờ một chút để đảm bảo file đã được tạo xong
            await new Promise(resolve => setTimeout(resolve, 10000));

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
    playSoundFile(path.join(__dirname, '../sound/ket_noi_thanh_cong.wav'), ws); // Phát âm thanh chào mừng khi kết nối thành công

    let audioChunks = []; // Mảng lưu các chunk âm thanh nhận từ ESP32

    ws.on('message', async (message) => {
        // Kiểm tra xem message là binary (âm thanh) hay text (điều khiển)
        if (Buffer.isBuffer(message) && message.length == 2048) {   // Trường hợp âm thanh
            audioChunks.push(message);
        } else {    // Trường hợp TEXT điều khiển
            const textMessage = message.toString();
            console.log('Received control message:', textMessage);

            if (textMessage === 'END_OF_STREAM') {  // Đã gửi xong âm thanh
                // Ghép toàn bộ buffer lại
                const fullAudioBuffer = Buffer.concat(audioChunks);
                console.log(`Final audio size: ${fullAudioBuffer.length} bytes`);

                try {
                    // Gọi PhoWhisper service để chuyển đổi âm thanh thành văn bản
                    console.log("Calling PhoWhisper service...");
                    const transcriptionResult = await transcribeAudio(fullAudioBuffer);
                    console.log("PhoWhisper Result:", transcriptionResult);

                    if (transcriptionResult.success && transcriptionResult.text) {
                        const recognizedText = transcriptionResult.text;
                        console.log(`Recognized Text: "${recognizedText}"`);

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
                            playSoundFile(path.join(__dirname, '../sound/xin_loi_toi_khong_nghe_ro_vui_long_thu_lai.wav'), ws);
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
