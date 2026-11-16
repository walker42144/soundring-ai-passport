<p align="right"><a href="soundring.zh_CN.md">简体中文</a> · <strong>English</strong></p>

> This document describes the earlier interface. See [the redesigned experience](soundring-next.md) for the current version.

# SoundRing: Spotify remote for AI Passport

SoundRing turns the AI Passport into a Spotify remote: it shows playback state and sends Web API
commands. Audio always plays on the phone, computer, or another Spotify Connect device; this firmware
never decodes, caches, or downloads audio.

## Screens and keys

The UI is a single 240 × 320 portrait page per screen with a black background and the Spotify green accent.

| Screen | UP / DOWN | Long UP / DOWN | OK | Long OK |
| --- | --- | --- | --- | --- |
| Home | previous / next track | volume ∓5 | play / pause | menu |
| Lists | move | previous / next page | open / play / transfer / toggle | back |
| Volume / Seek | ±5% / ±10 s | ±10% / ±30 s | commit | cancel and back |
| Brightness | ±10% | ±20% | save | cancel and back |
| Pairing / Help | — | — | — | back |

Home shows cover art, title (two lines), artist, active device, a local smooth progress bar with
server calibration, shuffle/repeat/volume state, and the transport row.

### Home footer guidance

The footer line reports the first blocking condition instead of the generic key hint. On-device text is
Simplified Chinese; the table gives its meaning.

| Condition | Footer meaning |
| --- | --- |
| Not authorized | Unauthorized: Settings -> PC pairing |
| Offline | Offline: check the Wi-Fi connection |
| Waiting for SNTP | Syncing time, please wait |
| Authorized but no active device | No active device: start playback on your phone first |
| Everything ready | OK: play/pause, hold OK: menu |

### Menu (long OK on Home)

13 entries in one scrolling window of 6 rows: playlists, queue preview, recently played, devices,
volume, seek, **shuffle**, **repeat**, like current, settings, help, screen off, back to home.

Shuffle and repeat are one-press toggles; their subtitle shows the live server state (Chinese on
device, meaning in parentheses):

* Shuffle — on / off; OK flips it and re-polls after 250 ms.
* Repeat — off / playlist / single track; OK advances `off -> context -> track -> off`.

A toast confirms each toggle ("shuffle on", "repeat: playlist", ...). Commands still go through the
shared 800 ms gate, so rapid double presses are rejected with a "too fast" hint instead of queueing.

## Architecture

| File | Responsibility |
| --- | --- |
| `main/sr_app.c` | Screen stack, key policy, worker task, polling/backoff, UI tick, provisioning callbacks |
| `main/sr_ui.c` | LVGL rendering and design tokens (black, `#1DB954`, 14/20/28 px fonts) |
| `main/sr_spotify.c` | Web API calls, token refresh, 429 classification, 2026 playlist/library endpoints |
| `main/sr_http.c` | Bounded HTTPS requests with certificate bundle, `Retry-After`, 1 KB error bodies |
| `main/sr_cover.c` | 24 KB download cap, ≤120 px JPEG decode, SPIFFS cache (8 files / 512 KB) |
| `main/sr_pair.c` | USB serial pairing: PBKDF2-HMAC-SHA256 + AES-256-GCM |
| `main/sr_policy.c`, `sr_progress.c`, `sr_text.c`, `sr_playback.c` | Host-testable logic |
| `main/fonts/` | Generated Noto Sans SC subsets (14/20/28 px), OFL 1.1 |

Network work runs in one worker task. It does not hold the state mutex during requests, so key
presses stay responsive. LVGL is only touched under `bsp_lvgl_lock()`.

The worker also drives a **local UI tick** on the Home screen: while playing it re-renders text and
the progress bar once per second, while paused once per 5 seconds. The tick never issues a request
and never touches the poll schedule, so the progress bar moves smoothly between (much cheaper)
network polls.

## Polling, battery and rate limits

| `poll_fast` | Playing | Paused | Notes |
| --- | --- | --- | --- |
| Standard (`1`) | 3000 ms | 6000 ms | Manual opt-in, smoother progress |
| Power saving (`0`, **default**) | 5000 ms | 15000 ms | Default floor for a battery device |

Values changed from the watch build's `1000` / `1500` ms. The AI Passport uses an ESP32-C3 with no
PSRAM and a single core, and `sr_http.c` creates a fresh connection per request
(`keep_alive_enable = false`), so every poll is a full TLS handshake. At one poll per second the
handshake dominated CPU time and battery draw, which is unacceptable on a battery device. Read
failures still back off exponentially 2 s → 4 s → 8 s → 16 s → 32 s, and command submission stays
gated at 800 ms.

HTTP 429 is split into two cases because the recovery windows differ by two orders of magnitude:

| Signal | Block | On-device message |
| --- | --- | --- |
| `error.reason = QUOTA_EXCEEDED` | 900 s (15 min) | Dev-mode quota exhausted, retrying in 15 minutes |
| any other 429 | `Retry-After` (1 s … 24 h, default 30 s) | Spotify is rate limiting, retrying automatically |

While a block is active the worker stops polling entirely, so the device does not hammer the API
every couple of seconds and does not repeat the toast.

## Library (like) endpoint

`uris` is a **query** parameter, not a JSON body, and a successful call returns `200` with an empty
body:

* like: `PUT /me/library?uris=<url_encode(uri)>` with no request body
* unlike: `DELETE /me/library?uris=<url_encode(uri)>`
* one request may carry up to 40 URIs; this firmware always sends exactly one track URI

## Lists

Rows whose `uri` matches the current playing track get a "now playing" subtitle prefix (rendered in
Chinese) in the playlists-tracks, recently-played and queue screens. The green highlight for the
cursor is separate and unchanged. Plain text is used instead of ▶-style symbols because the generated
font subset is built only from characters that appear in `main/`.

## First-time setup

1. On the device open **Settings → PC pairing**; it shows an 8-digit code (10 minutes, 5 attempts).
2. Connect USB and run `tools/soundring-setup.ps1 -ClientId <id>` on Windows PowerShell 7+.
3. The tool performs PKCE on a user-provided loopback redirect URI, encrypts Wi-Fi and tokens over the
   serial link, and never prints or stores them.

Scopes: `user-read-playback-state user-modify-playback-state playlist-read-private
playlist-read-collaborative user-read-recently-played user-library-read user-library-modify`.
No Client Secret is used anywhere.

## Settings

| Setting | Values | Stored as |
| --- | --- | --- |
| Brightness | 10…100 %, default 80 % | `bright` (`uint8`), clamped to the same 10…100 range on load |
| Auto screen off | off / 30 / 60 / 120 s | `dim` |
| Sync speed | standard / power saving (default) | `poll` |
| PC pairing | USB serial | — |
| Clear authorization | two-step confirm | erases token keys |

## Screen blanking

Blanking is two-stage and timed from the last key press:

| Stage | Trigger | What happens |
| --- | --- | --- |
| Dim | idle for the configured delay (30 / 60 / 120 s; can be disabled) | backlight drops to 10 % |
| Blank | another 30 s without a key | panel scan-out stops (`esp_lcd_panel_disp_on_off(false)`), then the backlight drops to 0 |

Display off is issued before the backlight, so the panel cannot flash a white frame while it is being
cut. ST7789 display-off keeps GRAM, so the wake path re-enables scan-out, forces a full rebuild and
redraws every widget.

Blank deliberately does not enter deep sleep. Deep sleep would drop the Wi-Fi association and the
SNTP result, so every wake-up would need a reconnect plus a fresh time sync before any request could
succeed. Instead the panel is switched off while the radio, the LVGL port and the pairing task keep
running: wake-up is instant, and the price is that the Wi-Fi link stays associated while blanked, so
standby still draws connection current.

While blanked, both rendering and network polling are suspended: the render path returns early, and
`poll_due_locked()` refuses to run, so no TLS handshake is spent on a dark screen. The worker task
parks on a 5 s cadence, which only serves the pairing callback and the blank timer; wake-up is driven
by the key callback, not by that cadence.

Any key press wakes the device. The waking press is consumed by the wake itself (it does not also
play/pause or move the cursor). **Settings -> Screen off now** and the menu entry blank the device
immediately.

## Storage and security boundary

Tokens live in NVS as plaintext; Flash/NVS encryption is not enabled on this board configuration.
Anyone who can read the Flash can extract them. This is weaker than a platform keystore and must not
be described otherwise. Revoke the app in the Spotify account when disposing of the device, and use
**Settings → Clear authorization** to remove local tokens.

## Build and test

```bash
idf.py build
idf.py merge-bin -o <absolute path>/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
python tools/archive_firmware.py create build --archive-root build/firmware
```

Host logic tests are part of `tools/validate.sh`; the font generator self-verifies every glyph.
**Any new non-ASCII string in `main/` requires regenerating the font subset**, otherwise the glyph is
missing at runtime and the UI shows blank boxes:

```bash
python tools/gen_sr_font.py         # regenerate from main/ string literals
python tests/test_sr_font.py
pwsh -NoProfile -File tools/soundring-setup.ps1 -SelfTest
```

## Limitations

- No OTA; update by flashing.
- Covers larger than 24 KB or not baseline JPEG fall back to a placeholder.
- The cover is decoded to 120 px because that is the exact display area; the internal heap has to
  stay large enough for a TLS handshake (~25–30 KB contiguous) while Wi-Fi is running.
- Development-mode apps can only read playlists the user owns or collaborates on.
- Development-mode quota exhaustion stops polling for 15 minutes; the only fix is waiting or
  switching to an app with production quota.
- Device-level verification (Wi-Fi, TLS, authorization, controls, memory headroom) is required
  before claiming the firmware works on hardware.
