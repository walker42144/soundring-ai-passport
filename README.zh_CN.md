<p align="right"><a href="README.md">中文首页</a> · <strong>简体中文</strong> · <a href="README.en.md">English</a></p>

# SoundRing · AI Passport Spotify 遥控器

为 FoloToy AI Passport 制作的三键 Spotify Connect 遥控器。它控制手机、电脑或其他 Spotify 播放设备，**自身不播放音频**。本仓库提供固件源码、可刷入固件、Windows 配网授权工具和使用教程。

适用硬件：ESP32-C3、8 MB Flash、无 PSRAM、240 × 320 屏幕。请勿将本固件直接用于其他开发板。

## 功能

- 首页显示歌曲、歌手、封面和播放进度，支持播放控制；双击确定收藏或取消收藏。
- 音乐库中的歌单、最近播放和播放队列；最近播放与队列最多显示 13 条。
- Windows 配对工具可配置 Spotify 授权和最多 4 组 Wi-Fi；可只添加备用网络，之后在设备设置中手动切换。
- 四种同步速度：自动、省电、标准和快速。自动模式在亮屏时采用快速同步、息屏后约每分钟同步一次；快速模式在播放中约每 2 秒、暂停或等待播放时约每 3 秒查询一次；Spotify 限流和失败退避仍然生效，耗电也会增加。
- 可分别设置屏幕调暗和自动熄灭时间，也可开启屏幕常亮并自行调整亮度。常亮时仍按相同时间调暗；变暗或熄屏后，第一次按键只恢复屏幕，不执行播放操作。
- 双击下一首进入沉浸模式，双击上一首查看设备状态；长按确定返回首页。状态页显示电池、内存、网络、亮度、运行时长及授权和同步信息，不估算剩余续航。
- 首页和沉浸模式的背景随专辑封面颜色缓慢变化。息屏后约每 60 秒同步，唤醒时立即安排同步，并遵守网络及限流退避。
- 支持常用中文、日文及部分其他文字；没有字形的字符在歌曲信息中直接跳过，不显示方框或额外空位。

## 第一步：获取和刷入固件

1. 下载本仓库；连接设备时请使用支持数据传输的 USB 线。
2. `firmware/soundring-app.bin` 是**应用镜像**，地址为 `0x20000`。只有在设备分区表与仓库 `partitions.csv` 一致时，才能使用它进行保留设置的应用更新。分区布局不确定时先核对，不要直接刷入。
3. `firmware/soundring-full.bin` 是**完整合并镜像**，用于从 `0x0` 进行全新安装或明确选择的完整刷新。完整刷入可能覆盖现有配置，请先保存需要的数据。不要将应用镜像刷到 `0x0`。
4. 安装 Python 和 esptool 后，在仓库目录运行下面命令。把 `COMX` 换成实际设备串口；完整刷新时按提示确认数据影响。

```powershell
python -m pip install esptool
python -m serial.tools.list_ports -v
# 已核对分区表、只更新应用：
python -m esptool --chip esp32c3 --port COMX write_flash 0x20000 firmware/soundring-app.bin
# 新设备或有意完整刷新（可能覆盖配置）：
python -m esptool --chip esp32c3 --port COMX write_flash 0x0 firmware/soundring-full.bin
```

`list_ports` 来自 pyserial；如果该命令提示模块不存在，运行 `python -m pip install pyserial`。写入成功后等待设备重启。固件校验值见 [`firmware/SHA256SUMS.txt`](firmware/SHA256SUMS.txt)。

## 第二步：配置 Wi-Fi 和 Spotify

1. 在 [Spotify 开发者后台](https://developer.spotify.com/dashboard)使用自己的账号和应用。填写或登记符合 [Spotify 回调地址规则](https://developer.spotify.com/documentation/web-api/concepts/redirect_uri)的本机 IPv4 回调地址，记下 Client ID 和完整回调地址。本仓库不提供任何个人 Client ID 或回调地址。
2. 此工具使用 PKCE，不需要 Client Secret。Spotify 开发模式的账号、Premium 和可读歌单等限制以 [官方迁移指南](https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide)为准。
3. Windows 可从 [最新发布页](https://github.com/walker42144/soundring-ai-passport/releases/latest)下载 `soundring-pairing-windows.exe` 直接运行。也可安装带 Tkinter 的 Python，使用源码运行：

```powershell
python -m pip install -r requirements-pairing.txt
python tools/soundring_pairing_gui.py
```

4. USB 连接设备，在设备进入 **设置 → 电脑配对**。在配对中心选择设备串口，输入设备显示的 8 位配对码。
5. 需要配网时勾选“配置 Wi-Fi”，输入自己的 **2.4 GHz** Wi-Fi 名称和密码。需要授权时勾选“授权 Spotify”，输入自己的 Client ID 和开发者后台登记的完整回调地址；两项可同时或分别配置。
6. 如需备用网络，点击“添加备用 Wi-Fi”，最多填写 3 组备用网络；原设备已保存的网络也占一个位置，设备总共最多保存 4 组。只添加备用网络时可将当前网络名称留空。
7. 点击“开始配对”，按浏览器提示登录并授权，等待工具提示配置完成。若只更新 Wi-Fi 或只更新授权，可取消另一项勾选。之后在设备“设置 → Wi-Fi”选择已保存网络，按确定连接。此版本为手动切换，暂不支持单独删除已保存的网络。

配对中心使用本机回调监听器接收授权结果；回调地址需有端口和路径，并与应用登记内容匹配。不要把密码、配对码或令牌发给别人。备选 PowerShell 7 工具：`pwsh -File tools/soundring-setup.ps1`，按照提示输入所需信息；`-SelfTest` 只运行本地加密自检。

## 操作

| 场景 | 按键 |
| --- | --- |
| 首页 | 上／下：上一首／下一首；确定：播放／暂停 |
| 首页 | 双击确定：收藏／取消收藏；长按上／下：调整音量 |
| 首页 | 双击下一首：沉浸模式；双击上一首：设备状态 |
| 沉浸模式／设备状态 | 长按确定返回首页；状态页按上／下翻页 |
| 首页 | 长按确定：打开菜单 |
| 列表 | 上／下选择；确定打开；长按确定返回；支持分页的页面可长按上／下翻页 |
| 设置 | “屏幕调暗”和“屏幕熄灭”分别选择时间；设置为“不自动”可关闭对应计时 |
| 设置 | “屏幕常亮”开启后不自动熄屏，但仍遵循调暗时间；“亮度”可手动调整 |
| 暗屏／息屏 | 第一次按任意功能键仅恢复亮度／点亮屏幕，再次按键才操作 |
| 设置 | “同步速度”按确定切换自动、省电、标准、快速；选择会保存 |
| 设置 | “Wi-Fi”查看已保存网络，选择后按确定连接；长按确定返回 |

## 从源码构建

安装并激活 **ESP-IDF 5.5.3**，在仓库根目录执行：

```text
idf.py set-target esp32c3
idf.py build
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

固件输出在 `build/`。更完整的构建和校验流程见 [`docs/development/engineering/build-and-test.zh_CN.md`](docs/development/engineering/build-and-test.zh_CN.md)。生成字库需要 Pillow；运行 `python tools/gen_sr_font.py --check` 可核对现有生成结果。

## 常见问题

- **已经播放但首页没有更新**：确认设备在首页且未息屏、Wi-Fi 已连通；可在设置里切换快速同步。网络、授权失效和 Spotify 限流也会影响更新。
- **歌单能看到但曲目打不开**：确认账号有访问权限。Spotify 开发模式对部分歌单内容有限制。
- **最近播放或队列为空**：检查 Spotify 授权范围、当前播放状态和网络；最近播放需要相应授权。列表上限为 13 项。
- **配对失败**：确认数据线和串口正确、设备仍在配对页、配对码有效、Wi-Fi 为 2.4 GHz，且 Client ID 与回调地址属于同一个应用。
- **部分字符未显示**：字库未覆盖所有 Unicode；不支持的字符会被跳过，Spotify 中原始名称不受影响。

设备端 Wi-Fi 凭据和 Spotify 令牌保存在未加密的 NVS 中。请保护设备和电脑访问权限，**不要公开完整 Flash 备份、配对信息或包含凭据的日志**。本项目非 Spotify 或 FoloToy 官方产品；Spotify 服务和接口限制可能变化。

项目使用 [MIT 许可证](LICENSE)，第三方代码与字体保留各自许可证。
