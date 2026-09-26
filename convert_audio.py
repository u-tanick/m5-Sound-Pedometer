import os
import glob
import math
import wave
import miniaudio
import numpy as np

os.makedirs("data/wav", exist_ok=True)

files = sorted(glob.glob("data/wav/*.wav") + glob.glob("data/wav/*.mp3"))
print("Found audio files:", files)

audio_data = {}

for f in files:
    name = os.path.splitext(os.path.basename(f))[0]
    # Decode MP3 to raw PCM using miniaudio
    decoded = miniaudio.decode_file(f, nchannels=1, sample_rate=22050)
    samples = np.frombuffer(decoded.samples, dtype=np.int16).astype(np.float32)
    duration = len(samples) / decoded.sample_rate
    peak = np.max(np.abs(samples))
    rms = np.sqrt(np.mean(samples ** 2))
    print(f"[{name}] {duration:.2f}s, Peak={peak:.1f}, RMS={rms:.1f}")
    audio_data[name] = {
        "samples": samples,
        "sample_rate": decoded.sample_rate,
        "duration": duration,
        "peak": peak,
        "rms": rms
    }

# Normalize each sound so that Peak is normalized to ~30000 (just under int16 max 32767)
# and RMS has good perceptual volume
target_peak = 30000.0

for name, d in audio_data.items():
    samples = d["samples"]
    cur_peak = d["peak"]
    if cur_peak > 0:
        gain = target_peak / cur_peak
    else:
        gain = 1.0
    
    normalized = samples * gain
    # Clip to int16 range
    normalized = np.clip(normalized, -32768, 32767).astype(np.int16)
    
    new_rms = np.sqrt(np.mean(normalized.astype(np.float32) ** 2))
    print(f"[{name}] Normalized gain={gain:.2f}x -> New Peak={np.max(np.abs(normalized))}, New RMS={new_rms:.1f}")
    
    wav_path = os.path.join("data/wav", f"{name}.wav")
    with wave.open(wav_path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2) # 16-bit
        wf.setframerate(d["sample_rate"])
        wf.writeframes(normalized.tobytes())
    print(f"Saved: {wav_path} ({os.path.getsize(wav_path)} bytes)")

print("\nAll audio converted and normalized to high quality WAV successfully!")
