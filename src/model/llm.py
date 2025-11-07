import os
import json
import time
import re
from pathlib import Path
from typing import List, Dict, Optional, Tuple, Any
import asyncio

import google.generativeai as genai
from google.generativeai import types
from google.generativeai.types import HarmCategory, HarmBlockThreshold

from settings import llm_settings as cfg
from .emotion_manager import EmotionManager, EMOTION_NEUTRAL


class SimpleRAG:
    # --- Class SimpleRAG giữ nguyên ---
    def __init__(
        self,
        folder: str,
        chunk_size: int = None,
        overlap: int = None
    ):
        self.folder = Path(folder)
        self.chunk_size = chunk_size or cfg.RAG_CHUNK_SIZE
        self.overlap = overlap or cfg.RAG_CHUNK_OVERLAP
        self.chunks: List[Tuple[str, str]] = []
        self._loaded = False

    def _tokenize(self, s: str) -> List[str]:
        return re.findall(r"\w+", s.lower(), flags=re.UNICODE)

    def _load(self):
        if self._loaded:
            return
        if not self.folder.exists():
            self.folder.mkdir(parents=True, exist_ok=True)
            self._loaded = True
            return
        doc_count = 0
        for file_path in self.folder.rglob("*.txt"):
            try:
                with open(file_path, "r", encoding="utf-8") as f:
                    text = f.read()
                i = 0
                while i < len(text):
                    chunk = text[i:i + self.chunk_size]
                    self.chunks.append((str(file_path), chunk))
                    i += max(1, self.chunk_size - self.overlap)
                doc_count += 1
            except Exception as e:
                print(f"  ⚠️  Error loading {file_path}: {e}")
        self._loaded = True

    def search(self, query: str, top_k: int = None) -> List[Dict[str, any]]:
        self._load()
        if not self.chunks:
            return []
        top_k = top_k or cfg.RAG_TOP_K
        q_tokens = set(self._tokenize(query))
        scored = []
        for src, chunk in self.chunks:
            c_tokens = set(self._tokenize(chunk))
            score = len(q_tokens & c_tokens)
            if score > 0:
                scored.append((score, src, chunk))
        scored.sort(key=lambda t: t[0], reverse=True)
        results = []
        for score, src, chunk in scored[:top_k]:
            results.append({
                "source": Path(src).name,
                "score": int(score),
                "text": chunk
            })
        return results


class ChatHistory:
    # --- Class ChatHistory giữ nguyên ---
    def __init__(self, history_dir: str = None):
        self.history_dir = Path(history_dir or cfg.HISTORY_DIR)
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.history_file = self.history_dir / "history.json"
        self.memory: Dict[str, List[Dict]] = {}

    def add(self, session_id: str, role: str, text: str, emotion_code: Optional[str] = None):
        messages = self.memory.setdefault(session_id, [])
        new_message = {"role": role, "content": text, "timestamp": time.time()}
        if emotion_code:
            new_message["emotion"] = emotion_code
        messages.append(new_message)
        if len(messages) > cfg.MAX_HISTORY_TURNS * 2:
            self.memory[session_id] = messages[-cfg.MAX_HISTORY_TURNS * 2:]
        try:
            log_entry = {"session_id": session_id, **new_message}
            with open(self.history_file, "a", encoding="utf-8") as f:
                f.write(json.dumps(log_entry, ensure_ascii=False) + "\n")
        except Exception as e:
            print(f"⚠️  Failed to save history: {e}")

    def get_history(self, session_id: str) -> List[Dict]:
        return self.memory.get(session_id, [])


class LLMEngine:
    """LLM Engine with Gemini API, RAG, and Emotion Analysis"""
    def __init__(self):
        self.client: Optional[genai.GenerativeModel] = None
        self.rag = SimpleRAG(cfg.RAG_DIR)
        self.history = ChatHistory()
        self._initialize_client()
        self.emotion_manager = EmotionManager(self.client)

    def _initialize_client(self):
        api_key = cfg.GEMINI_API_KEY
        if not api_key or "YOUR_API_KEY" in api_key:
            raise ValueError("❌ GEMINI_API_KEY chưa được cấu hình!")
        genai.configure(api_key=api_key)
        self.client = genai.GenerativeModel(cfg.GEMINI_MODEL)

    def _build_system_prompt(self) -> str:
        return cfg.ROLE_PROMPT + "\n" + cfg.SAFETY_PROMPT

    def _format_rag_context(self, docs: List[Dict]) -> str:
        if not docs: return ""
        context_parts = ["\n=== TÀI LIỆU THAM KHẢO ==="]
        for i, doc in enumerate(docs, 1):
            context_parts.append(f"[Tài liệu {i} - {doc['source']}]:\n{doc['text']}")
        context_parts.append("=== KẾT THÚC TÀI LIỆU ===")
        return "\n\n".join(context_parts)

    def _format_history_for_gemini(self, history: List[Dict]) -> List[Dict[str, Any]]:
        out: List[Dict[str, Any]] = []
        for msg in history:
            role = "user" if msg["role"] == "user" else "model"
            text = msg.get("content") or ""
            out.append({'role': role, 'parts': [text]})
        return out

    def chat(
        self,
        text: str,
        session_id: str = "default",
        use_rag: bool = True
    ) -> Tuple[str, Dict[str, Any]]:
        self.history.add(session_id, "user", text)
        emotion_code, optimization_hint = asyncio.run(self.emotion_manager.analyze(text))

        rag_context = ""
        if use_rag:
            docs = self.rag.search(text)
            if docs:
                rag_context = self._format_rag_context(docs)

        enhanced_prompt = "\n".join(
            [self._build_system_prompt(), optimization_hint, rag_context]
        ).strip()
        
        history = self.history.get_history(session_id)
        gemini_history = self._format_history_for_gemini(history[:-1])
        
        generation_config = types.GenerationConfig(
            temperature=cfg.TEMPERATURE,
            max_output_tokens=cfg.MAX_OUTPUT_TOKENS,
            top_p=cfg.TOP_P,
            top_k=cfg.TOP_K,
        )

        try:
            contents_for_api = [
                {'role': 'user', 'parts': [enhanced_prompt]},
                {'role': 'model', 'parts': ["Dạ vâng ạ. Tớ đã hiểu rồi."]},
            ] + gemini_history + [{'role': 'user', 'parts': [text]}]
            
            # [SỬA LỖI] Định nghĩa và truyền các cài đặt an toàn vào API
            safety_settings = {
                HarmCategory.HARM_CATEGORY_HARASSMENT: HarmBlockThreshold.BLOCK_NONE,
                HarmCategory.HARM_CATEGORY_HATE_SPEECH: HarmBlockThreshold.BLOCK_NONE,
                HarmCategory.HARM_CATEGORY_SEXUALLY_EXPLICIT: HarmBlockThreshold.BLOCK_NONE,
                HarmCategory.HARM_CATEGORY_DANGEROUS_CONTENT: HarmBlockThreshold.BLOCK_NONE,
            }

            response = self.client.generate_content(
                contents=contents_for_api,
                generation_config=generation_config,
                safety_settings=safety_settings  # Thêm tham số này vào
            )

            # [CẢI TIẾN] Kiểm tra kỹ hơn trước khi truy cập `response.text`
            if not response.parts:
                finish_reason = "UNKNOWN"
                if hasattr(response, 'prompt_feedback') and response.prompt_feedback:
                    finish_reason = response.prompt_feedback.block_reason.name
                # Ném ra lỗi rõ ràng hơn để khối `except` có thể bắt được
                raise ValueError(f"Response from Gemini was blocked. Finish reason: {finish_reason}")

            reply = response.text
            self.history.add(session_id, "assistant", reply, emotion_code=emotion_code)
            
            result_json = {"user_chat": text, "bot_chat": reply, "emotion": emotion_code}
            return reply, result_json

        except Exception as e:
            # Khối `except` này bây giờ sẽ bắt được cả lỗi do bị chặn
            print(f"❌ LLM Error: {e}")
            
            safe_msg = "Xin lỗi, tớ đang bị mệt một chút. Cậu thử lại sau nhé."
            self.history.add(session_id, "assistant", safe_msg, emotion_code=EMOTION_NEUTRAL)
            
            error_json = {"user_chat": text, "bot_chat": safe_msg, "emotion": EMOTION_NEUTRAL}
            return safe_msg, error_json