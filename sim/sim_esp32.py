#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import asyncio
import websockets
import numpy as np
import sounddevice as sd
import wave
import datetime

WS_URL = "ws://13.239.36.114:8000/ws"
SAMPLE_RATE = 16000
SAMPLES_PER_CHUNK = 512   # 512 mẫu int16 = 1024 bytes
BYTES_PER_SAMPLE = 2
CHUNK_BYTES = SAMPLES_PER_CHUNK * BYTES_PER_SAMPLE

# Trạng thái giống ESP32
STATE_STREAMING = "STREAMING"
STATE_WAITING = "WAITING"
STATE_PLAYING = "PLAYING_RESPONSE"
current_state = STATE_STREAMING

# Log file cho mic và server
mic_log_file = wave.open(f"mic_log_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.wav", "wb")
mic_log_file.setnchannels(1)
mic_log_file.setsampwidth(2)
mic_log_file.setframerate(SAMPLE_RATE)

server_log_file = wave.open(f"server_log_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.wav", "wb")
server_log_file.setnchannels(1)
server_log_file.setsampwidth(2)
server_log_file.setframerate(SAMPLE_RATE)

async def sender(ws):
    global current_state
    with sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16",
                        blocksize=SAMPLES_PER_CHUNK) as mic:
        while True:
            if current_state == STATE_STREAMING:
                data, _ = mic.read(SAMPLES_PER_CHUNK)
                pcm_bytes = data.tobytes()
                if len(pcm_bytes) == CHUNK_BYTES:
                    await ws.send(pcm_bytes)
                    mic_log_file.writeframes(pcm_bytes)  # ghi log mic
            else:
                await asyncio.sleep(0.02)

async def receiver(ws):
    global current_state
    async for message in ws:
        if isinstance(message, bytes):
            # Nhận audio từ server
            if current_state != STATE_PLAYING:
                print("🔊 Bắt đầu phát âm thanh từ server...")
                current_state = STATE_PLAYING
            server_log_file.writeframes(message)  # ghi log server audio
            arr = np.frombuffer(message, dtype=np.int16)
            sd.play(arr, SAMPLE_RATE)
        else:
            # Nhận text control
            print(f"📩 Server text: {message}")
            if message == "PROCESSING_START":
                current_state = STATE_WAITING
                print("⏸️ Dừng mic, server đang xử lý...")
            elif message == "TTS_END":
                current_state = STATE_STREAMING
                print("▶️ Quay lại STREAMING, tiếp tục thu mic.")
            elif message in ["00", "01", "10"]:
                emo = {"00":"NEUTRAL","01":"HAPPY","10":"SAD"}.get(message,"UNKNOWN")
                print(f"😊 Emotion từ server: {emo}")

async def main():
    async with websockets.connect(WS_URL, max_size=None) as ws:
        print("✅ Connected to server")
        await asyncio.gather(sender(ws), receiver(ws))

if __name__ == "__main__":
    try:
        asyncio.run(main())
    finally:
        mic_log_file.close()
        server_log_file.close()
        print("📁 Log WAV files saved.")
