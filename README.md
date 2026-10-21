# ESP32BGM — Background Music Module

<img width="800" height="500" alt="ESP32BGM web UI" src="https://github.com/user-attachments/assets/1ae17d7f-0b93-4f5f-8ffe-be9abacf5fd7"/>

![ESP32BGM](image.png)

A stand-alone background-music player built on the **Ai-Thinker ESP32 Audio Kit**
(ESP32-A1S module, ES8388 codec). It plays internet radio or music from the SD
card and is controlled from a built-in web page.

## Features

- **Two playback sources**
  - **Radio URL**: Icecast/Shoutcast HTTP streams, with a station list you can
    edit from the browser. If a stream drops, the player reconnects on its own.
  - **SD card**: every supported file under `/music` (sub-folders included),
    played in sorted order and looped.
  - FTP library (planned; the stub returns “not implemented”).
- **Espressif `esp_audio_codec` decoder**, the same one the Basmio project uses:

  | Format | Extensions | Radio MIME |
  |---|---|---|
  | MP3 | `.mp3` | `audio/mpeg` |
  | AAC / HE-AAC (ADTS) | `.aac` | `audio/aac`, `audio/aacp` |
  | M4A (AAC in MP4) | `.m4a`, `.mp4` | `audio/mp4` |
  | FLAC | `.flac` | `audio/flac` |
  | OGG Vorbis / Opus | `.ogg`, `.oga`, `.opus` | `audio/ogg`, `audio/opus` |
  | WAV (PCM / ADPCM) | `.wav` | `audio/wav` |

- Volume, bass and treble control (3-band EQ).
- Web UI: the main player, a station manager, SD card info and a diagnostics
  page.
- Wi-Fi setup through a captive portal. On first boot, join the
  `ESP32-Radio-Setup` AP (password `radiosetup`).
- Settings are kept across reboots in NVS (Preferences): mode, volume, EQ,
  last station and last track.

## Hardware

| Part | Details |
|---|---|
| Board | Ai-Thinker ESP32 Audio Kit v2.2 (ESP32-A1S, 4 MB flash, PSRAM) |
| Codec | ES8388 (I2C control, I2S audio) |
| Storage | microSD in SD_MMC 1-bit mode |
| Output | Headphone jack / speaker amp on the kit |

> **DIP switches**: SD 1-bit mode must not clash with the keys or JTAG. Use the
> setting from the Ai-Thinker docs for SD + audio.

A 3D-printable enclosure (STL + FreeCAD) is in [`Docs/housing`](Docs/housing).

## Architecture

```
                 ┌──────────────┐
 Radio URL ────► │ URLStream    │──► BufferedStream (64 KB, PSRAM) ─┐
                 └──────────────┘                                   │
 SD card  ─────► File (SD_MMC) ─────────────────────────────────────┤
                                                                     ▼
                                            StreamCopy ──► EncodedAudioStream
                                                                     │
                                         EspAudioDecoder (esp_audio_codec)
                                                                     │  PCM 16-bit stereo
                                                                     ▼
                                       Equalizer3Bands ──► AudioBoardStream (ES8388 / I2S)
                                                                     ▲
                                  sample-rate changes ───────────────┘ (decoder notifies I2S directly)
```

| File | Role |
|---|---|
| `src/main.cpp` | Global objects, constants, `setup()` / `loop()` |
| `src/EspAudioDecoder.cpp`, `include/EspAudioDecoder.h` | arduino-audio-tools `AudioDecoder` wrapping the Espressif simple decoder |
| `src/AudioPipeline.cpp` | I2S/codec init, EQ, starting a decoded stream, codec detection by extension or MIME |
| `src/AudioTask.cpp` | FreeRTOS audio task (16 KB stack) and the state lock shared with the web task |
| `src/ModeManager.cpp` | Switching modes, play / pause / stop / select |
| `src/SourceRadioUrl.cpp` | HTTP radio: open, MIME detection, recovery |
| `src/SourceSdCard.cpp` | SD mount, library scan, track playback and auto-advance |
| `src/NetworkManager.cpp` | Wi-Fi STA, fallback AP and captive DNS |
| `src/WebTask.cpp`, `src/WebHandlers.cpp` | Web server task and HTTP routes |
| `src/SettingsStore.cpp` | NVS settings and `/stations.json` on LittleFS |
| `src/data/*.html` | Web UI templates (uploaded to LittleFS) |
| `lib/esp_audio_codec` | Vendored copy of Espressif `esp_audio_codec` 2.5.0 (ESP32 prebuilt libs) |

### Decoder notes

- `EspAudioDecoder` accepts chunks of any size. It keeps a 16 KB input buffer,
  and the Espressif parser finds frame boundaries and resyncs after corrupt data.
  If 16 KB in a row can't be parsed, they are dropped so playback can recover.
- ID3v2 tags (including large embedded cover art) are skipped before decoding.
- HE-AAC (AAC+/SBR) is enabled, since many AAC radio streams use it.
- The output is always 16-bit stereo: mono is duplicated to both channels, and
  8/24/32-bit sources are converted. Only the sample rate follows the source,
  and the decoder pushes rate changes straight to the ES8388/I2S.
- When an SD track ends, `finish()` flushes the parser (FLAC needs an
  end-of-stream signal) before the next track starts.
- M4A files need the `moov` atom before the audio data (“fast start”) to play
  as a stream. Re-mux with `ffmpeg -i in.m4a -c copy -movflags +faststart out.m4a`
  if needed.
- HLS / `.m3u8` and `.pls` playlist URLs are not supported. Use the direct
  stream URL.

## Building & flashing

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
pio run -t upload
```

```bash
pio run -t uploadfs
```

The first command builds and flashes the firmware. The second uploads the web
UI from `src/data` to LittleFS; run it once, and again whenever the HTML
changes. Then open the serial monitor:

```bash
pio device monitor
```

## Usage

1. On first boot, connect to the **ESP32-Radio-Setup** Wi-Fi network (password
   `radiosetup`). The captive portal opens the Wi-Fi setup page.
2. Once the device is on your network, open its IP address (printed on the
   serial monitor) in a browser.
3. Choose **Radio URL** or **SD Card**, then use Play / Pause, the station or
   track list, volume and EQ.
4. Add or remove stations on **Manage**. For a radio station you can enter the
   MIME type (for example `audio/aac`); leave it empty to use the server's
   `Content-Type`.

### SD card layout

```
/music/
  Album A/01 - Track.mp3
  Album B/song.flac
  ambient.ogg
```

If `/music` doesn't exist, the root of the card is scanned instead.

### HTTP API

| Route | Method | Parameters |
|---|---|---|
| `/` | GET | Main page |
| `/setmode` | POST | `m` = `0` radio, `1` SD, `2` FTP |
| `/play`, `/pause` | POST | — |
| `/selectitem`, `/setstation` | GET/POST | `i` (or `s`) = item index |
| `/setvolume` | GET/POST | `v` = 0–100 |
| `/setbass`, `/settreble` | GET/POST | `v` = −10…10 |
| `/savedefaults` | GET/POST | Saves the current settings |
| `/refreshlibrary` | POST | Rescans the SD card / reloads stations |
| `/getstations` | GET | Station list (JSON) |
| `/addstation` | GET/POST | `name`, `url`, optional `mime` |
| `/deletestation` | GET/POST | `id` |
| `/sdinfo`, `/diagnostics` | GET | Status pages |
| `/savewifi` | POST | `ssid`, `password` |
| `/setup`, `/resetwifi`, `/reboot` | GET | Wi-Fi and system |

## Roadmap

- [ ] Port to native ESP-IDF 5.x (following the Basmio audio backend:
      `esp_codec_dev`, `i2s_std`, `esp_http_server`).
- [ ] FTP library source.
- [ ] MQTT bridge (stubbed in `MqttBridge.cpp`).

## Sources & credits

- Ai-Thinker-Open / ESP32-A1S-AudioKit: https://github.com/Ai-Thinker-Open/ESP32-A1S-AudioKit
- pschatzmann / arduino-audio-tools and arduino-audio-driver
- Espressif `esp_audio_codec`: https://components.espressif.com/components/espressif/esp_audio_codec

## License

The project source code is GPL 3.0 (see [`LICENSE`](LICENSE)).
`lib/esp_audio_codec` is © Espressif Systems and ships under its own
Espressif Modified MIT license ([`lib/esp_audio_codec/LICENSE`](lib/esp_audio_codec/LICENSE)),
which allows use only with Espressif chips.
