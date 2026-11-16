[English](2026-11-16-esp32-resource-optimization.md) · [简体中文](2026-11-16-esp32-resource-optimization.zh_CN.md)

# SoundRing resource and API research

Research date: 2026-11-16. This is research, not measured performance. The ESP32-C3 target has no PSRAM. Preserve the existing cover size.

- The current 240 × 40 RGB565 display buffer uses 19,200 bytes and already uses partial rendering. [LVGL display documentation](https://lvgl.io/docs/open/9.2/porting/display) describes the tradeoff between buffer size and chunked updates.
- The 120 × 120 RGB565 cover reserves 28,800 bytes. Dynamic allocation needs careful fragmentation and lifetime testing.
- Log free heap and largest free block using the same capabilities; a historical low-water mark cannot identify a particular operation. See [ESP-IDF heap debugging](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c3/api-reference/system/heap_debug.html).
- The current build disables Mbed TLS dynamic buffers. [ESP-IDF RAM guidance](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c3/api-guides/performance/ram-usage.html) identifies TLS settings as candidates, but savings and server compatibility require device measurement; retain certificate and hostname validation.
- [spotify-nowplaying](https://github.com/t-philip/spotify-nowplaying) avoids simultaneous TLS connections and filters JSON; its ESP32 and threading assumptions differ from this C3. It is AGPL-3.0: review licensing before copying code.
- [Spotify playlist items](https://developer.spotify.com/documentation/web-api/reference/get-playlists-items) supports field filtering and pagination. The [2026 migration guide](https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide) documents endpoint and development-mode access changes. One HTTP 400 does not establish the cause.
- Existing code already implements Retry-After backoff. [Spotify rate limits](https://developer.spotify.com/documentation/web-api/concepts/rate-limits) describes backoff and playlist snapshots; any cache must handle account and snapshot changes.

First establish operation-specific memory baselines and fix reproducible behavior, then evaluate one configuration change at a time with device-side tests.
