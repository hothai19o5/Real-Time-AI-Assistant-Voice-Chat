import os
import logging
from dotenv import load_dotenv
from io import BytesIO
from flask import Flask, request, jsonify
from elevenlabs.client import ElevenLabs

load_dotenv()

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Khởi tạo Flask app và ElevenLabs client
app = Flask(__name__)
client = ElevenLabs(
    api_key=os.getenv("ELEVENLABS_API_KEY"),
)

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
            tag_audio_events=True,  # Tag audio events như laughter, applause, etc.
            language_code="vi",  # Ngôn ngữ của âm thanh
            diarize=True,  # Chia tách người nói
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
    app.run(debug=True, host='0.0.0.0', port=5002)  # Chạy Flask server ở cổng 5002
