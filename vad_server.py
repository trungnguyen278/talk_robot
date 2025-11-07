# --- START OF FILE main.py ---

import asyncio
import wave
from datetime import datetime
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
import os
import torch
from collections import deque
import numpy as np

# --- IMPORT PIPELINE TỪ THƯ MỤC MODULES ---
from modules.pipeline import VoiceAssistantPipeline

# --- Cấu hình ---
SAMPLE_RATE = 16000
BIT_DEPTH_BYTES = 2
CHANNELS = 1
AUDIO_CHUNK_SIZE = 1024

# --- Cấu hình VAD ---
VAD_FRAME_MS = 30
VAD_CHUNK_SIZE = (SAMPLE_RATE * VAD_FRAME_MS // 1000) * BIT_DEPTH_BYTES
VAD_SPEECH_THRESHOLD = 0.5
VAD_SILENCE_FRAMES_TRIGGER = 1
VAD_SILENCE_FRAMES_END = 25
VAD_BUFFER_FRAMES = 5

app = FastAPI()

print("\n... (các dòng print khởi tạo pipeline) ...\n")
pipeline = VoiceAssistantPipeline()
print("\n... (các dòng print pipeline ready) ...\n")

try:
    torch.set_num_threads(1)
    vad_model, utils = torch.hub.load(repo_or_dir='snakers4/silero-vad', model='silero_vad', force_reload=False, onnx=True)
    (get_speech_timestamps, save_audio, read_audio, VADIterator, collect_chunks) = utils
    print("Silero VAD model loaded successfully.")
except Exception as e:
    print(f"Error loading Silero VAD model: {e}")
    vad_model = None

def save_audio_to_wav(audio_data: bytes, folder: str = "audio_files") -> str:
    os.makedirs(folder, exist_ok=True)
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    filename = os.path.join(folder, f"recording_{timestamp}.wav")
    try:
        with wave.open(filename, 'wb') as wf:
            wf.setnchannels(CHANNELS)
            wf.setsampwidth(BIT_DEPTH_BYTES)
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(audio_data)
        print(f"Audio received and saved to: {filename}")
        return filename
    except Exception as e:
        print(f"Error saving WAV file: {e}")
        return ""

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    print(f"Client connected from: {websocket.client.host}")
    
    is_speaking = False
    silence_counter = 0
    speech_trigger_counter = 0
    is_processing = False
    
    pre_buffer = deque(maxlen=VAD_BUFFER_FRAMES) 
    speech_buffer = []

    try:
        while True:
            data = await websocket.receive_bytes()

            if is_processing:
                continue

            if len(data) != VAD_CHUNK_SIZE:
                continue

            # Chuyển đổi dữ liệu chính xác
            audio_numpy = np.frombuffer(data, dtype=np.int16)
            audio_tensor = torch.from_numpy(audio_numpy).float() / 32768.0
            
            speech_prob = vad_model(audio_tensor, SAMPLE_RATE).item()

            if speech_prob > VAD_SPEECH_THRESHOLD:
                silence_counter = 0
                if not is_speaking:
                    speech_trigger_counter += 1
                    if speech_trigger_counter >= VAD_SILENCE_FRAMES_TRIGGER:
                        print("==> Voice activity detected. Start recording.")
                        is_speaking = True
                        speech_buffer.extend(list(pre_buffer))
                if is_speaking:
                    speech_buffer.append(data)
            else:
                speech_trigger_counter = 0
                if is_speaking:
                    silence_counter += 1
                    speech_buffer.append(data)
                    if silence_counter >= VAD_SILENCE_FRAMES_END:
                        print("==> Silence detected. End of utterance.")
                        is_processing = True
                        await websocket.send_text("PROCESSING_START")
                        full_audio_data = b"".join(speech_buffer)
                        input_audio_path = save_audio_to_wav(full_audio_data)
                        if input_audio_path:
                            try:
                                result = pipeline.process(audio_input_path=input_audio_path)
                                output_audio_path = result.get("output_audio")
                                if output_audio_path and os.path.exists(output_audio_path):
                                    with open(output_audio_path, 'rb') as audio_file:
                                        while True:
                                            chunk = audio_file.read(AUDIO_CHUNK_SIZE)
                                            if not chunk: break
                                            await websocket.send_bytes(chunk)
                                else:
                                    print("Pipeline did not return a valid audio output path.")
                            except Exception as e:
                                print(f"An error occurred during pipeline processing: {e}")
                            finally:
                                await websocket.send_text("TTS_END")
                                print("Finished streaming response.")
                        is_speaking = False
                        silence_counter = 0
                        speech_buffer.clear()
                        pre_buffer.clear()
                        is_processing = False
                else:
                    pre_buffer.append(data)

    except WebSocketDisconnect:
        print(f"Client {websocket.client.host} disconnected.")
    except Exception as e:
        import traceback
        print(f"A critical error occurred in websocket connection:")
        traceback.print_exc()

@app.get("/")
def read_root():
    return {"status": "Voice Assistant Server is running"}