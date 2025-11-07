import time
import sys
import json
from pathlib import Path
from typing import Optional

# Thêm đường dẫn gốc để Python tìm thấy các module settings
ROOT_DIR = Path(__file__).resolve().parent.parent
sys.path.append(str(ROOT_DIR))

from modules.stt import STTEngine
from modules.tts import TTSEngine
from modules.llm import LLMEngine

class VoiceAssistantPipeline:
    def __init__(self):
        print("\n" + "="*60)
        print("🚀 Khởi tạo Voice Assistant Pipeline")
        print("="*60 + "\n")
        
        self.stt_engine = STTEngine()
        self.llm_engine = LLMEngine()
        self.tts_engine = TTSEngine()
        
        print("\n" + "="*60)
        print("✅ Pipeline đã sẵn sàng!")
        print("="*60 + "\n")
    
    def process(self, audio_input_path: str, session_id: str = "default") -> dict:
        start_time = time.time()
        
        print("\n" + "🔄 " + "="*58)
        print(f"BẮT ĐẦU PIPELINE VỚI FILE AUDIO: {audio_input_path}")
        print("="*60 + "\n")
        
        # Step 1: STT
        print("📍 BƯỚC 1: Speech to Text (dùng GPU)")
        print("-" * 60)
        input_text = self.stt_engine.transcribe(audio_input_path)
        print(f"✓ Chuyển đổi thành văn bản: {input_text}\n")
        
        # Step 2: LLM
        print("📍 BƯỚC 2: Xử lý ngôn ngữ (API) & Phân tích cảm xúc")
        print("-" * 60)
        response_text, emotion_details = self.llm_engine.chat(input_text, session_id=session_id)
        print(f"✓ Phản hồi từ LLM: {response_text}")
        print(f"✓ Kết quả phân tích cảm xúc: {json.dumps(emotion_details, ensure_ascii=False)}\n")
        
        # Step 3: TTS
        print("📍 BƯỚC 3: Text to Speech (dùng GPU)")
        print("-" * 60)
        output_audio_path = self.tts_engine.synthesize(response_text)
        print(f"✓ Tạo file âm thanh tại: {output_audio_path}\n")
        
        processing_time = time.time() - start_time
        
        print("="*60)
        print(f"✅ PIPELINE HOÀN TẤT trong {processing_time:.2f} giây")
        print("="*60 + "\n")
        
        return {
            "input_text": input_text,
            "response_text": response_text,
            "output_audio": output_audio_path,
            "processing_time": processing_time,
            "emotion_details": emotion_details
        }

# === [PHẦN ĐÃ SỬA] Thêm điểm khởi đầu để chạy file độc lập ===
if __name__ == '__main__':
    # Thư viện để đọc tham số từ dòng lệnh
    import argparse
    
    # 1. Tạo một trình phân tích cú pháp
    parser = argparse.ArgumentParser(description="Run the Voice Assistant Pipeline for a single audio file.")
    
    # 2. Định nghĩa tham số --input mà chúng ta muốn nhận
    parser.add_argument("--input", type=str, required=True, help="Path to the input audio file (.wav)")
    
    # 3. Đọc các tham số đã được truyền vào
    args = parser.parse_args()

    # 4. Bây giờ, chúng ta thực sự khởi tạo và chạy pipeline
    print(">>> Chạy pipeline ở chế độ thử nghiệm độc lập <<<")
    
    try:
        # Tạo một đối tượng từ class VoiceAssistantPipeline
        assistant_pipeline = VoiceAssistantPipeline()
        
        # Gọi phương thức process với file input từ dòng lệnh
        result = assistant_pipeline.process(audio_input_path=args.input)
        
        # In kết quả cuối cùng ra
        print("\n" + "="*60)
        print(" KẾT QUẢ CUỐI CÙNG ".center(60, "="))
        print(f"  - Input Text: {result['input_text']}")
        print(f"  - Response Text: {result['response_text']}")
        print(f"  - Output Audio: {result['output_audio']}")
        print(f"  - Processing Time: {result['processing_time']:.2f}s")
        print("="*60)

    except Exception as e:
        import traceback
        print(f"\nLỖI: Một lỗi đã xảy ra trong quá trình chạy pipeline:")
        traceback.print_exc()