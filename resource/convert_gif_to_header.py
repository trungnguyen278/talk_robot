import os
from pathlib import Path
from PIL import Image
import io

# ===============================================================
# 1. Cấu hình chung
# ===============================================================
PATH_INPUT = "emoji_gif"       # Folder chứa GIF
PATH_OUTPUT = "emoji_h"        # Folder xuất file .h
MAX_FRAMES = 80                # Giới hạn số frame
JPEG_QUALITY = 80              # Chất lượng JPG (0-100)
RESIZE = None                  # (width, height) hoặc None

os.makedirs(PATH_OUTPUT, exist_ok=True)

# ===============================================================
# 2. Hàm chuyển từng GIF sang .h
# ===============================================================
def convert_gif_to_h(input_gif_path):
    name = Path(input_gif_path).stem
    output_h_path = f"{PATH_OUTPUT}/{name}.h"
    video_name = name

    print(f"Đang xử lý: {input_gif_path} → {output_h_path}")

    # Mở file GIF
    gif = Image.open(input_gif_path)

    frames = []
    try:
        for i in range(MAX_FRAMES):
            gif.seek(i)
            frame = gif.convert("RGB")
            if RESIZE:
                frame = frame.resize(RESIZE)

            # Lưu frame thành JPEG trong RAM
            buf = io.BytesIO()
            frame.save(buf, format="JPEG", quality=JPEG_QUALITY)
            jpg_bytes = buf.getvalue()

            # Chuyển sang mảng C
            byte_array = ','.join(f"0x{b:02X}" for b in jpg_bytes)
            frames.append(f"const uint8_t frame_{i}[] PROGMEM = {{{byte_array}}};")
    except EOFError:
        pass

    # Viết file .h
    with open(output_h_path, "w", encoding="utf-8") as f:
        f.write(f"#pragma once\n\n")
        f.write(f"#include <pgmspace.h>\n\n")
        for frame_data in frames:
            f.write(frame_data + "\n\n")

        # Danh sách con trỏ frame
        frame_ptrs = ','.join(f"frame_{i}" for i in range(len(frames)))
        f.write(f"const uint8_t* const {video_name}_frames[] PROGMEM = {{{frame_ptrs}}};\n")
        f.write(f"const uint16_t {video_name}_num_frames = {len(frames)};\n")

# ===============================================================
# 3. Duyệt toàn bộ thư mục input và xử lý từng GIF
# ===============================================================
for file in os.listdir(PATH_INPUT):
    if file.lower().endswith(".gif"):
        gif_path = os.path.join(PATH_INPUT, file)
        convert_gif_to_h(gif_path)

print("✅ Hoàn tất chuyển đổi tất cả GIF → .h")
