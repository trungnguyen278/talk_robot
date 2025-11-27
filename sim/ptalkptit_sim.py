import asyncio
import threading
import time
import random
import struct
import os
import queue
from enum import Enum

import websockets
import pyaudio
import tkinter as tk
from PIL import Image, ImageTk

# ===============================================================
# 1. CẤU HÌNH (tương đương main.cpp)
# ===============================================================

# Thư mục chứa GIF emotion
EMOJI_DIR = r"C:\Users\LEGION\Desktop\ptit\talk_robot\resource\emoji_gif"

# WebSocket server
WEBSOCKET_SERVER_HOST = "13.239.36.114"
WEBSOCKET_SERVER_PORT = 8000
WEBSOCKET_SERVER_PATH = "/ws"
SERVER_URL = f"ws://{WEBSOCKET_SERVER_HOST}:{WEBSOCKET_SERVER_PORT}{WEBSOCKET_SERVER_PATH}"

TIMEOUT_MS = 20000

# Audio config
I2S_SAMPLE_RATE = 16000
I2S_READ_CHUNK_SIZE = 1024  # bytes (512 samples int16)
SPEAKER_GAIN = 0.5
PLAYBACK_BUFFER_SIZE = 8192
ANIMATION_FRAME_DELAY_MS = 50

# ===============================================================
# 2. STATE & EMOTION (tương đương enum + emotion.h)
# ===============================================================

class State(Enum):
    STATE_OFFLINE_WIFI = 0
    STATE_DISCONNECTED_WS = 1
    STATE_FREE = 2
    STATE_STREAMING = 3
    STATE_WAITING = 4
    STATE_PLAYING_RESPONSE = 5

# Emotion giống define trong emotion.h
EMOTION_NEUTRAL = 0
EMOTION_HAPPY   = 1
EMOTION_SAD     = 2

# Map emotion → file GIF
EMOTION_GIF = {
    EMOTION_NEUTRAL: "binhthuong.gif",
    EMOTION_HAPPY:   "vuive.gif",
    EMOTION_SAD:     "buon.gif",
}

# Stunned (DISCONNECTED_WS)
STUNNED_GIF = "hoamat.gif"

# Thinking (STATE_WAITING)
THINKING_GIF = "suynghi2.gif"

# Animation list cho STATE_FREE (random roaming)
ANIMATION_GIFS = [
    "buonngu.gif",
    "nhaymat.gif",
    "ngacnhien2.gif",
    "nheomat.gif",
    "duamat.gif",
    "doxet.gif",
    "macdinh.gif",
    "camlang.gif",
    "chamhoi.gif",
    "cuoito.gif",
    "khongchiudau.gif",
    "tucgian.gif",
    "buon2.gif",
    "domohoi.gif",
    "tucgian2.gif",
    "vuimung.gif",
]

# ===============================================================
# 3. ADPCM (port y nguyên từ C)
# ===============================================================

index_table = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
]

step_table = [
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
]

class ADPCMState:
    def __init__(self):
        self.predictor = 0
        self.index = 0

def adpcm_encode_block(pcm_samples, state: ADPCMState) -> bytes:
    predictor = state.predictor
    index = state.index
    if index < 0:
        index = 0
    if index > 88:
        index = 88
    step = step_table[index]

    out = bytearray()
    high_nibble = False
    out_byte = 0

    for s in pcm_samples:
        # tăng gain đầu vào giống code C
        sample = int(s * 3.0)
        if sample > 32767:
            sample = 32767
        if sample < -32768:
            sample = -32768

        diff = sample - predictor
        sign = 0
        if diff < 0:
            sign = 8
            diff = -diff

        delta = 0
        temp_step = step

        if diff >= temp_step:
            delta |= 4
            diff -= temp_step
        temp_step >>= 1
        if diff >= temp_step:
            delta |= 2
            diff -= temp_step
        temp_step >>= 1
        if diff >= temp_step:
            delta |= 1

        nibble = delta | sign

        diffq = step >> 3
        if delta & 4:
            diffq += step
        if delta & 2:
            diffq += (step >> 1)
        if delta & 1:
            diffq += (step >> 2)

        if sign:
            predictor -= diffq
        else:
            predictor += diffq

        if predictor > 32767:
            predictor = 32767
        if predictor < -32768:
            predictor = -32768

        index += index_table[nibble]
        if index < 0:
            index = 0
        if index > 88:
            index = 88
        step = step_table[index]

        if not high_nibble:
            out_byte = nibble & 0x0F
            high_nibble = True
        else:
            out_byte |= (nibble & 0x0F) << 4
            out.append(out_byte)
            high_nibble = False

    if high_nibble:
        out.append(out_byte)

    state.predictor = predictor
    state.index = index

    return bytes(out)

def adpcm_decode_block(adpcm_data: bytes, state: ADPCMState):
    predictor = state.predictor
    index = state.index
    if index < 0:
        index = 0
    if index > 88:
        index = 88
    step = step_table[index]

    pcm_out = []

    for b in adpcm_data:
        for shift in (0, 4):
            nibble = (b >> shift) & 0x0F
            sign = nibble & 8
            delta = nibble & 7

            diffq = step >> 3
            if delta & 4:
                diffq += step
            if delta & 2:
                diffq += (step >> 1)
            if delta & 1:
                diffq += (step >> 2)

            if sign:
                predictor -= diffq
            else:
                predictor += diffq

            if predictor > 32767:
                predictor = 32767
            if predictor < -32768:
                predictor = -32768

            index += index_table[nibble]
            if index < 0:
                index = 0
            if index > 88:
                index = 88
            step = step_table[index]

            sample = int(predictor * SPEAKER_GAIN)
            if sample > 32767:
                sample = 32767
            if sample < -32768:
                sample = -32768

            pcm_out.append(sample)

    state.predictor = predictor
    state.index = index
    return pcm_out

# ===============================================================
# 4. BIẾN TOÀN CỤC & RING BUFFER (mô phỏng)
# ===============================================================

current_state = State.STATE_OFFLINE_WIFI
emotion = EMOTION_NEUTRAL

last_received_time = time.time() * 1000.0  # ms

# Hàng đợi ADPCM từ MIC → WebSocket
mic_queue = queue.Queue(maxsize=100)
# Hàng đợi ADPCM từ WebSocket → SPEAKER
spk_queue = queue.Queue(maxsize=100)

# ADPCM state
adpcm_mic_state = ADPCMState()
adpcm_spk_state = ADPCMState()

# Cờ chạy
running = True

state_lock = threading.Lock()

def set_state(s: State):
    global current_state
    with state_lock:
        current_state = s
    print(f"[STATE] -> {s.name}")

def get_state():
    with state_lock:
        return current_state

emotion_lock = threading.Lock()

def set_emotion(e: int):
    global emotion
    with emotion_lock:
        emotion = e
    print(f"[EMOTION] -> {e}")

def get_emotion():
    with emotion_lock:
        return emotion

def update_last_received():
    global last_received_time
    last_received_time = time.time() * 1000.0

def clear_mic_ring_buffer():
    while not mic_queue.empty():
        try:
            mic_queue.get_nowait()
        except queue.Empty:
            break
    print("[RING] Cleared mic ring buffer")

def clear_spk_ring_buffer():
    while not spk_queue.empty():
        try:
            spk_queue.get_nowait()
        except queue.Empty:
            break
    print("[RING] Cleared spk ring buffer")

# ===============================================================
# 5. DISPLAY TASK (GIF UI bằng Tkinter + Pillow)
# ===============================================================

class EmojiDisplay(threading.Thread):
    def __init__(self, emoji_dir):
        super().__init__(daemon=True)
        self.emoji_dir = emoji_dir
        self.root = None
        self.label = None
        self.frames_cache = {}  # filename -> [PIL.ImageTk.PhotoImage,...]
        self.current_frames = None
        self.current_index = 0
        self.current_mode = None  # 'emotion', 'stunned', 'thinking', 'free'
        self.current_file = None

    def load_gif_frames(self, filename):
        path = os.path.join(self.emoji_dir, filename)
        if not os.path.isfile(path):
            print(f"[DISPLAY] GIF not found: {path}")
            return [None]
        if filename in self.frames_cache:
            return self.frames_cache[filename]

        img = Image.open(path)
        frames = []
        try:
            while True:
                frame = img.copy().convert("RGBA")
                frames.append(ImageTk.PhotoImage(frame))
                img.seek(img.tell() + 1)
        except EOFError:
            pass
        if not frames:
            frames = [ImageTk.PhotoImage(Image.new("RGBA", (128, 128), (0, 0, 0, 255)))]
        self.frames_cache[filename] = frames
        print(f"[DISPLAY] Loaded {len(frames)} frames from {filename}")
        return frames

    def select_frames_for_state(self):
        s = get_state()

        if s == State.STATE_FREE:
            # random animation
            mode = "free"
            if self.current_mode != mode or self.current_frames is None or self.current_index >= len(self.current_frames):
                gif_file = random.choice(ANIMATION_GIFS)
                self.current_frames = self.load_gif_frames(gif_file)
                self.current_index = 0
                self.current_mode = mode
                self.current_file = gif_file

        elif s == State.STATE_OFFLINE_WIFI:
            # Có thể hiển thị màn đen; ở đây cứ giữ frame cũ, chớp hơi chậm
            mode = "offline"
            self.current_mode = mode
            if self.current_frames is None:
                # Tạo frame đen
                img = Image.new("RGBA", (240, 240), (0, 0, 0, 255))
                self.current_frames = [ImageTk.PhotoImage(img)]
                self.current_index = 0

        elif s == State.STATE_DISCONNECTED_WS:
            mode = "stunned"
            if self.current_mode != mode or self.current_frames is None:
                self.current_frames = self.load_gif_frames(STUNNED_GIF)
                self.current_index = 0
                self.current_mode = mode
                self.current_file = STUNNED_GIF

        elif s == State.STATE_WAITING:
            mode = "thinking"
            if self.current_mode != mode or self.current_frames is None:
                self.current_frames = self.load_gif_frames(THINKING_GIF)
                self.current_index = 0
                self.current_mode = mode
                self.current_file = THINKING_GIF

        else:
            # STREAMING / PLAYING_RESPONSE: hiển thị emotion hiện tại
            mode = "emotion"
            e = get_emotion()
            gif_file = EMOTION_GIF.get(e, EMOTION_GIF[EMOTION_NEUTRAL])
            if self.current_mode != mode or self.current_file != gif_file or self.current_frames is None:
                self.current_frames = self.load_gif_frames(gif_file)
                self.current_index = 0
                self.current_mode = mode
                self.current_file = gif_file

    def loop(self):
        if not running:
            try:
                self.root.destroy()
            except:
                pass
            return

        self.select_frames_for_state()

        if self.current_frames:
            frame = self.current_frames[self.current_index % len(self.current_frames)]
            if frame is not None:
                self.label.configure(image=frame)
                self.label.image = frame  # giữ reference tránh GC
            self.current_index = (self.current_index + 1) % len(self.current_frames)

        self.root.after(ANIMATION_FRAME_DELAY_MS, self.loop)

    def on_close(self):
        global running
        running = False
        self.root.destroy()

    def run(self):
        self.root = tk.Tk()
        self.root.title("PTalkPTIT Simulator")
        self.label = tk.Label(self.root, bg="black")
        self.label.pack(fill="both", expand=True)
        self.root.configure(bg="black")
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(0, self.loop)
        self.root.mainloop()

# ===============================================================
# 6. AUDIO TASKS (MIC & SPEAKER)
# ===============================================================

def mic_task():
    global running
    pa = pyaudio.PyAudio()
    stream = pa.open(format=pyaudio.paInt16,
                     channels=1,
                     rate=I2S_SAMPLE_RATE,
                     input=True,
                     frames_per_buffer=I2S_READ_CHUNK_SIZE // 2)
    print("[MIC] Started")

    try:
        while running:
            s = get_state()
            if s in (State.STATE_STREAMING, State.STATE_FREE):
                data = stream.read(I2S_READ_CHUNK_SIZE // 2, exception_on_overflow=False)
                # bytes → int16 samples
                num_samples = len(data) // 2
                pcm_samples = struct.unpack("<" + "h" * num_samples, data)
                adpcm_bytes = adpcm_encode_block(pcm_samples, adpcm_mic_state)

                try:
                    mic_queue.put(adpcm_bytes, timeout=0.1)
                except queue.Full:
                    print("[MIC] mic_queue FULL, dropping audio")
            else:
                time.sleep(0.02)
    finally:
        stream.stop_stream()
        stream.close()
        pa.terminate()
        print("[MIC] Stopped")

def speaker_task():
    global running
    pa = pyaudio.PyAudio()
    stream = pa.open(format=pyaudio.paInt16,
                     channels=1,
                     rate=I2S_SAMPLE_RATE,
                     output=True)
    print("[SPK] Started")

    try:
        while running:
            try:
                adpcm_data = spk_queue.get(timeout=0.1)
            except queue.Empty:
                continue

            pcm_samples = adpcm_decode_block(adpcm_data, adpcm_spk_state)
            # int16 → bytes
            pcm_bytes = struct.pack("<" + "h" * len(pcm_samples), *pcm_samples)
            stream.write(pcm_bytes)
    finally:
        stream.stop_stream()
        stream.close()
        pa.terminate()
        print("[SPK] Stopped")

# ===============================================================
# 7. WEBSOCKET LOGIC (mô phỏng onWebsocketEvent + onWebsocketMessage + loop)
# ===============================================================

async def ws_sender(ws):
    """UPSTREAM: gửi ADPCM từ MIC lên server (chính là đoạn 4️⃣ trong loop())"""
    global running
    while running:
        s = get_state()
        if s in (State.STATE_STREAMING, State.STATE_FREE):
            try:
                adpcm_data = mic_queue.get_nowait()
            except queue.Empty:
                await asyncio.sleep(0.01)
                continue

            try:
                await ws.send(adpcm_data)
                # print(f"[WS-TX] {len(adpcm_data)} bytes")
            except Exception as e:
                print("[WS-TX] ERROR:", e)
                set_state(State.STATE_DISCONNECTED_WS)
                break
        else:
            await asyncio.sleep(0.02)

async def ws_receiver(ws):
    """Nhận message từ server, xử lý giống onWebsocketMessage"""
    global running
    while running:
        try:
            msg = await ws.recv()
        except websockets.ConnectionClosed:
            print("[WS] Connection closed")
            set_state(State.STATE_DISCONNECTED_WS)
            break
        except Exception as e:
            print("[WS] recv error:", e)
            set_state(State.STATE_DISCONNECTED_WS)
            break

        update_last_received()

        if isinstance(msg, str):
            text_msg = msg
            print(f"[WS-TEXT] Received: {text_msg}")

            if text_msg == "PROCESSING_START":
                print("Server is processing. Pausing mic.")
                set_state(State.STATE_WAITING)
                set_emotion(EMOTION_NEUTRAL)
                clear_mic_ring_buffer()
                clear_spk_ring_buffer()

            elif text_msg == "TTS_END":
                print("End of TTS. Returning to streaming mode.")
                # playback_buffer_flush: đã phát realtime nên bỏ qua
                set_state(State.STATE_STREAMING)
                set_emotion(EMOTION_NEUTRAL)

            elif text_msg == "LISTENING":
                print("listening")

            else:
                print("Received emotion details from server.")
                if text_msg == "00":
                    set_emotion(EMOTION_NEUTRAL)
                elif text_msg == "01":
                    set_emotion(EMOTION_HAPPY)
                elif text_msg == "10":
                    set_emotion(EMOTION_SAD)
                else:
                    # unknown emotion code
                    pass

        else:
            # Binary: audio ADPCM
            if get_state() != State.STATE_PLAYING_RESPONSE:
                print("Receiving audio from server, pausing mic and starting playback...")
                set_state(State.STATE_PLAYING_RESPONSE)
                clear_mic_ring_buffer()
                clear_spk_ring_buffer()
                adpcm_spk_state.predictor = 0
                adpcm_spk_state.index = 0

            adpcm_bytes = msg
            # Đẩy vào hàng đợi speaker
            try:
                spk_queue.put(adpcm_bytes, timeout=0.1)
            except queue.Full:
                print("[SPK] spk_queue FULL, dropping audio")

        await asyncio.sleep(0)  # nhường vòng lặp

async def ws_timeout_ping(ws):
    """Giống đoạn 3️⃣ TIMEOUT trong loop() – ping nếu WS idle"""
    global running
    while running:
        await asyncio.sleep(1.0)
        s = get_state()
        now_ms = time.time() * 1000.0
        if s in (State.STATE_STREAMING, State.STATE_FREE):
            if now_ms - last_received_time > TIMEOUT_MS:
                print("[TIMEOUT] WS idle, sending ping...")
                try:
                    pong_waiter = await ws.ping()
                    await pong_waiter
                    print("[TIMEOUT] WS ping sent.")
                    set_state(State.STATE_FREE)
                except Exception as e:
                    print("[TIMEOUT] WS dead -> Disconnect", e)
                    set_state(State.STATE_DISCONNECTED_WS)
                    await ws.close()
                    break
                update_last_received()

async def ws_main_loop():
    """Tương đương phần WebSocket trong loop(): reconnect mỗi 5s nếu mất"""
    global running
    while running:
        try:
            print(f"[WS] Connecting to {SERVER_URL} ...")
            set_state(State.STATE_DISCONNECTED_WS)
            async with websockets.connect(SERVER_URL, ping_interval=None, max_size=None) as ws:
                print("[WS] Connected.")
                set_state(State.STATE_STREAMING)
                update_last_received()

                sender = asyncio.create_task(ws_sender(ws))
                receiver = asyncio.create_task(ws_receiver(ws))
                timeout_task = asyncio.create_task(ws_timeout_ping(ws))

                done, pending = await asyncio.wait(
                    [sender, receiver, timeout_task],
                    return_when=asyncio.FIRST_COMPLETED
                )

                for task in pending:
                    task.cancel()

        except Exception as e:
            print("[WS] Connect failed:", e)
            set_state(State.STATE_DISCONNECTED_WS)

        if not running:
            break

        print("[WS] Reconnecting in 5 seconds...")
        await asyncio.sleep(5)

# ===============================================================
# 8. MAIN (tương đương setup() + loop())
# ===============================================================

def main():
    global running

    print("==============================================")
    print("  PTalkPTIT - ESP32 Client Simulator (Python)")
    print("==============================================")

    # Start UI
    display = EmojiDisplay(EMOJI_DIR)
    display.start()

    # Start MIC & SPEAKER threads
    t_mic = threading.Thread(target=mic_task, daemon=True)
    t_spk = threading.Thread(target=speaker_task, daemon=True)
    t_mic.start()
    t_spk.start()

    # Ban đầu coi như WiFi ok, chờ WS kết nối
    set_state(State.STATE_DISCONNECTED_WS)

    try:
        asyncio.run(ws_main_loop())
    except KeyboardInterrupt:
        print("KeyboardInterrupt, stopping...")
    finally:
        running = False
        time.sleep(0.5)
        print("Simulator stopped.")

if __name__ == "__main__":
    main()
