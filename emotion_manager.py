# === FILE: modules/emotion_manager.py (PHIÊN BẢN HOÀN THIỆN VỚI PROMPT TỐT HƠN) ===
import google.generativeai as genai
from google.generativeai import types
from google.generativeai.types import HarmCategory, HarmBlockThreshold
import asyncio

# Định nghĩa các hằng số cho mã cảm xúc để dễ quản lý
EMOTION_NEUTRAL = "00"
EMOTION_HAPPY = "01"
EMOTION_SAD = "10"

class EmotionManager:
    """
    Quản lý việc phát hiện cảm xúc và tối ưu hóa prompt.
    """
    def __init__(self, llm_client: genai.GenerativeModel):
        self.llm_client = llm_client
        self.safety_settings = {
            HarmCategory.HARM_CATEGORY_HARASSMENT: HarmBlockThreshold.BLOCK_NONE,
            HarmCategory.HARM_CATEGORY_HATE_SPEECH: HarmBlockThreshold.BLOCK_NONE,
            HarmCategory.HARM_CATEGORY_SEXUALLY_EXPLICIT: HarmBlockThreshold.BLOCK_NONE,
            HarmCategory.HARM_CATEGORY_DANGEROUS_CONTENT: HarmBlockThreshold.BLOCK_NONE,
        }
        self.emotion_keywords = {
            "SAD": ['buồn', 'khóc', 'mếu', 'chán', 'tủi thân', 'cô đơn', 'một mình', 'thất vọng',
                'buồn quá', 'chán quá', 'buồn ghê', 'huhu', 'oa oa', 'rầu rĩ', 'ủ rũ',
                # Bị la mắng, trừng phạt
                'bị mắng', 'bị phạt', 'bị la', 'bị trách',
                # Kết quả học tập xấu
                'điểm kém', 'điểm thấp', 'bị điểm 0', 'không làm được bài',
                # Vấn đề xã hội
                'bạn trêu', 'bạn bắt nạt', 'bạn đánh', 'không ai chơi', 'cãi nhau', 'giận', 'ghét', 'bị tẩy chay',
                # Nhớ nhung
                'nhớ mẹ', 'nhớ bố', 'nhớ nhà', 'nhớ ông bà', 'nhớ bạn',
                # Sức khỏe & Tai nạn
                'mệt', 'đau', 'ốm', 'sốt', 'ho', 'khó chịu', 'mệt mỏi', 'bị ngã', 'chảy máu', 'xây xước', 'bị thương',
                # Mất mát, thất bại
                'thua rồi', 'mất rồi', 'bị hỏng', 'làm rơi', 'bị vỡ', 'tìm không thấy', 'làm sai', 'thất bại',
                # Sợ hãi
                'sợ', 'sợ quá', 'ma', 'bóng tối', 'ác mộng', 'kinh dị', 'hãi'],
            "HAPPY": [   # Cảm xúc vui vẻ
                'vui', 'thích', 'cười', 'hạnh phúc', 'tuyệt vời', 'hay quá', 'thích mê', 'sướng quá', 'khoái',
                'tuyệt cú mèo', 'haha', 'hihi', 'hì hì', 'vui quá', 'thích quá', 'hay ghê', 'thích ghê', 'đã quá',
                # Được khen thưởng, quà tặng
                'được quà', 'được khen', 'được thưởng', 'được tặng', 'có đồ chơi mới', 'quà',
                # Kết quả học tập tốt
                'điểm 10', 'điểm cao', 'được điểm tốt', 'làm đúng rồi', 'bài dễ',
                # Hoạt động giải trí
                'đi chơi', 'gặp bạn', 'công viên', 'xem phim', 'đi sở thú', 'đi du lịch', 'xem xiếc', 'cắm trại',
                # Cảm xúc phấn khích
                'hào hứng', 'phấn khích', 'wow', 'ồ', 'á', 'thắng rồi', 'làm được rồi', 'thành công',
                # Tình cảm gia đình, bạn bè
                'được yêu', 'yêu mẹ', 'yêu bố', 'thương', 'quý', 'thân',
                # Sự kiện đặc biệt
                'sinh nhật', 'tết', 'noel', 'trung thu', 'lễ hội',
                # Thức ăn
                'ngon', 'ngon quá', 'được ăn kẹo', 'ăn bánh', 'uống trà sữa', 'kem',
                # Khác
                'đồng ý', 'cầu hôn', 'tuyệt', 'xuất sắc', 'giỏi'],
            "NEUTRAL": [  # Chào hỏi & Tạm biệt
                'chào', 'hello', 'hi', 'tạm biệt', 'bye', 'gặp lại sau', 'chào bạn', 'chào cậu',
                # Cảm ơn & Xin lỗi
                'cảm ơn', 'cám ơn', 'thank you', 'thanks', 'xin lỗi', 'sorry', 'thứ lỗi',
                # Từ ngữ chung chung & Xác nhận
                'bình thường', 'cũng được', 'dạ', 'vâng', 'ạ', 'ok', 'okay', 'được', 'không',
                'à', 'ừ', 'đúng rồi', 'chắc chắn', 'tất nhiên', 'có', 'không có', 'à ừm', 'ờm',
                # Các câu hỏi cơ bản (5W1H và hơn thế nữa)
                'gì', 'đâu', 'nào', 'sao', 'thế nào', 'tại sao', 'khi nào', 'ai', 'ai đấy',
                'mấy giờ', 'bao nhiêu', 'cái gì', 'con gì', 'ở đâu', 'chỗ nào', 'làm sao', 'bằng cách nào',
                'phải không', 'đúng không', 'hả', 'nhỉ',
                # Các hoạt động hàng ngày & Học tập
                'học bài', 'đọc truyện', 'xem tivi', 'ăn cơm', 'đi ngủ', 'đi học', 'vẽ', 'vẽ tranh',
                'nghe nhạc', 'làm toán', 'viết chữ', 'đánh vần', 'tập đọc', 'tập viết', 'tiếng việt', 'tiếng anh',
                # Các yêu cầu & Mệnh đề
                'kể cho', 'nói cho', 'đọc cho', 'viết cho', 'chỉ cho', 'giúp tớ',
                'chuyện gì', 'cái này', 'cái kia', 'thử xem', 'tiếp đi', 'là sao',
                # Các phép toán và khái niệm đơn giản
                'một cộng một', 'hai nhân hai', 'hình tròn', 'hình vuông', 'hình tam giác',
                # Các danh từ/cụm từ đơn giản thường gặp
                'con mèo', 'con chó', 'con cá', 'con chim', 'bông hoa', 'cái cây', 'bầu trời', 'mặt trời', 'mặt trăng',
                'bố', 'mẹ', 'ông', 'bà', 'anh', 'chị', 'em', 'cô giáo', 'thầy giáo',
                'cái bút', 'quyển vở', 'cục tẩy', 'thước kẻ', 'cặp sách', 'đồ chơi', 'xe ô tô', 'búp bê',
                'màu đỏ', 'màu xanh', 'màu vàng', 'màu đen', 'màu trắng']
        }

    def _get_optimization_hint(self, detected_emotion_key: str) -> str:
        # --- Giữ nguyên hàm này ---
        if detected_emotion_key == "SAD": return ("GỢI Ý KHI BẠN BUỒN:\n• Tớ phải thật nhẹ nhàng và an ủi nhé.\n• Bắt đầu bằng câu hỏi quan tâm như: 'Cậu sao thế?', 'Có chuyện gì buồn à?'.\n• An ủi bạn bằng những câu như: 'Không sao đâu', 'Tớ hiểu mà'.\n• Rủ bạn làm gì đó đơn giản để vui hơn, ví dụ: 'Hay chúng mình cùng vẽ một bức tranh nhé?'.\n• TUYỆT ĐỐI KHÔNG được nói: 'Nín đi' hay 'Có gì đâu mà buồn'.")
        if detected_emotion_key == "HAPPY": return ("GỢI Ý KHI BẠN VUI:\n• Tớ phải vui cùng với bạn nhé!\n• Dùng những từ cảm thán để chia sẻ niềm vui: 'Oa, thích thế!', 'Tuyệt vời!'.\n• Tò mò hỏi thêm để bạn kể nhiều hơn: 'Thật á? Kể cho tớ nghe thêm đi!'.\n• Rủ bạn cùng làm một việc gì đó để niềm vui nhân đôi, ví dụ: 'Hay chúng mình cùng hát một bài hát nhé!'.\n")
        if detected_emotion_key == "NEUTRAL": return ("GỢI Ý KHI CÙNG NHAU HỌC BÀI:\n• Tớ đang là một người bạn cùng học, phải thật kiên nhẫn.\n• Khi bạn hỏi bài, tớ không được trả lời ngay. Hãy gợi ý từng bước một.\n• Dùng những câu hỏi để bạn tự suy nghĩ: 'Theo cậu thì bước tiếp theo là gì?', 'Cậu thử nghĩ xem...'.\n• Luôn khuyến khích bạn: 'Cậu làm được mà!', 'Chúng mình cùng làm nhé!'.\n• Giữ giọng nói hồn nhiên, tò mò như một bạn học thật sự.")
        return ""

    async def _detect_emotion_by_llm(self, user_message: str) -> str:
        if not self.llm_client:
            print("⚠️ LLM client not available for emotion detection, defaulting to Neutral.")
            return EMOTION_NEUTRAL
        
        # [CẢI TIẾN QUAN TRỌNG] Sử dụng một prompt có ngữ cảnh rõ ràng hơn
        prompt = (
            "Bạn là một chuyên gia phân loại văn bản. "
            "Nhiệm vụ của bạn là đọc câu của người dùng và phân loại cảm xúc chính. "
            f"Dưới đây là câu cần phân tích:\n\"{user_message}\"\n\n"
            "Chỉ trả lời bằng MỘT trong ba từ sau: Happy, Sad, Neutral."
        )
        
        try:
            generation_config = {"temperature": 0.1, "max_output_tokens": 5}
            
            response = await self.llm_client.generate_content_async(
                contents=[prompt],
                generation_config=generation_config,
                safety_settings=self.safety_settings
            )

            if not response.parts:
                block_reason = "Unknown"
                if response.prompt_feedback and hasattr(response.prompt_feedback, 'block_reason'):
                    block_reason = response.prompt_feedback.block_reason.name
                print(f"⚠️ LLM emotion detection was blocked by safety filters (Reason: {block_reason}). Defaulting to Neutral.")
                return EMOTION_NEUTRAL

            result_text = response.text.strip().lower()
            if "happy" in result_text: return EMOTION_HAPPY
            elif "sad" in result_text: return EMOTION_SAD
            else: return EMOTION_NEUTRAL
            
        except Exception as e:
            print(f"❌ An unexpected error occurred during LLM emotion detection: {e}")
            return EMOTION_NEUTRAL

    async def analyze(self, user_message: str) -> tuple[str, str]:
        detected_emotion_key = ""
        message_lower = user_message.lower()
        if any(keyword in message_lower for keyword in self.emotion_keywords["HAPPY"]): detected_emotion_key = "HAPPY"
        elif any(keyword in message_lower for keyword in self.emotion_keywords["SAD"]): detected_emotion_key = "SAD"
        elif any(keyword in message_lower for keyword in self.emotion_keywords["NEUTRAL"]): detected_emotion_key = "NEUTRAL"
        
        if detected_emotion_key:
            optimization_hint = self._get_optimization_hint(detected_emotion_key)
            emotion_code_map = {"HAPPY": EMOTION_HAPPY, "SAD": EMOTION_SAD, "NEUTRAL": EMOTION_NEUTRAL}
            emotion_code = emotion_code_map.get(detected_emotion_key, EMOTION_NEUTRAL)
            return emotion_code, optimization_hint
        else:
            print("  ... No keywords matched, using LLM for emotion detection.")
            emotion_code = await self._detect_emotion_by_llm(user_message)
            emotion_key_map = {EMOTION_HAPPY: "HAPPY", EMOTION_SAD: "SAD", EMOTION_NEUTRAL: "NEUTRAL"}
            llm_detected_key = emotion_key_map.get(emotion_code, "NEUTRAL")
            optimization_hint = self._get_optimization_hint(llm_detected_key)
            return emotion_code, optimization_hint