from flask import Flask, request, jsonify, Response
import edge_tts
import asyncio
import logging
import io
from pydub import AudioSegment

app = Flask(__name__)
logging.basicConfig(level=logging.INFO)

@app.route('/tts', methods=['POST'])
def text_to_speech():
    try:
        # Lấy văn bản và thông số từ request
        data = request.json
        if not data or 'text' not in data:  # Kiểm tra xem văn bản có trong request hay không
            return jsonify({"error": "Thiếu văn bản đầu vào"}), 400
            
        text = data['text'] # Văn bản cần chuyển đổi
        voice = data.get('voice', 'vi-VN-HoaiMyNeural') # Giọng nói mặc định
        rate = data.get('rate', "+0%") # Tốc độ nói mặc định
        volume = data.get('volume', "+0%") # Âm lượng mặc định
        
        # Gọi Edge TTS API và lấy dữ liệu âm thanh MP3
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        audio_data = loop.run_until_complete(generate_audio_data(text, voice, rate, volume))

        #lưu file mp3 vào ./sound
        # with open('./sound_debug/sound_tts_received.mp3', 'wb') as f:
        #     f.write(audio_data)
        
        # Chuyển đổi MP3 sang WAV sử dụng pydub
        mp3_audio = AudioSegment.from_mp3(io.BytesIO(audio_data))
        
        # Chuyển đổi sang định dạng phù hợp cho ESP32
        # Mono (1 kênh), 16kHz sample rate, 16-bit
        wav_audio = mp3_audio.set_channels(1).set_frame_rate(16000).set_sample_width(2)
        
        # Xuất sang định dạng WAV
        wav_buffer = io.BytesIO()
        wav_audio.export(wav_buffer, format="wav")
        wav_buffer.seek(0)
        
        logging.info(f"Đã tạo xong audio, kích thước: {len(wav_buffer.getvalue())} bytes")
        
        # Trả về dữ liệu WAV
        return Response(
            wav_buffer.getvalue(),
            mimetype='audio/wav'
        )
        
    except Exception as e:
        logging.error(f"Lỗi: {str(e)}")
        return jsonify({"error": str(e)}), 500
    
@app.route('/voices', methods=['GET'])
def list_voices():
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    voices = loop.run_until_complete(edge_tts.list_voices())
    return jsonify(voices)

async def generate_audio_data(text, voice, rate, volume):
    """Tạo dữ liệu âm thanh từ text và trả về dưới dạng binary data"""
    communicate = edge_tts.Communicate(text, voice, rate=rate, volume=volume)
    
    # Thu thập dữ liệu âm thanh từ stream
    audio_data = bytes()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            audio_data += chunk["data"]
    
    return audio_data

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5001)