<p align="right"><a href="soundring-next.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# SoundRing: redesigned experience

This version provides a high-contrast playback screen and a grouped menu. The device controls an existing Spotify Connect player; it does not play audio itself.

Hold OK on the playback screen for Music library, Playback devices, Playback controls, Settings and Sleep. Short UP/DOWN skip tracks, short OK toggles playback, double OK toggles the current track’s saved state, and long UP/DOWN adjust volume. Lists use UP/DOWN to select, OK to open, and long OK to return. Long UP/DOWN advances pages where available.

The Windows pairing center can configure Wi-Fi and Spotify authorization together or separately. Connect the device over USB, enter its pairing page and enter the displayed code in the application. Spotify authorization uses PKCE and a user-provided registered loopback redirect URI. No client secret is required. The tool masks the Wi-Fi password. Device-side credentials remain stored in unencrypted NVS.

Settings provide independent dim and screen-off timeouts, always-on display, brightness and automatic synchronization. Always-on still dims at the configured time; the first gesture after dimming or screen-off only restores the display. The home background changes subtly with the cover colors.

The generated 14px and 20px fonts include the GB2312 level-one common Chinese repertoire in addition to interface text; this improves, but does not guarantee, coverage of arbitrary track metadata. Device tests are required for rendering, Wi-Fi, Spotify authorization, playback, TLS memory and sleep behavior. The prior description in `soundring.md` documents the earlier interface.
