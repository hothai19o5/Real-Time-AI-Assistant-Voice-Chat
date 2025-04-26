import dotenv from 'dotenv';
dotenv.config();        // Load biến môi trường từ file .env
import { WebSocket, WebSocketServer } from 'ws';    // Import WebSocket từ thư viện ws
import fs from 'fs';            // Import fs để làm việc với file hệ thống
import path from 'path';        // Import path để xử lý đường dẫn file
import { GoogleGenerativeAI } from '@google/generative-ai';         // Import Google Generative AI SDK
import axios from 'axios';      // Import axios để thực hiện các yêu cầu HTTP
import FormData from 'form-data';       // Import FormData để gửi dữ liệu dạng form

// --- Cấu hình ---
const PORT = 8080;                                              // Port server lắng nghe
const GEMINI_API_KEY = process.env.GEMINI_API_KEY;              // Khóa API Gemini từ biến môi trường
const PHOWHISPER_SERVICE_URL = 'http://localhost:5000/transcribe'; // Định nghĩa URL cho PhoWhisper service
// Cấu hình FPT TTS API
const FPT_TTS_API_KEY = process.env.FPT_TTS_API_KEY;            // Khóa API TTS từ biến môi trường
const FPT_TTS_VOICE = process.env.FPT_TTS_VOICE;
const FPT_STT_API_KEY = process.env.FPT_STT_API_KEY;
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

// Khởi tạo Gemini
const genAI = new GoogleGenerativeAI(GEMINI_API_KEY);
const geminiModel = genAI.getGenerativeModel({ model: "gemini-2.0-flash" });
// Khởi tạo lịch sử trợ lý với một số câu hỏi mẫu
const initialAssistantHistory = [
    {
        role: "user",
        parts: [{ text: "Xin chào, từ bây giờ bạn sẽ là trợ lý ảo AI của tôi. Tôi sẽ gửi các câu hỏi cho bạn và bạn sẽ trả lời trong vòng tối đa khoảng 150-200 chữ. Trả lời tôi bằng tiếng việt, nếu có các từ bằng bắt buộc bằng tiếng anh thì hãy trả lời theo cách phát âm tiếng việt." }],
    },
    {
        role: "model",
        parts: [{ text: "Chào bạn! Tôi là một mô hình ngôn ngữ lớn, được đào tạo bởi Google. Tôi ở đây để giúp bạn với thông tin và các tác vụ dựa trên văn bản. Bạn có thể coi tôi là trợ lý AI ảo của bạn." }],
    },
    // {
    //   role: "user",
    //   parts: [{ text: "Nếu bạn thấy câu hỏi của tôi có ý nghĩa như `bật bài hát + tên_bài_hát` thì hãy đưa ra câu trả lời là `COMMAND bật bài hát tên_bài_hát` " }],
    // },
    // {
    //   role: "model",
    //   parts: [{ text: "Được thôi, ví dụ nếu câu hỏi của bạn là `bật anh là ai` thì tôi sẽ trả lời `COMMAND bật bài hát anh là ai`" }],
    // },
];

// --- Tạo WebSocket Server ---
const wss = new WebSocketServer({
    port: PORT,
    perMessageDeflate: false, // Tắt nén dữ liệu, giảm độ trễ, giảm tải CPU 
    keepalive: true, // Bật tính năng giữ kết nối sống
    clientTracking: true, // Theo dõi trạng thái kết nối của client
    keepaliveInterval: 30000 // Thời gian giữ kết nối sống (30 giây)
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

    // Lọc lệnh giới thiệu bản thân
    if (normalizedText.includes('giới thiệu bản thân') || normalizedText.includes('bạn là ai') ||
        normalizedText.includes('bạn tên gì') || normalizedText.includes('tên bạn là gì')
        || normalizedText.includes('tên mày là gì') || normalizedText.includes('mày là ai') || normalizedText.includes('mày tên gì')) {
        return {
            type: 'INTRODUCE'
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
        const musicDir = './music';

        // Lọc tất cả file .wav trong thư mục nhạc
        const files = fs.readdirSync(musicDir).filter(file =>
            file.toLowerCase().endsWith('.wav')
        );

        if (files.length === 0) {   // Nếu không có file nhạc nào trong thư mục music
            playSoundFile('./sound/khong_tim_thay_file_nhac.wav', ws); // Phát âm thanh thông báo không có nhạc
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
                playSoundFile('./sound/khong_tim_thay_file_nhac.wav', ws); // Phát âm thanh thông báo không tìm thấy bài hát
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
        playSoundFile('./sound/loi_phat_nhac.wav', ws); // Phát âm thanh lỗi
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
        playSoundFile('./sound/khong_lay_duoc_thoi_tiet.wav', ws); // Phát âm thanh lỗi
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
        playSoundFile('./sound/khong_xem_duoc_thoi_gian.wav', ws); // Phát âm thanh lỗi
    }
}

// Hàm lấy thông tin thời tiết từ WeatherAPI.com
async function fetchWeatherData(location, apiKey) {
    // Lấy địa điểm từ trong map, nếu không tìm thấy thì auto trả về Hà Nội
    const queryLocation = locationMap[location] || 'Hanoi';
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


        return `Thời tiết hiện tại ở ${queryLocation} là ${data.current.condition.text}. 
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

// /**
//  * Gửi phản hồi TTS tới ESP32 sử dụng FPT TTS API với xử lý song song
//  * @param {string} text - Văn bản cần chuyển thành giọng nói
//  * @param {WebSocket} ws - WebSocket connection
//  */
// async function sendTTSResponse(text, ws) {
//     if (ws.readyState !== WebSocket.OPEN) return;   // Kiểm tra trạng thái kết nối WebSocket

//     try {
//         ws.send("AUDIO_STREAM_START");

//         // Chuẩn bị header với format=wav để nhận WAV thay vì MP3
//         const headers = {
//             'api_key': FPT_TTS_API_KEY,
//             'voice': FPT_TTS_VOICE,
//             'format': 'wav',
//             'Cache-Control': 'no-cache'
//         };

//         // Gọi API FPT để chuyển text thành speech
//         const response = await axios.post(
//             'https://api.fpt.ai/hmi/tts/v5',
//             text,
//             { headers }
//         );

//         if (response.data.error === 0) {    // Nếu không có lỗi từ API
//             const audioUrl = response.data.async;   // Lấy URL của file âm thanh đã tạo

//             // Chờ một chút để đảm bảo file đã được tạo xong
//             await new Promise(resolve => setTimeout(resolve, 10000));

//             // Tải file WAV từ URL
//             const audioResponse = await axios.get(audioUrl, { responseType: 'arraybuffer' });
//             const audioBuffer = Buffer.from(audioResponse.data);

//             // Chia thành các chunk và gửi qua WebSocket
//             const audioChunks = chunkAudioData(audioBuffer, 2048);

//             for (let i = 0; i < audioChunks.length; i++) {
//                 if (ws.readyState !== WebSocket.OPEN) break;
//                 ws.send(audioChunks[i]);
//                 await new Promise(resolve => setTimeout(resolve, 50));
//             }

//             // Kết thúc stream
//             if (ws.readyState === WebSocket.OPEN) {
//                 const silenceBuffer = Buffer.alloc(1600, 0); // 50ms of silence
//                 ws.send(silenceBuffer);
//                 await new Promise(resolve => setTimeout(resolve, 100));
//                 ws.send("AUDIO_STREAM_END");
//             }
//         } else {
//             console.error("FPT TTS API Error:", response.data);
//             throw new Error(`FPT API error: ${response.data.message || 'Unknown error'}`);
//         }
//     } catch (error) {
//         console.error("Error sending TTS response:", error);

//         // Gửi thông báo lỗi nếu ws vẫn mở
//         if (ws.readyState === WebSocket.OPEN) {
//             ws.send("AUDIO_STREAM_END");
//             ws.send(`Error generating speech: ${error.message}`);
//         }
//     }
// }

/**
 * Gửi phản hồi TTS tới ESP32 sử dụng Edge TTS thông qua service Python
 * @param {string} text - Văn bản cần chuyển thành giọng nói
 * @param {WebSocket} ws - WebSocket connection
 */
async function sendTTSResponse(text, ws) {
    if (ws.readyState !== WebSocket.OPEN) return;

    try {
        ws.send("AUDIO_STREAM_START");

        const response = await axios.post('http://localhost:5001/tts', {
            text: text,
            voice: 'vi-VN-HoaiMyNeural',
            rate: "+0%",
            volume: "-50%"
        }, {
            responseType: 'arraybuffer',
            timeout: 30000 // 30 giây timeout
        });

        // Chuyển đổi phản hồi thành buffer
        const audioBuffer = Buffer.from(response.data);
        console.log(`Nhận được dữ liệu âm thanh: ${audioBuffer.length} bytes`);

        // Chia nhỏ thành các chunk để gửi qua WebSocket
        const audioChunks = chunkAudioData(audioBuffer, 2048);
        console.log(`Chia thành ${audioChunks.length} đoạn âm thanh để gửi`);

        // Gửi từng chunk âm thanh qua WebSocket
        for (let i = 0; i < audioChunks.length; i++) {
            if (ws.readyState !== WebSocket.OPEN) {
                console.log("WebSocket đã đóng, dừng phát âm thanh");
                break;
            }

            ws.send(audioChunks[i]);

            // Đợi một chút giữa các chunk để tránh buffer overflow
            await new Promise(resolve => setTimeout(resolve, 30));
        }

        // Kết thúc stream
        if (ws.readyState === WebSocket.OPEN) {
            const silenceBuffer = Buffer.alloc(1600, 0); // 50ms silence
            ws.send(silenceBuffer);
            await new Promise(resolve => setTimeout(resolve, 100));
            ws.send("AUDIO_STREAM_END");
        }

    } catch (error) {
        console.error("Lỗi gửi phản hồi TTS:", error);

        // Phát âm thanh lỗi và gửi thông báo nếu ws vẫn mở
        if (ws.readyState === WebSocket.OPEN) {
            playSoundFile('./sound/tts_timeout.wav', ws);
            ws.send("AUDIO_STREAM_END");
        }
    }
}

/**
 * Gửi phản hồi TTS tới ESP32 sử dụng Edge TTS thông qua service Python, hàm này sẽ tách văn bản thành các đoạn nhỏ rồi gửi TTS để tốc độ phản hồi cao hơn.
 * @param {string} text - Văn bản cần chuyển thành giọng nói
 * @param {WebSocket} ws - WebSocket connection
 */
// async function sendTTSResponse(text, ws) {
//     if (ws.readyState !== WebSocket.OPEN) return;

//     try {
//         console.log("Bắt đầu xử lý Edge TTS...");
//         ws.send("AUDIO_STREAM_START");

//         // Tách văn bản thành các đoạn nhỏ để xử lý nhanh hơn
//         const textChunks = tachVanBanThanhDoan(text, 150);
//         console.log(`Đã tách văn bản thành ${textChunks.length} đoạn`);

//         // Theo dõi thứ tự phát và trạng thái
//         let currentPlayingIndex = 0;
//         let isPlayingChunk = false;
//         const audioCache = new Map();

//         // Hàm phát đoạn tiếp theo nếu có
//         const playNextChunkIfAvailable = async () => {
//             if (isPlayingChunk) return; // Đang phát một đoạn, đợi

//             if (audioCache.has(currentPlayingIndex)) {
//                 isPlayingChunk = true;
//                 const audioData = audioCache.get(currentPlayingIndex);
//                 audioCache.delete(currentPlayingIndex); // Xóa khỏi cache để giải phóng bộ nhớ

//                 console.log(`Đang phát đoạn ${currentPlayingIndex + 1}/${textChunks.length}`);

//                 // Chia nhỏ và gửi qua WebSocket
//                 const audioChunks = chunkAudioData(audioData, 2048);
//                 for (const chunk of audioChunks) {
//                     if (ws.readyState !== WebSocket.OPEN) {
//                         isPlayingChunk = false;
//                         return;
//                     }
//                     ws.send(chunk);
//                     await new Promise(resolve => setTimeout(resolve, 30));
//                 }

//                 // Thêm pause nhỏ giữa các đoạn
//                 await new Promise(resolve => setTimeout(resolve, 50));

//                 // Cập nhật index và tiếp tục kiểm tra đoạn tiếp theo
//                 currentPlayingIndex++;
//                 isPlayingChunk = false;
//                 playNextChunkIfAvailable();
//             }
//         };

//         // Xử lý song song các đoạn văn bản
//         const processingPromises = textChunks.map((chunk, index) => {
//             return (async () => {
//                 try {
//                     console.log(`Đang gọi Edge TTS cho đoạn ${index + 1}/${textChunks.length}`);

//                     // Gọi API Python Edge TTS
//                     const response = await axios.post('http://localhost:5001/tts', {
//                         text: chunk,
//                         voice: 'vi-VN-HoaiMyNeural',
//                         rate: "+0%",
//                         volume: "-50%"
//                     }, {
//                         responseType: 'arraybuffer'
//                     });

//                     // Chuyển đổi phản hồi thành buffer
//                     const audioBuffer = Buffer.from(response.data);

//                     // Lưu vào cache theo đúng thứ tự
//                     audioCache.set(index, audioBuffer);

//                     // Kiểm tra xem có thể phát ngay không
//                     if (index === currentPlayingIndex) {
//                         playNextChunkIfAvailable();
//                     }
//                 } catch (error) {
//                     console.error(`Lỗi xử lý đoạn ${index + 1}:`, error.message);
//                 }
//             })();
//         });

//         // Đợi tất cả các đoạn xử lý xong hoặc thất bại
//         await Promise.allSettled(processingPromises);

//         // Đợi đến khi phát xong tất cả các đoạn
//         const waitForAllChunksToPlay = async () => {
//             while (currentPlayingIndex < textChunks.length) {
//                 if (ws.readyState !== WebSocket.OPEN) break;
//                 await new Promise(resolve => setTimeout(resolve, 100));
//                 playNextChunkIfAvailable();
//             }
//         };

//         // Đợi phát hết các đoạn
//         await waitForAllChunksToPlay();

//         // Kết thúc stream
//         if (ws.readyState === WebSocket.OPEN) {
//             const silenceBuffer = Buffer.alloc(1600, 0); // 50ms silence
//             ws.send(silenceBuffer);
//             await new Promise(resolve => setTimeout(resolve, 100));
//             ws.send("AUDIO_STREAM_END");
//         }
//     } catch (error) {
//         console.error("Lỗi gửi phản hồi TTS:", error);

//         // Phát âm thanh lỗi và gửi thông báo nếu ws vẫn mở
//         if (ws.readyState === WebSocket.OPEN) {
//             playSoundFile('./sound/tts_timeout.wav', ws);
//             ws.send("AUDIO_STREAM_END");
//         }
//     }
// }

/**
 * Tách văn bản thành các đoạn nhỏ dựa trên dấu câu
 * @param {string} text - Văn bản cần tách
 * @param {number} maxLength - Độ dài tối đa mỗi đoạn
 * @returns {string[]} - Mảng các đoạn văn bản
 */
// function tachVanBanThanhDoan(text, maxLength = 150) {
//     // Xóa khoảng trắng thừa
//     text = text.trim();

//     // Nếu văn bản đủ ngắn, trả về nguyên văn
//     if (text.length <= maxLength) {
//         return [text];
//     }

//     // Tách theo dấu chấm trước
//     const dauChamSplits = text.split(/(?<=\.)\s+/);
//     const ketQua = [];
//     let doanHienTai = '';

//     for (const cau of dauChamSplits) {
//         // Nếu câu ngắn, thêm vào đoạn hiện tại
//         if (cau.length <= maxLength) {
//             if (doanHienTai.length + cau.length + 1 <= maxLength) {
//                 doanHienTai += (doanHienTai ? ' ' : '') + cau;
//             } else {
//                 // Nếu thêm vào sẽ quá dài, lưu đoạn hiện tại và bắt đầu đoạn mới
//                 ketQua.push(doanHienTai);
//                 doanHienTai = cau;
//             }
//         } else {
//             // Nếu câu dài, tách tiếp theo dấu phẩy
//             const dauPhaySplits = cau.split(/(?<=,)\s+/);

//             for (const ve of dauPhaySplits) {
//                 if (doanHienTai.length + ve.length + 1 <= maxLength) {
//                     doanHienTai += (doanHienTai ? ' ' : '') + ve;
//                 } else {
//                     // Nếu vế quá dài, lưu đoạn hiện tại và bắt đầu đoạn mới
//                     if (doanHienTai) ketQua.push(doanHienTai);

//                     // Nếu vế vẫn dài hơn maxLength, chia nhỏ hơn nữa theo từng từ
//                     if (ve.length > maxLength) {
//                         let phanDu = ve;
//                         while (phanDu.length > maxLength) {
//                             // Tìm vị trí khoảng trắng gần nhất
//                             let viTriCat = phanDu.lastIndexOf(' ', maxLength);
//                             if (viTriCat <= 0) viTriCat = maxLength;

//                             // Thêm phần đã cắt vào kết quả
//                             ketQua.push(phanDu.substring(0, viTriCat).trim());
//                             phanDu = phanDu.substring(viTriCat).trim();
//                         }
//                         doanHienTai = phanDu;
//                     } else {
//                         doanHienTai = ve;
//                     }
//                 }
//             }
//         }
//     }

//     // Thêm đoạn cuối cùng nếu còn
//     if (doanHienTai) {
//         ketQua.push(doanHienTai);
//     }

//     return ketQua;
// }

/**
 * Bỏ header WAV (44 bytes đầu) nếu có
 * @param {Buffer} audioBuffer - Buffer âm thanh
 * @returns {Buffer} - Buffer PCM thuần túy
 */
// function stripWavHeader(audioBuffer) {
//     // Kiểm tra xem có phải file WAV không
//     const isWav = audioBuffer.length > 44 &&
//         audioBuffer.toString('ascii', 0, 4) === 'RIFF' &&
//         audioBuffer.toString('ascii', 8, 12) === 'WAVE';

//     // Nếu là WAV, bỏ 44 byte header
//     if (isWav) {
//         console.log("Đã phát hiện header WAV, bỏ 44 byte đầu");
//         return audioBuffer.slice(44);
//     } else {
//         return audioBuffer;
//     }
// }

/**
 * Hàm gọi API Speech-to-Text của FPT AI sử dụng axios
 * @param {Buffer} audioBuffer - Buffer chứa dữ liệu âm thanh PCM
 * @param {string} apiKey - API key của FPT AI
 * @param {WebSocket} ws - WebSocket để phát âm thanh lỗi (optional)
 * @returns {Promise<Object>} Kết quả nhận dạng
 */
async function callApiSpeechToText(audioBuffer, apiKey, ws = null, timeoutMs = 30000) {

    // Kiểm tra đầu vào
    if (!audioBuffer || audioBuffer.length === 0) {
        const error = new Error('Không có dữ liệu âm thanh');
        console.error(error.message);
        return { success: false, error: error.message };
    }

    if (!apiKey) {
        const error = new Error('Thiếu API key');
        console.error(error.message);
        return { success: false, error: error.message };
    }

    try {
        // Tạo WAV header
        const wavHeaderBuffer = createWavHeader(audioBuffer.length, {
            numChannels: 1,
            sampleRate: 16000,
            bitsPerSample: 16
        });

        // Tạo file WAV đầy đủ
        const wavData = Buffer.concat([wavHeaderBuffer, audioBuffer]);

        console.log(`Đang gửi âm thanh đến FPT STT API: ${wavData.length} bytes`);

        const response = await axios.post(
            'https://api.fpt.ai/hmi/asr/general',
            wavData,
            {
                headers: {
                    'api_key': apiKey
                }
            }
        );

        // Xử lý các trạng thái khác nhau
        switch (response.data.status) {
            case 0: // Thành công
                console.log(`STT thành công: "${response.data.hypotheses[0].utterance}"`);
                return {
                    success: true,
                    text: response.data.hypotheses[0].utterance,
                    id: response.data.id,
                    status: response.data.status
                };

            case 1: // Không có giọng nói
                console.log('STT: Không phát hiện giọng nói');
                if (ws && ws.readyState === WebSocket.OPEN) {
                    playSoundFile('./sound/khong_nghe_thay_giong_noi.wav', ws);
                }
                return {
                    success: false,
                    error: 'Không phát hiện giọng nói',
                    id: response.data.id,
                    status: response.data.status
                };

            case 2: // Đã hủy
                console.log('STT: Yêu cầu bị hủy');
                return {
                    success: false,
                    error: 'Yêu cầu bị hủy',
                    id: response.data.id,
                    status: response.data.status
                };

            case 9: // Hệ thống bận
                console.log('STT: Hệ thống bận');
                if (ws && ws.readyState === WebSocket.OPEN) {
                    playSoundFile('./sound/he_thong_dang_ban.wav', ws);
                }
                return {
                    success: false,
                    error: response.data.message || 'Hệ thống đang bận',
                    id: response.data.id,
                    status: response.data.status
                };

            default:
                console.log(`STT: Lỗi không xác định (status: ${response.data.status})`);
                return {
                    success: false,
                    error: 'Lỗi không xác định',
                    id: response.data.id,
                    status: response.data.status
                };
        }
    } catch (error) {
        // Xử lý lỗi từ axios
        console.error('Lỗi khi gọi API Speech-to-Text:', error);

        // Phát âm thanh lỗi nếu có WebSocket
        if (ws && ws.readyState === WebSocket.OPEN) {
            playSoundFile('./sound/stt_timeout.wav', ws);
        }

        // Kiểm tra lỗi timeout
        if (error.code === 'ECONNABORTED') {
            return {
                success: false,
                error: `Timeout (${timeoutMs / 1000}s)`,
                timedOut: true
            };
        }

        // Trả về thông tin lỗi chi tiết
        return {
            success: false,
            error: error.message || 'Lỗi không xác định',
            status: error.response?.status
        };
    }
}

// Hàm gọi PhoWhisper để dùng STT, nhưng hiện tại đang dùng API STT của FPT.
// async function transcribeAudio(audioBuffer) {
//     try {
//         // Tạo WAV header
//         const wavHeaderBuffer = createWavHeader(audioBuffer.length, {
//             numChannels: 1,
//             sampleRate: 16000,
//             bitsPerSample: 16
//         });

//         // Tạo file WAV đầy đủ
//         const wavData = Buffer.concat([wavHeaderBuffer, audioBuffer]);

//         console.log(`Sending audio to PhoWhisper: ${wavData.length} bytes`);

//         // Tạo form data để gửi file
//         const formData = new FormData();
//         formData.append('audio', Buffer.from(wavData), {
//             filename: 'audio.wav',
//             contentType: 'audio/wav'
//         });

//         try {
//             // Gọi API Python với timeout dài hơn
//             const response = await axios.post(PHOWHISPER_SERVICE_URL, formData, {
//                 headers: {
//                     ...formData.getHeaders()
//                 },
//                 maxContentLength: Infinity,
//                 maxBodyLength: Infinity,
//                 timeout: 30000  // 30 giây timeout
//             });

//             return response.data;
//         } catch (axiosError) {
//             if (axiosError.response) {
//                 // Máy chủ đã phản hồi với mã lỗi
//                 console.error("PhoWhisper service error details:",
//                     axiosError.response.status,
//                     axiosError.response.data);
//             }
//             throw axiosError;
//         }
//     } catch (error) {
//         playSoundFile('./sound/pho_whisper_timeout.wav', ws); // Phát âm thanh lỗi
//         console.error("Error calling PhoWhisper service:", error.message);
//         throw error;
//     }
// }

// --- Xử lý kết nối Client ---
wss.on('connection', (ws) => {
    ws.isAlive = true; // Đánh dấu kết nối là sống

    // Xử lý ping/pong
    ws.on('pong', () => { ws.isAlive = true; });

    console.log('Client connected');
    playSoundFile("./sound/ket_noi_thanh_cong.wav", ws); // Phát âm thanh chào mừng khi kết nối thành công

    let audioChunks = []; // Mảng lưu các chunk âm thanh nhận từ ESP32

    // Bắt đầu phiên chat với lịch sử mẫu
    // Gọi từ ngoài để không phải khởi tạo liên tục
    const chat = geminiModel.startChat({
        history: initialAssistantHistory, // Truyền mảng lịch sử mẫu vào đây
    });


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
                    // console.log("Calling PhoWhisper service...");
                    // const transcriptionResult = await transcribeAudio(fullAudioBuffer);
                    // console.log("PhoWhisper Result:", transcriptionResult);

                    // Gọi FPT Speech-to-Text API để chuyển đổi âm thanh thành văn bản
                    console.log("Calling FPT Speech-to-Text API...");
                    const transcriptionResult = await callApiSpeechToText(fullAudioBuffer, FPT_STT_API_KEY, ws, 30000);

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
                        else if (commandResult.type === 'INTRODUCE') {
                            console.log(`Introduce command detected`);
                            playSoundFile('./sound/gioi_thieu_ban_than_' + Math.floor(Math.random() * 4 + 1) + '.wav', ws); // Phát âm thanh giới thiệu bản thân
                        }
                        else {
                            // No specific command detected, process with Gemini as before
                            try {
                                console.log("Sending text to Gemini...");
                                // Đã khởi tạo chat với lịch sử mẫu ở trên
                                // Sau đó, khi muốn gửi câu hỏi thực tế từ người dùng (recognizedText), sẽ dùng chat.sendMessage()
                                // Điều này sẽ tự động bao gồm lịch sử mẫu và các lượt sau đó làm ngữ cảnh
                                const result = await chat.sendMessage(recognizedText);
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
                            playSoundFile('./sound/xin_loi_toi_khong_nghe_ro_vui_long_thu_lai.wav', ws);
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
