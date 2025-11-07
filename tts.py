"""
Text-to-Speech Module with In-Memory Model Caching and CUDA Acceleration
Model: ZipVoice
"""
import sys
import json
from pathlib import Path
import torch
import soundfile as sf
from settings import tts_settings as cfg
import traceback

# === [GIẢI PHÁP] Thêm đường dẫn mã nguồn ZipVoice vào sys.path ===
# Điều này giải quyết triệt để lỗi "ModuleNotFoundError: No module named 'zipvoice'".
if str(cfg.ZIPVOICE_CODE_DIR) not in sys.path:
    sys.path.insert(0, str(cfg.ZIPVOICE_CODE_DIR))
# =================================================================

# === [BƯỚC 1] IMPORT TẤT CẢ CÁC THÀNH PHẦN CẦN THIẾT ===
try:
    # Model chính và Vocoder
    from zipvoice.models.zipvoice import ZipVoice
    from vocos import Vocos

    # Tokenizer
    from zipvoice.tokenizer.tokenizer import EspeakTokenizer

    # Các hàm tiện ích
    from zipvoice.utils.checkpoint import load_checkpoint
    from zipvoice.utils.feature import VocosFbank
    # [ĐÃ SỬA LỖI] Sửa tên file import từ 'infer_utils' thành 'infer'
    from zipvoice.utils.infer import (
        add_punctuation,
        chunk_tokens_punctuation,
        cross_fade_concat,
        load_prompt_wav,
        remove_silence,
        rms_norm,
    )
except ImportError as e:
    print(" LỖI IMPORT NGHIÊM TRỌNG ".center(80, "!"))
    print(f"Không thể import các thành phần cần thiết. Lỗi: {e}")
    print("Vui lòng kiểm tra các điều sau:")
    print(f"1. Thư mục '{cfg.ZIPVOICE_CODE_DIR}' có tồn tại.")
    print(f"2. Bạn đã cài đặt các thư viện chưa? Chạy lệnh: pip install vocos k2")
    sys.exit(1)


class TTSEngine:
    def __init__(self):
        self._validate_setup()
        self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        print(f"✅ TTS Engine sẽ chạy trên thiết bị: {self.device}.")

        print("🔧 Đang tải tất cả các model TTS vào bộ nhớ (chỉ một lần)...")
        
        try:
            # --- 1. Tải cấu hình từ model.json ---
            model_config_path = cfg.MODEL_DIR / "model.json"
            if not model_config_path.exists():
                raise FileNotFoundError(f"Không tìm thấy file cấu hình model tại: {model_config_path}")
            with open(model_config_path, "r") as f:
                self.model_config = json.load(f)
            self.sampling_rate = self.model_config["feature"]["sampling_rate"]
            print(f"  ✓ Đã tải cấu hình, sample rate là {self.sampling_rate} Hz.")

            # --- 2. Khởi tạo Tokenizer ---
            token_file = cfg.MODEL_DIR / "tokens.txt"
            if not token_file.exists():
                raise FileNotFoundError(f"Không tìm thấy file tokens tại: {token_file}")
            self.tokenizer = EspeakTokenizer(token_file=token_file, lang=cfg.LANG)
            tokenizer_config = {"vocab_size": self.tokenizer.vocab_size, "pad_id": self.tokenizer.pad_id}
            print("  ✓ Tokenizer đã sẵn sàng.")

            # --- 3. Khởi tạo Model chính (ZipVoice) ---
            self.model = ZipVoice(**self.model_config["model"], **tokenizer_config)
            checkpoint_path = self._find_checkpoint()
            load_checkpoint(filename=str(checkpoint_path), model=self.model, strict=True)
            self.model.to(self.device)
            self.model.eval()
            print("  ✓ Model ZipVoice chính đã được tải lên GPU.")

            # --- 4. Khởi tạo Vocoder ---
            self.vocoder = Vocos.from_pretrained("charactr/vocos-mel-24khz")
            self.vocoder.to(self.device)
            self.vocoder.eval()
            print("  ✓ Vocoder đã được tải lên GPU.")

            # --- 5. Khởi tạo Feature Extractor ---
            self.feature_extractor = VocosFbank()
            print("  ✓ Feature Extractor đã sẵn sàng.")

            print("✅ Tất cả các thành phần TTS đã sẵn sàng!")

        except Exception as e:
            print(" LỖI KHỞI TẠO MODEL TTS ".center(80, "❌"))
            traceback.print_exc()
            raise

        cfg.OUTPUT_AUDIO_DIR.mkdir(parents=True, exist_ok=True)

    def _validate_setup(self):
        if not cfg.ZIPVOICE_CODE_DIR.exists():
            raise FileNotFoundError(f"Thư mục mã nguồn ZipVoice không tìm thấy: {cfg.ZIPVOICE_CODE_DIR}")
        if not cfg.MODEL_DIR.exists():
            raise FileNotFoundError(f"Thư mục model ZipVoice không tìm thấy: {cfg.MODEL_DIR}")

    def _find_checkpoint(self) -> Path:
        for ext in cfg.CHECKPOINT_EXTENSIONS:
            files = list(cfg.MODEL_DIR.glob(f"*{ext}"))
            if files:
                return files[0]
        raise FileNotFoundError(f"Không tìm thấy file checkpoint trong '{cfg.MODEL_DIR}'")

    @torch.inference_mode()
    def synthesize(self, text: str, output_path: str = None, ref_audio: str = None, prompt_text: str = None) -> Path:
        output_wav_path = Path(output_path) if output_path else cfg.OUTPUT_AUDIO_DIR / "output.wav"
        ref_audio_path = ref_audio or cfg.DEFAULT_REF_AUDIO
        prompt_text = prompt_text or cfg.DEFAULT_PROMPT_TEXT
        
        print(f"🔊 Đang tổng hợp giọng nói trên '{self.device}': '{text[:40]}...'")

        # --- 1. Chuẩn bị Audio Prompt ---
        prompt_wav = load_prompt_wav(str(ref_audio_path), sampling_rate=self.sampling_rate)
        prompt_wav = remove_silence(prompt_wav, self.sampling_rate, only_edge=False, trail_sil=200)
        prompt_wav, prompt_rms = rms_norm(prompt_wav, target_rms=0.1)
        prompt_features = self.feature_extractor.extract(prompt_wav, sampling_rate=self.sampling_rate).to(self.device)
        prompt_features = prompt_features.unsqueeze(0) * 0.1 # feat_scale

        # --- 2. Chuẩn bị Text ---
        text = add_punctuation(text)
        prompt_text = add_punctuation(prompt_text)
        tokens_str = self.tokenizer.texts_to_tokens([text])[0]
        prompt_tokens_str = self.tokenizer.texts_to_tokens([prompt_text])[0]
        prompt_tokens = self.tokenizer.tokens_to_token_ids([prompt_tokens_str])
        
        # Chia text thành các đoạn nhỏ để tránh OOM và cải thiện chất lượng
        chunked_tokens_str = chunk_tokens_punctuation(tokens_str, max_tokens=100)
        chunked_tokens = self.tokenizer.tokens_to_token_ids(chunked_tokens_str)
        
        # --- 3. Tổng hợp đặc trưng âm thanh (acoustic features) ---
        wav_chunks = []
        for tokens_chunk in chunked_tokens:
            batch_tokens = [tokens_chunk]
            batch_prompt_tokens = prompt_tokens * len(batch_tokens)
            batch_prompt_features = prompt_features.repeat(len(batch_tokens), 1, 1)
            batch_prompt_features_lens = torch.full((len(batch_tokens),), prompt_features.size(1), device=self.device)
            
            pred_features, _, _, _ = self.model.sample(
                tokens=batch_tokens,
                prompt_tokens=batch_prompt_tokens,
                prompt_features=batch_prompt_features,
                prompt_features_lens=batch_prompt_features_lens,
                speed=cfg.SPEECH_SPEED,
                num_step=cfg.NUM_STEP,
                guidance_scale=3.0 # Giá trị mặc định tốt cho model này
            )
            pred_features = pred_features.permute(0, 2, 1) / 0.1 # feat_scale
            
            # --- 4. Dùng Vocoder để chuyển features thành audio ---
            for i in range(pred_features.size(0)):
                wav = self.vocoder.decode(pred_features[i].unsqueeze(0)).squeeze(1).clamp(-1, 1)
                if prompt_rms < 0.1:
                    wav = wav * prompt_rms / 0.1
                wav_chunks.append(wav)
        
        # --- 5. Nối các đoạn audio và lưu file ---
        final_wav = cross_fade_concat(wav_chunks, fade_duration=0.1, sample_rate=self.sampling_rate)
        final_wav = remove_silence(final_wav, self.sampling_rate, only_edge=(not cfg.REMOVE_LONG_SIL))
        
        sf.write(str(output_wav_path), final_wav.cpu().squeeze().numpy(), self.sampling_rate)
        
        print(f"✅ File âm thanh đã được tạo: {output_wav_path}")
        return output_wav_path


if __name__ == '__main__':
    print("\n" + "="*80)
    print(" CHẠY THỬ NGHIỆM MODULE TTS ".center(80))
    print("="*80 + "\n")
    try:
        engine = TTSEngine()
        print("\n--> KIỂM TRA `nvidia-smi` NGAY! Bạn sẽ thấy bộ nhớ VRAM được sử dụng. <--\n")
        
        path1 = engine.synthesize("Xin chào, đây là phiên bản đã sửa lỗi hoàn chỉnh.")
        print(f"--> Kết quả được lưu tại: {path1}")
        
        print("\n" + "✅ CHẠY THỬ NGHIỆM THÀNH CÔNG! ".center(80, "=") + "\n")

    except Exception as e:
        print(f"\nLỖI trong quá trình chạy thử nghiệm: {e}")
        traceback.print_exc()