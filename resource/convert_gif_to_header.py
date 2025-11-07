import os
from PIL import Image, ImageSequence
from io import BytesIO

# =========================
# ⚙️ CẤU HÌNH NGƯỜI DÙNG
# =========================
PATH_INPUT = "emoji_gif"          # Thư mục chứa file GIF
PATH_OUTPUT = "emoji_h"           # Thư mục xuất file .h
MAX_FRAMES = 80                   # Giới hạn số frame (tránh đầy flash)
JPEG_QUALITY = 80                 # Chất lượng JPG (0-100)
RESIZE = None                     # (160,128) hoặc None
# =========================


def gif_to_jpg_bytes(frame):
    """Chuyển frame Pillow -> bytes JPG"""
    buf = BytesIO()
    frame.save(buf, format="JPEG", quality=JPEG_QUALITY)
    return buf.getvalue()


def write_header(video_name, frames_data, output_file):
    """Sinh file .h chuẩn kiểu ESP32"""
    with open(output_file, "w", encoding="utf-8") as f:
        f.write(f"#ifndef {video_name.upper()}_H\n#define {video_name.upper()}_H\n\n\n\n")

        # --- 1. Từng frame ---
        for i, data in enumerate(frames_data):
            f.write(f"const uint8_t {video_name}_jpg_frame_{i}[] PROGMEM = {{\n")
            for j, b in enumerate(data):
                if j % 16 == 0:
                    f.write("  ")
                f.write(f"0x{b:02X},")
                if j % 16 == 15:
                    f.write("\n")
            f.write("\n};\n\n")

        # --- 2. Danh sách con trỏ ---
        f.write(f"const uint8_t* const {video_name}_frames[] PROGMEM = {{\n")
        for i in range(len(frames_data)):
            f.write(f"  {video_name}_jpg_frame_{i},\n")
        f.write("};\n\n")

        # --- 3. Kích thước từng frame ---
        f.write(f"const uint16_t {video_name}_frame_sizes[] PROGMEM = {{\n")
        for data in frames_data:
            f.write(f"  {len(data)},\n")
        f.write("};\n\n")

        # --- 4. Tổng số frame ---
        f.write(f"const uint16_t {video_name}_NUM_FRAMES = {len(frames_data)};\n\n")

        # --- 5. Struct VideoInfo ---
        f.write(f"VideoInfo {video_name} = {{\n")
        f.write(f"    {video_name}_frames,\n")
        f.write(f"    {video_name}_frame_sizes,\n")
        f.write(f"    {video_name}_NUM_FRAMES\n")
        f.write("};\n\n#endif\n")

    print(f"✅ Hoàn tất tạo {output_file} ({len(frames_data)} frames).")


def process_gif(input_gif, output_h):
    """Xử lý 1 file GIF"""
    frames_data = []
    with Image.open(input_gif) as im:
        print(f"🔍 Đang xử lý {input_gif} ...")
        for i, frame in enumerate(ImageSequence.Iterator(im)):
            if i >= MAX_FRAMES:
                print(f"⚠️ Dừng ở frame {MAX_FRAMES}, tránh đầy bộ nhớ flash.")
                break
            frame = frame.convert("RGB")
            if RESIZE:
                frame = frame.resize(RESIZE)
            jpg_bytes = gif_to_jpg_bytes(frame)
            frames_data.append(jpg_bytes)
            print(f"🖼️ Frame {i:03d} - {len(jpg_bytes)} bytes")

    video_name = os.path.splitext(os.path.basename(input_gif))[0]
    write_header(video_name, frames_data, output_h)


# === MAIN ===
if not os.path.exists(PATH_INPUT):
    print(f"❌ Không tìm thấy thư mục {PATH_INPUT}")
    exit(1)

os.makedirs(PATH_OUTPUT, exist_ok=True)

# Duyệt tất cả các file GIF trong thư mục
for file in os.listdir(PATH_INPUT):
    if file.lower().endswith(".gif"):
        input_gif = os.path.join(PATH_INPUT, file)
        name = os.path.splitext(file)[0]
        output_h = os.path.join(PATH_OUTPUT, f"{name}.h")
        process_gif(input_gif, output_h)

print("🎉 Hoàn tất chuyển đổi toàn bộ GIF → .h")
