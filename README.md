# M5Stack Core2 v1.1 · Friday 语音终端

[![build](https://github.com/integemjack/friday.m5stack-core2-v1.1/actions/workflows/build.yml/badge.svg)](https://github.com/integemjack/friday.m5stack-core2-v1.1/actions/workflows/build.yml)

把 M5Stack Core2 v1.1 变成 Friday 的实体语音对讲机：**按住说话 → Friday 听写、思考、用它自己的音色回答（TTS）**，全程与 Friday Work 桌面客户端走同一条通道（SChat WebRTC 工作通道），无需在电脑上装任何东西。

## 架构

```
M5Stack Core2 v1.1 (esp32, 16MB flash / 8MB PSRAM)
├─ WiFi STA ── HTTPS(自签,跳过校验) ── chatserver 47.100.98.3:2012
│    ├─ POST /login-sessions          扫码登录建会话（SCSH:1:login:<token> 二维码）
│    ├─ POST /login-sessions/poll     2s 轮询，approved 拿凭据
│    ├─ POST /auth/refresh            token 轮转
│    └─ GET  /friends                 找 Friday 机器人（bot 好友）
├─ WSS /ws（schat-ws/v1 信封 {e,d,a,t}）
│    └─ p2p_request{callType:"work"} → p2p_response → offer/answer/ice_candidate
├─ WebRTC DataChannel「friday-work」（esp_peer：ICE+DTLS+SCTP，本机作 caller）
│    ├─ WorkFramer 分片（整帧 → B{"sid":n,"seq":i,"count":N,"data":…}）
│    └─ JSON-RPC：hello{protocol:1,client:"m5core2"} → voice.start → voice.feed(PCM16 b64)
│                                                    ↑ 事件：voice.state/audio(PCM16 b64)
├─ 麦克风：PDM (I2S0, M5Unified Mic) 16kHz 单声道，100ms 帧 → base64 → voice.feed
├─ 扬声器：I2S1 16kHz，voice.audio 帧 → 环形缓冲 → 48k 上采样（队列满丢帧，绝不积压延迟）
│    半双工：Speaker 在播时暂停录音（Core2 麦克/喇叭共享 GPIO0，无回声消除）
│    ★ 节能：功放（ALDO3）收到语音才开，播完静默 700ms 自动关
└─ 屏幕（v0.3）：状态栏（阶段·电量·RSSI）/ Friday 动态图标（液体彩虹圆环，随语音状态和真实电平变）
     / 字幕气泡·草稿·发送目标 / 屏下机身三键提示（不是按钮，只是印着告诉你 A/B/C 干什么）
     / 右滑音量页 / 左滑会话列表页（选发送目标）/ 点图标=丢草稿或打断
```

对应服务端实现：Friday `qt/src/core/remote/`（RemoteWorkSession=被叫、RemoteVoice=语音会话、
WorkFramer=分片），协议文档 `qt/docs/REMOTE.md` §2 WebRTC 工作通道、§6 语音（voice.*）。
与 Friday Work（`works/friday-desktop-work`）的 WorkSignaling.ts / Login.ts / SchatClient.ts 逐字段对齐。

## 首次使用

1. 烧录后屏幕出现 SoftAP 配网页提示：手机连 `Friday-Voice`（密码 `12345678`），
   打开 `http://192.168.4.1`，填 WiFi 和（可选）chatserver 地址，保存自动重启。
2. 屏幕出现二维码 → 用 Secret-Chat App 扫码并在手机上批准。
3. 自动拨号 Friday 机器人 → 屏幕中央出现彩色圆环动画 → 按下面对应操作。

## 操作（v0.3）

用机身下面的三个原生按键（横屏下沿从左到右）：

| 键 | 作用 |
|----|------|
| 左 A | **按住说话，松开结束**（草稿先留在服务端，屏幕变青色字幕；侧页里照常能用） |
| 中 B | **发送**：发给当前目标（默认助手，语音回答）；发给会话则用 runner.submit 投递文字 |
| 右 C | **弃用**，按了不响应（机身键带里画成灰的） |

原来的「取消」挪到**点屏幕中间的 Friday 图标**：有草稿=丢弃（voice.stop），没草稿=打断播报/思考。

- **右滑**（主屏）：音量页，拖动音量条或点 ＋/－ 调，自动存到 NVS。
- **左滑**（主屏）：会话列表页（activity.subscribe），顶部固定「助手」且默认选中，
  点某个会话即成为发送目标，返回后按 B 把草稿发过去；列表可上下拖动，20 秒不动自动回主屏。
- 助手答完一轮会自动关语音会话；再按 A 会自动重新 voice.start，不用管。
- 点顶部状态栏 = 检查/安装 OTA 更新。
- 侧页返回：往回滑、点左上「返回」，或放着不管（音量页 12 秒、会话页 20 秒自动回主屏）。
- 节能：平时功放是关的，**收到语音才开喇叭**，播完静默 700ms 自动关；主屏 1 分钟不碰亮度降到 24，
  按键或触摸会亮回来。

> 说明：松开 A 后草稿存在服务端（voice 的 pending），只有按 B 才交给助手/会话，点中间图标丢弃；
> 连按两次说话会接着同一段草稿追加，和桌面端按住说话的语义一致。

凭据/WiFi/选择过的 bot 都存 NVS，重启免扫码。

凭据生命周期（踩过的坑，别再改回去）：
- 扫码批准时服务端给的 token 是 **7 天** 的 access JWT；`refresh` 换回来的是 **15 分钟** 的短 token。
  所以**不要每次连接前都 refresh**，否则等于把自己降级成 15 分钟寿命。
- 只在离过期不足 2 分钟时 refresh；**refresh 失败绝不清凭据**（旧 token 还能用，Friday Work 同理）。
- 只有 WS hello 被判 `auth_expired`、且换一份凭据再试也失败时，才清凭据回扫码页。
- token 死线按 JWT 的 `exp-iat` 算，存在内存里（开机相对毫秒）；别跨重启比较，重启后一律先拿现有 token 直接连。
- WS 每 20 秒 ping（NAT 会回收空闲长连接），链路断了（WS 或数据通道）整条拆掉带退避重连，连上后退避归零。

## 构建（本机已有环境）

```powershell
cmd /c "cd /d C:\Users\ultra\Desktop\works\esp-idf-v5.5 && call export.bat && cd /d C:\Users\ultra\Desktop\works\m5stack-core2-v1.1 && idf.py build"
```

烧录（设备插 USB 后）：同上把 `build` 换成 `-p COMx flash monitor`。

## 界面自检（不用肉眼盯屏幕）

改完界面后把绘制目标换成 PSRAM 画布，让设备把每个页面渲染成位图吐给电脑看版式：

```powershell
# 1. 调试构建（环境变量 UI_DUMP=1 时 main/CMakeLists.txt 会给 ui.cpp 加 -DUI_DUMP）
cmd /c "cd /d C:\Users\ultra\Desktop\works\esp-idf-v5.5 && call export.bat >nul && cd /d C:\Users\ultra\Desktop\works\m5stack-core2-v1.1 && set UI_DUMP=1&& idf.py -B build-dump build"
# 2. 烧进去（只烧 app 分区 0x100000），设备连上 NVS 里的 WiFi 后在 :8098 起服务
# 3. 拉回电脑还原成 PNG，再看每页的元素占位
node tools/ui-dump-fetch.mjs          # 不给 IP 会扫 192.168.20.0/24；也可 $env:UI_DUMP_IP=...
node tools/ui-dump-decode.mjs        # → tools/ui-dump/pages/*.png
node tools/ui-dump-inspect.mjs       # → tools/ui-dump/inspect.txt（每块墨迹的 y/x 占位，查重叠/贴边/触边）
```

调试构建不跑状态机、不拨号，只渲染 ui.cpp 里 `ui_dump_all()` 列的那几页（主屏各状态、音量、会话、
扫码、配网、选 bot），数据是 4 位游程 + 4 位 RGB565 的 RLE 文本。正常构建里这部分全被 `#ifdef` 编译掉。

## CI 构建与发布（GitHub Actions）

push 到 `main` 自动构建（`espressif/idf:v5.5.0`），产物上传 artifact；
推 `vX.Y.Z` tag 时校验 tag 与 `main/app.h` 的 `APP_VERSION` 一致后自动发 Release，附：

- `m5_friday_voice.bin` — app 分区镜像（OTA 用 / 单烧到 `0x100000`）
- `m5stack-core2-factory-16MB.bin` — 16MB 全量出厂镜像（空白机 `esptool write_flash 0x0`）
- `bootloader.bin` / `partition-table.bin` — 分开烧：`0x0` / `0x8000`

## OTA 自更新

分区表（v0.2.0 起）：`ota_0`/`ota_1` 各 3MB 双分区 + `otadata`，从 factory 布局升级需重烧分区表。

- 自动：开机 30s 后静默查一次，之后每 6h 一次；发现新版只提示（状态栏），不自动装。
- 手动：**点屏幕顶部状态栏** → 检查 → 有新版则下载写入备用分区 → 自动重启切换（启动镜像校验不过自动回滚另一分区）。
- 来源：GitHub Releases `latest` 的 `m5_friday_voice.bin`，HTTPS 真证书校验（区别于 chatserver 的自签跳过）。
- 设备需能直连 `api.github.com` / `release-assets.githubusercontent.com`；连不上时手动检查会提示失败，不影响其他功能。

版本号单一来源：`main/app.h` 的 `APP_VERSION`（构建时注入 esp_app_desc，Release tag 必须与它一致）。

## 组件来源（components.espressif.com 在本机不可达，全部本地化）

`components/` 下（拷贝自 `C:\Users\ultra\Desktop\works\` 的 *-ref 克隆，可联网环境可换回注册表，见 main/idf_component.yml 注释）：

| 组件 | 来源 | 备注 |
|---|---|---|
| esp_peer 1.5.7 | esp-webrtc-solution | WebRTC 栈；CMakeLists 加了 `PRIV_REQUIRES esp_libsrtp` |
| esp_libsrtp | esp-adf-libs | **必须这个版本**（esp_peer 用它的 5 参数 srtp API；esp-protocols 开源版是 3 参数就地 API，不兼容）|
| esp_websocket_client 1.8.0 | esp-protocols | IDF 5.5 起移出核心；API 已改 `esp_websocket_register_events` |
| M5Unified 0.2.27 | m5stack | 支持 Core2 v1.1（AXP2101 功放 ALDO3、屏幕初始化）|
| M5GFX 0.2.34 | m5stack | 内嵌 efontCN 中文字体 + qrcode 绘制 |

## 已知边界

- chatserver 是自签证书：当前 `ESP_TLS_INSECURE` 跳过校验（与 Friday Work 首版一致的取舍）。
  TODO v2：TOFU（首次锁定证书指纹）。
- 外放无回声消除：TTS 播放期间麦克风静默（协议 voice.playing 语义），纯按键半双工。
  建议戴 3.5mm 耳机可解锁「边放边听」（Mic 那时不会拾到外放）。
- 一个 Friday 同时只允许一路语音（服务端语义）：桌面端语音模式和它互斥，后开的接管。
- `sdkconfig.defaults` 关了 WiFi 驱动 IRAM 优化（省 5KB IRAM，ESP32 IRAM 紧张）。
- 机身键 C 已弃用（按了不响应）；它的两个动作分别挤到「点中间图标」和「反滑/自动返回」上。
  侧页里 A/B 照常能用（说话/发送），因为它们是全局动作。

## 文件

```
main/
├─ app_main.cpp   状态机（BOOT→PORTAL→WIFI→QR→WS→DIAL→PEER→HELLO→VOICE→LIVE）+ 断线退避重拨
├─ schat.cpp/.h   chatserver HTTP（扫码/轮询/refresh/friends）+ WS 客户端（信封、ack、事件分发）
├─ work.cpp/.h    p2p 信令状态机 + esp_peer 封装 + WorkFramer + JSON-RPC + voice.* 事件处理
├─ audio_io.cpp   Mic/Speaker 半双工管道（乒乓采样、播放队列丢帧策略）
├─ ui.cpp/.h      屏幕（主屏动态图标/字幕/目标、音量页、会话页、扫码页、配网页）+ 机身键与触摸手势
├─ portal.cpp/.h  SoftAP 配网（HTTP 表单）
├─ ota.cpp/.h     固件自更新（GitHub Releases，双分区切换）
├─ store.cpp/.h   NVS 持久化
└─ util.cpp/.h    base64 / 随机 hex / 时间
```
