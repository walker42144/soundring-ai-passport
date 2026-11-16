<p align="right"><a href="README.md">简体中文</a> · <strong>English</strong></p>

# SoundRing for AI Passport

A three-button Spotify Connect remote for the FoloToy AI Passport (ESP32-C3, 8 MB flash, no PSRAM, 240 × 320 display). It controls an existing Spotify player and does not play audio itself. The public project contains source, a Windows pairing tool, host tests and prebuilt images. It does not include the author's network settings or Spotify application configuration.

## Features

- Playback information, artwork and a locally interpolated progress bar; playback controls and double-click to save/unsave.
- Playlists, recent listening and the playback queue, with bounded streaming for large responses. Recent and queue views contain up to 13 items.
- Wi-Fi and Spotify PKCE setup together or separately through a USB pairing session.
- Automatic, eco, normal and quick synchronization modes; automatic uses quick sync while the screen is on and roughly one-minute polling while it is off; the quick mode polls approximately every 2 seconds while playing and every 3 seconds otherwise. Network errors and Spotify rate limits still take precedence. Faster polling uses more power.
- Adjustable dim and screen-off timers, always-on display, adjustable brightness and wake-only first key press.
- Artwork-based ambient background with a slow color transition.
- Chinese and Japanese font coverage and a fallback that omits unsupported metadata characters without adding spaces.

## Getting started

1. Read the [Chinese setup guide](README.zh_CN.md) or the [English setup guide](docs/GETTING_STARTED.md).
2. If your device already has a compatible partition table, flash `firmware/soundring-app.bin` at `0x20000`. If you do not know the installed layout, do not use the application-only procedure until it is verified. For a deliberate complete refresh, `firmware/soundring-full.bin` starts at `0x0` and may overwrite stored settings.
3. Open **Settings → Computer pairing** on the device. Run the Windows pairing tool, enter the on-screen code, choose Wi-Fi and/or Spotify authorization, and provide your own credentials and registered local redirect URI.
4. Start Spotify on an existing playback device. On SoundRing hold OK to open the menu.

## Source layout

`main/` contains the application, `components/bsp/` the board support, `tools/soundring_pairing_gui.py` the pairing UI, and `tools/soundring-setup.ps1` the PowerShell alternative. `assets/fonts/` contains the font source and license. `tests/` contains host tests; `firmware/` contains the verified prebuilt image pair.

Built with ESP-IDF 5.5.3 and `sdkconfig.defaults`; inspect `partitions.csv` before flashing. This is an independent project, not an official Spotify or FoloToy product. Spotify API availability and account restrictions may change.

## License and privacy

The project is under the [MIT License](LICENSE); bundled third-party code and fonts retain their own licenses. Device-side Wi-Fi credentials and Spotify tokens are stored in unencrypted NVS; protect physical access and do not upload flash backups or logs. See the [English guide](docs/GETTING_STARTED.md) for operating details and limitations.

## Immersive mode and device status

On the home screen, double press Next for immersive mode or Previous for device status. Hold OK to return home; use Up/Down to switch status pages. The screen-off policy schedules playback synchronization approximately once per minute and requests a fresh synchronization on wake, subject to connectivity and Spotify rate limits.
