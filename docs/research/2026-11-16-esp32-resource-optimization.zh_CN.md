[English](2026-11-16-esp32-resource-optimization.md) · 简体中文

# SoundRing 内存、显示与 Spotify API 优化调研

调研日期：2026-11-16。本文是设计参考，不代表性能已经在设备上验证。目标设备为 ESP32-C3、无 PSRAM、240×320 RGB565 屏幕；保持现有封面分辨率。

## 本项目现状

- `components/bsp/src/bsp_display_lvgl.c` 为单个 40 行绘图缓冲：240×40×2 = 19,200 字节；已经不是 240×320 的整屏帧缓冲。更小的缓冲需要更多分块，收益与延迟须真机比较。
- `main/sr_cover.c` 的 120×120 RGB565 静态数组占 28,800 字节，运行期间保留；不建议未经连续块及并发验证就改成动态申请。
- `main/sr_http.c` 和 `main/sr_policy.c` 已经提取并处理 Retry-After；`main/sr_spotify.c` 的歌单获取已经采用分页、fields 过滤和新旧端点回退。
- 本地生成的 `sdkconfig` 中 TLS 输入缓冲为 8192、动态 TX/RX 缓冲未启用，保留 peer certificate。配置只代表当前工作树，需确认与交付构建配置一致。

## 官方资料与候选方案

1. **优先可观测性与峰值控制。** ESP-IDF 的 [ESP32-C3 堆调试文档](https://docs.espressif.com/projects/esp-idf/en/v5.1.6/esp32c3/api-reference/system/heap_debug.html) 说明总空闲堆和最大连续块是不同指标；比较两者可帮助发现碎片。建议分别在请求前、TLS 握手、响应读取、JPEG 解码和释放后记录同一能力集合的 free、largest 和低水位。历史低水位不能单独归因给当前操作；建议分场景测量并标注固件版本、采样时刻。测试不得输出 Wi-Fi、令牌或个人内容。
2. **限制同时发生的大型内存操作。** 无 PSRAM 的 [spotify-nowplaying](https://github.com/t-philip/spotify-nowplaying) 项目说明其避免同时保持两个 TLS 连接，并使用流式过滤 JSON。可借鉴“串行化大操作、仅保留必要字段”的思路，但项目使用普通 ESP32、TFT_eSPI 和双核任务，不能把其线程模型直接移植到单核 C3。先检查 SoundRing 已有网络任务与操作调度，再决定是否有重叠需要消除。该项目为 AGPL-3.0，参考设计思想即可；直接复制代码前必须单独核查兼容性和许可义务。
3. **分块绘制的空间与速度折中。** [LVGL 官方 8.4 手册](https://docs.lvgl.io/8/_downloads/39cea4971f327964c804e4e6bc96bfb4/LVGL.pdf) 介绍无效区域与分块刷新；它不是本项目 LVGL 版本的性能保证。当前 40 行约 18.75 KiB；24 行约 11.25 KiB，理论节省 7.5 KiB；20 行约 9.375 KiB，理论节省 9.375 KiB。较小缓冲可能增加刷屏开销；应测量动画、SPI 耗时、按键响应和 DMA 稳定性后再选用。用户尚未要求修改缓冲。
4. **TLS 内存选项需版本和服务器兼容性验证。** [ESP-IDF ESP32-C3 Mbed TLS 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/protocols/mbedtls.html) 展示动态 TX/RX 缓冲和其他配置可能降低其示例的堆占用；示例数据不可当成本项目预期节省。不同 IDF 版本、证书生命周期和重连行为会影响结果，改前需确认实际版本与配置、保持证书和主机名校验，再测试首次授权、令牌刷新、断线重连及 CDN 下载。[ESP-IDF 5.2.5 配置说明](https://docs.espressif.com/projects/esp-idf/en/v5.2.5/esp32c3/esp-idf-en-v5.2.5-esp32c3.pdf) 对动态释放配置/CA 后的重新握手有额外限制；不能盲目开启全部选项。
5. **歌单数据最小化与兼容性。** [Spotify 歌单项目 API](https://developer.spotify.com/documentation/web-api/reference/get-playlists-items) 支持 limit、offset、fields；减少字段可降低传输和解析量，但实际效果要按项目已有实现衡量。[2026 开发模式迁移指南](https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide) 说明相关开发模式中 `/tracks` 更名 `/items`，并限制为用户拥有或协作的歌单；不同配额模式及应用状态要分别核实。此前一次 HTTP 400 并不足以证明是该迁移或内存问题。应记录脱敏后的请求端点、状态和错误类别并复现。
6. **限流与缓存。** [Spotify 限流指南](https://developer.spotify.com/documentation/web-api/concepts/rate-limits) 建议遵守 429 的 Retry-After，歌单可用 snapshot_id 判断版本。SoundRing 已有 429 退避；若进一步引入歌单缓存，应先确认当前 API 能返回所需版本标识、缓存失效与账户变化处理，避免显示过期内容。该指南提及批量 API 不表示所有批量端点在所有应用模式下仍可用，应以当前迁移指南为准。

## 推荐验证顺序

1. 收集真实工作负载的堆内存基线，确认网络操作是否重叠，以及何时出现 400；不改变封面分辨率。
2. 优先修复已复现的功能问题；对重复 UI 更新或网络调度仅做可测试的小范围修改。
3. 如用户希望继续压缩内存，在独立构建中分别测试 40、24、20 行缓冲和 TLS 配置；分别报告最低内存、最大连续块、耗时、视觉效果与回归结果，不将理论值当成真机结果。
4. 任何固件发布前进行本地构建、自动测试和真机回归；保留证书校验、现有授权数据及回滚方案。
