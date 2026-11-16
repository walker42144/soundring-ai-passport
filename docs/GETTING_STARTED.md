<p align="right"><a href="GETTING_STARTED.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Setup and operation

## Flash

Use a data-capable USB cable and identify the device's serial port. Install Python, `esptool` and `pyserial`. The application image requires the partition layout in `partitions.csv`; verify the installed table before using it to update an existing device. Replace `COMX` with the actual port:

```powershell
python -m pip install esptool pyserial
python -m serial.tools.list_ports -v
python -m esptool --chip esp32c3 --port COMX write_flash 0x20000 firmware/soundring-app.bin
```

For a blank device or an intentional full refresh, the merged image is flashed at `0x0` using `firmware/soundring-full.bin`. A full refresh can erase stored configuration. Never write the application-only image at `0x0`. Verify downloaded files against `firmware/SHA256SUMS.txt`.

## Pair Wi-Fi and Spotify

1. In the [Spotify developer dashboard](https://developer.spotify.com/dashboard), use your own app and Client ID. Register a loopback IPv4 redirect URI with an explicit port and path according to [Spotify's redirect URI rules](https://developer.spotify.com/documentation/web-api/concepts/redirect_uri). The exact URI used by the tool must match the app registration. This project does not ship anyone's Client ID or redirect URI.
2. On Windows, install a Python distribution with Tkinter and run:

   ```powershell
   python -m pip install -r requirements-pairing.txt
   python tools/soundring_pairing_gui.py
   ```

3. Connect the device, open **Settings → Computer pairing**, select its serial port in the tool and enter the displayed eight-digit code.
4. Select Wi-Fi, Spotify or both. For Wi-Fi enter your own 2.4 GHz SSID and password. For Spotify enter your own Client ID and registered local redirect URI. Complete browser authorization and wait for confirmation. No Client Secret is needed.

The PowerShell 7 alternative is `pwsh -File tools/soundring-setup.ps1`; it prompts for required values. Use `-SelfTest` for its local cryptographic self-test without connecting a device. Spotify development-mode rules and playlist permissions can change; check the [official migration guide](https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide).

## Controls and troubleshooting

On the home screen, press up/down to skip, OK to pause or resume, double OK to save/unsave, hold up/down for volume, and hold OK for the menu. In lists use up/down to select, OK to open and hold OK to return. Long up/down changes pages where supported. Automatic synchronization uses fast polling while the display is on and approximately 60-second polling while it is off. Dim and screen-off timeouts can be configured separately. Always-on disables automatic screen-off but retains automatic dimming; choose brightness in Settings. The first key gesture after dimming or screen-off only restores the display. Synchronization speed is selectable in Settings; the quick setting trades power and request volume for lower latency, while retry and rate-limit backoff remain active.

If playback information is stale, check Wi-Fi, authorization, the active Spotify player and whether the device is awake on the home screen. If pairing fails, check the cable, serial port, eight-digit code, 2.4 GHz network, Client ID and redirect registration. Playlist availability depends on Spotify account permissions; recent history requires its corresponding scope. Recent and queue views show at most 13 items. Unsupported metadata glyphs are omitted.

## Build and privacy

Activate ESP-IDF 5.5.3, then run `idf.py set-target esp32c3`, `idf.py build` and `idf.py merge-bin -o FoloToy-AI-Passport-full.bin`. For the repository validation workflow see [build and test](development/engineering/build-and-test.md). Font regeneration additionally needs Pillow.

The pairing tool handles credentials in memory, but the device stores Wi-Fi credentials and Spotify tokens in unencrypted NVS. Protect physical access. Never publish full Flash backups, pairing codes, access tokens, refresh tokens, or unsanitized logs. The source project is MIT licensed; bundled fonts and third-party components retain their own licenses.
