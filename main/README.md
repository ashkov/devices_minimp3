minimp3 example (dr_mp3)

This project demonstrates decoding an MP3 placed directly into the firmware image and playing it via I2S to PCM5102.

Steps:
1. Copy your mp3 file to `minimp3/main/test.mp3` (replace existing placeholder or add one).
2. Adjust I2S pin definitions in `main.c` if your wiring differs.
3. Build & flash:

```bash
cd minimp3
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Notes:
- The project embeds `test.mp3` into the binary using CMake; the MP3 will be available in memory at runtime.
- We use the official `dr_mp3` single-file decoder. Run the helper to fetch the upstream `dr_mp3.h` implementation:

```bash
./fetch_dr_mp3.sh
```

- Place your `test.mp3` into `minimp3/main/` before building. If you don't fetch `dr_mp3.h`, the project currently contains a placeholder decoder that produces silence.
- Ensure PCM5102 wiring: DIN -> `PIN_DIN`, SCK/BCK -> `PIN_SCK`, LCK -> `PIN_LCK`, GND and VIN as appropriate.

MP3 size considerations:
- The MP3 is embedded into the firmware image; the maximum embeddable size depends on your flash and partition layout. Typical Wemos D1 R32 boards have 4MB flash, but your build logs earlier showed a 2MB image size and factory app partition ~1MB — keep embedded MP3 small (recommended < 200-400 KB) to avoid overflowing the flash partition.
- For larger files, use SPIFFS/LittleFS or an SD card and stream from there instead of embedding.
