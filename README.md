# ESP32BGM — Background Music Module

<img width="800" height="500" alt="ESP32BGM web UI" src="https://github.com/user-attachments/assets/1ae17d7f-0b93-4f5f-8ffe-be9abacf5fd7"/>

![ESP32BGM](image.png)

A stand-alone background-music player built on the **Ai-Thinker ESP32 Audio Kit**
(ESP32-A1S module, ES8388 codec). It plays internet radio or music from the SD
card and is controlled from a built-in web page.

> **This branch (`idf-port`) is the native ESP-IDF 5.x firmware.** The original
> Arduino/PlatformIO version is on the `bgm-core` branch.

## Features

- **Two playback sources**
  - **Radio URL**: HTTP and HTTPS Icecast/Shoutcast streams, with a station list
    you can edit from the browser. `.m3u`/`.pls` playlist URLs are followed to
    the first stream. Streams are pre-buffered (128 KB ring in PSRAM) and
    reconnect on their own when they drop.
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

- Hardware volume on the ES8388, plus a bass/treble 3-band EQ.
- Web UI: the main player, a station manager, SD card info and a diagnostics
  page. The pages are built into the firmware, so there is no filesystem upload.
- Wi-Fi setup through a captive portal. When no Wi-Fi is configured, or the
  connection fails, the device opens the `ESP32-Radio-Setup` AP (password
  `radiosetup`).
- Settings are kept across reboots in NVS: mode, volume, EQ, last station,
  last track and the station list.

## Hardware

| Part | Details |
|---|---|
| Board | Ai-Thinker ESP32 Audio Kit v2.2 (ESP32-A1S, 4 MB flash, PSRAM) |
| Codec | ES8388: I2C SCL 32 / SDA 33, I2S MCLK 0 / BCLK 27 / WS 25 / DOUT 26, PA enable 21 |
| Storage | microSD, SDMMC 1-bit (CLK 14, CMD 15, D0 2) |
| Output | Headphone jack / speaker amp on the kit |

> **DIP switches**: SD 1-bit mode must not clash with the keys or JTAG. Use the
> setting from the Ai-Thinker docs for SD + audio.

A 3D-printable enclosure (STL + FreeCAD) is in [`Docs/housing`](Docs/housing).

## Architecture

```
 Radio URL ──► reader task (esp_http_client, core 0) ──► 128 KB PSRAM stream buffer ─┐
                                                                                     │
 SD card  ───► FILE* on FATFS (/sdcard, 16 KB buffer) ───────────────────────────────┤
                                                                                     ▼
                                                 audio task (core 1): 2 KB chunks per lock hold
                                                                                     │
                                                    EspAudioDecoder (esp_audio_codec)
                                                                                     │ PCM 16-bit stereo
                                                                                     ▼
                                              3-band EQ ──► esp_codec_dev ──► ES8388 / I2S
                                         (the codec is reopened when the source sample rate changes)
```

| File (`main/`) | Role |
|---|---|
| `main.cpp` | Constants, global state, `app_main()` |
| `app_state.h` | Shared types and every cross-module declaration |
| `audio_decoder.cpp/.h` | `EspAudioDecoder`, a wrapper around the Espressif simple decoder |
| `audio_output.cpp` | I2C + I2S + ES8388 through `esp_codec_dev`, the EQ, volume, sample-rate changes |
| `audio_task.cpp` | Audio task, the state lock (a recursive mutex), starting/stopping streams, codec detection |
| `mode_manager.cpp` | Switching modes, play / pause / stop / select |
| `source_radio.cpp` | HTTP(S) stream sessions, playlist following, pre-buffering, recovery |
| `source_sd.cpp` | SD mount, recursive library scan, track playback and auto-advance |
| `source_ftp.cpp` | FTP stub |
| `network_manager.cpp` | Wi-Fi STA with auto-reconnect, the setup AP |
| `captive_dns.cpp` | DNS server that sends every hostname to the setup AP |
| `settings_store.cpp` | NVS settings (namespace `radio`) and the stations JSON |
| `web_server.cpp` | `esp_http_server` routes and HTML templating |
| `web/*.html` | Web UI templates (embedded with `EMBED_TXTFILES`) |

Dependencies come from the ESP Component Registry (`main/idf_component.yml`):
`espressif/esp_audio_codec` and `espressif/esp_codec_dev`.

### Decoder notes

- `EspAudioDecoder` accepts chunks of any size. It keeps a 16 KB input buffer,
  and the Espressif parser finds frame boundaries and resyncs after corrupt data.
  If 16 KB in a row can't be parsed, they are dropped so playback can recover.
- ID3v2 tags (including large embedded cover art) are skipped before decoding.
- HE-AAC (AAC+/SBR) is enabled, since many AAC radio streams use it.
- The output is always 16-bit stereo: mono is duplicated to both channels, and
  8/24/32-bit sources are converted. When the sample rate changes, the codec is
  reopened at the new rate.
- When an SD track ends, `finish()` flushes the parser (FLAC needs an
  end-of-stream signal) before the next track starts.
- M4A files need the `moov` atom before the audio data (“fast start”) to play
  as a stream. Re-mux with `ffmpeg -i in.m4a -c copy -movflags +faststart out.m4a`
  if needed.
- HLS (`.m3u8` with `#EXT-X-` segments) is not supported.

## Building & flashing

Requires **ESP-IDF 5.4 or newer** (developed with 5.5.1).

```bash
idf.py set-target esp32
```

```bash
idf.py build
```

```bash
idf.py -p COM5 flash monitor
```

Replace `COM5` with your board's serial port. The first build downloads the
managed components. `sdkconfig.defaults` turns on PSRAM, the custom partition
table (`partitions.csv`: 64 KB NVS, ~3.9 MB app), HTTPS with the certificate
bundle, FATFS long file names and the Wi-Fi/LWIP buffer sizes for streaming.

## Usage

1. On first boot, connect to the **ESP32-Radio-Setup** Wi-Fi network (password
   `radiosetup`). The captive portal opens the main page; go to **Setup** and
   enter your Wi-Fi network.
2. Once the device is connected, open the IP address shown on the serial
   monitor (`net: connected, IP …`) in a browser.
3. Choose **Radio URL** or **SD Card**, then use Play / Pause, the station or
   track list, volume and EQ.
4. Add or remove stations on **Manage**. For a radio station you can enter the
   MIME type (for example `audio/aac`); leave it empty to use the server's
   `Content-Type`, then the URL extension.

### SD card layout

```
/music/
  Album A/01 - Track.mp3
  Album B/song.flac
  ambient.ogg
```

If `/music` doesn't exist, the root of the card is scanned instead. Up to 1000
tracks are indexed.

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

Any other path returns the main page, which is what captive-portal checks expect.

## Changes from the Arduino version

| Area | Arduino (`bgm-core`) | ESP-IDF (`idf-port`) |
|---|---|---|
| Framework | Arduino core 2.0.17 (IDF 4.4) | ESP-IDF 5.x |
| Codec driver | arduino-audio-driver | `esp_codec_dev` (ES8388) |
| Volume | Software gain | ES8388 hardware volume |
| Radio buffer | 64 KB, read in the audio task | 128 KB PSRAM, filled by a separate reader task, with pre-buffering |
| HTTPS / playlists | No | Yes (certificate bundle) / `.m3u`, `.pls` |
| Web pages | LittleFS, needs `uploadfs` | Built into the firmware |
| Stations | `/stations.json` on LittleFS | JSON blob in NVS (`radio/stations`) |
| Wi-Fi drop | Reconnected only when the radio recovery ran | Reconnects on its own |
| No credentials in SD mode | No web UI | Setup AP starts |
| Static internal RAM | ~94 KB | ~37 KB |

### Not yet at feature parity

- **Chip temperature** on the diagnostics page shows “Unavailable”: IDF 5
  supports no temperature sensor on the original ESP32.
- **FTP library** and the **MQTT bridge** are still stubs, as in the Arduino
  version. The FTP/MQTT settings keys are no longer read.
- Settings written by the Arduino firmware use the same NVS keys, but the larger
  NVS partition means they may not survive the switch. Plan to re-enter Wi-Fi
  and stations once.
- The firmware is build-tested only; it has not been run on hardware yet.

## Roadmap

- [ ] Test on hardware (ES8388 init, SD 1-bit, HTTPS streams).
- [ ] FTP library source.
- [ ] MQTT bridge.
- [ ] Hardware keys (GPIO 36/13/19/23/18/5) for play/pause/next/volume.

## Sources & credits

- Ai-Thinker-Open / ESP32-A1S-AudioKit: https://github.com/Ai-Thinker-Open/ESP32-A1S-AudioKit
- Espressif `esp_audio_codec`: https://components.espressif.com/components/espressif/esp_audio_codec
- Espressif `esp_codec_dev`: https://components.espressif.com/components/espressif/esp_codec_dev
- The 3-band EQ algorithm comes from musicdsp.org, via pschatzmann/arduino-audio-tools.

## License

GPL 3.0 (see [`LICENSE`](LICENSE)). The `esp_audio_codec` component is
© Espressif Systems and ships under its own Espressif Modified MIT license,
which allows use only with Espressif chips.
