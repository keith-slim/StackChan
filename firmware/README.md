
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```

### Voice photo recognition (Gemini Live)

The self-hosted camera voice command reuses the existing
`self.camera.take_photo` MCP tool. ESP32 JPEG capture/upload comes from
`xiaozhi-esp32` (`tts-sync`), and image recognition and spoken results
come from `stackchan-ai-server` (`ja-localization`).

For this feature, update both dependencies **before** flashing this firmware.
The AI server advertises a session-limited image endpoint during MCP
initialization, so the firmware does not need a separately configured
camera-recognition URL or an additional open LAN port. The existing camera,
speech, subtitle and local SD paths remain separate.

Verification after deployment:
1. Wake the robot and say 「写真を撮って」, then confirm the shutter fires.
2. Check the AI-server `[VISION]` logs for initialization, recognition and
   a reply sent to Gemini Live. A tool send log alone is **not** proof of capture.
3. Confirm the robot speaks its image description and displays the subtitle.
4. Check a camera/HTTP failure does not claim an image was recognized.
5. Regression-check normal conversation, servo, LED, quiet mode and camera
   streaming. This requires a physical device; a source commit is not a test.
