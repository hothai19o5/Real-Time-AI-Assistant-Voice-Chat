from flask import Flask, request, jsonify
import torch
from transformers import AutoModelForSpeechSeq2Seq, AutoProcessor
import tempfile
import os
import numpy as np
import logging
import io

app = Flask(__name__)

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Đường dẫn đến model (cần tải trước hoặc chỉ định đường dẫn)
MODEL_PATH = "./models/phowhisper"

# Kiểm tra GPU
device = "cuda" if torch.cuda.is_available() else "cpu"
torch_dtype = torch.float16 if torch.cuda.is_available() else torch.float32

logger.info(f"Loading PhoWhisper model from {MODEL_PATH} on {device}")

# Load model và processor
model = AutoModelForSpeechSeq2Seq.from_pretrained(
    MODEL_PATH,
    torch_dtype=torch_dtype,
    low_cpu_mem_usage=False,
    use_safetensors=True
)
model.to(device)

processor = AutoProcessor.from_pretrained(MODEL_PATH)

logger.info("Model loaded successfully")

@app.route('/transcribe', methods=['POST'])
def transcribe():
    try:
        app.logger.info("Received transcription request")
        
        # Kiểm tra xem có file audio được gửi không
        if 'audio' not in request.files:
            app.logger.error("No audio file in request")
            return jsonify({"error": "No audio file provided", "success": False}), 400
        
        audio_file = request.files['audio']
        app.logger.info(f"Received audio file: {audio_file.filename}")
        
        # Tạo file tạm để lưu audio
        with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as tmp:
            audio_file.save(tmp.name)
            tmp_filename = tmp.name
            app.logger.info(f"Saved audio to temporary file: {tmp_filename}")
        
        try:
            # Đọc file âm thanh với librosa thay vì truyền đường dẫn trực tiếp
            import librosa
            import numpy as np
            
            # Đọc file âm thanh thành mảng số
            app.logger.info("Loading audio file with librosa...")
            audio_array, sr = librosa.load(tmp_filename, sr=16000, mono=True)
            app.logger.info(f"Audio loaded: {len(audio_array)} samples, sr={sr}")
            
            # Chuyển đổi âm thanh thành tính năng đầu vào
            app.logger.info("Extracting features...")
            input_features = processor.feature_extractor(
                audio_array,  # Truyền mảng âm thanh thay vì đường dẫn
                sampling_rate=16000,
                return_tensors="pt"
            ).input_features
            
            # Chạy inference
            app.logger.info("Running inference...")
            predicted_ids = model.generate(
                input_features.to(device),
                max_length=256
            )
            app.logger.info("Inference completed")
            
            # Chuyển đổi ID thành text
            transcription = processor.decode(predicted_ids[0], skip_special_tokens=True)
            app.logger.info(f"Transcription result: '{transcription}'")
            
            return jsonify({
                "text": transcription,
                "success": True
            })
            
        except Exception as e:
            app.logger.exception(f"Error during transcription: {str(e)}")
            return jsonify({"error": str(e), "success": False}), 500
            
        finally:
            # Xóa file tạm
            if os.path.exists(tmp_filename):
                os.unlink(tmp_filename)
                app.logger.info(f"Deleted temporary file: {tmp_filename}")
    
    except Exception as e:
        app.logger.exception(f"Unexpected error in transcribe endpoint: {str(e)}")
        return jsonify({"error": str(e), "success": False}), 500

if __name__ == "__main__":
    app.run(host='0.0.0.0', port=5000)