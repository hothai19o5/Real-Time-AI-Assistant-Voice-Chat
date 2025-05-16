from flask import Flask, request, jsonify, Response
import edge_tts
import asyncio
import logging
import io
import os
from pydub import AudioSegment
from dotenv import load_dotenv
from io import BytesIO
from flask import Flask, request, jsonify
from elevenlabs.client import ElevenLabs

# Tải biến môi trường từ file .env
load_dotenv()

app = Flask(__name__)
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

client = ElevenLabs(
    api_key=os.getenv("ELEVENLABS_API_KEY"),
)

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

@app.route('/stt', methods=['POST'])
def stt():
    # Kiểm tra xem có dữ liệu âm thanh không
    if 'audio' not in request.files:
        return jsonify({'success': False, 'error': 'Không có tệp âm thanh trong yêu cầu'}), 400

    audio_file = request.files['audio']

    try:
        # Đọc tệp âm thanh từ request
        audio_data = BytesIO(audio_file.read())

        logger.info(f"Processing audio file of size: {len(audio_data.getvalue())} bytes")

        # Gọi API ElevenLabs để nhận dạng âm thanh
        transcription = client.speech_to_text.convert(
            file=audio_data,
            model_id="scribe_v1",  # Sử dụng model scribe_v1
            tag_audio_events=False,  # Tag audio events như laughter, applause, etc.
            language_code="vi",  # Ngôn ngữ của âm thanh
            diarize=False,  # Chia tách người nói
        )

        # Log response type and content for debugging
        logger.info(f"Transcription response type: {type(transcription)}")
        logger.info(f"Transcription response: {transcription}")
        
        # Handle the response properly based on ElevenLabs SDK format
        # The response is likely an object with a text attribute
        if hasattr(transcription, 'text'):
            text = transcription.text
            return jsonify({'success': True, 'text': text})
        else:
            # If it's a dictionary (older SDK versions)
            if isinstance(transcription, dict) and 'text' in transcription:
                return jsonify({'success': True, 'text': transcription['text']})
            
            return jsonify({'success': False, 'error': 'Không tìm thấy dữ liệu text trong kết quả'}), 422

    except Exception as e:
        return jsonify({'success': False, 'error': str(e)}), 500


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5001)