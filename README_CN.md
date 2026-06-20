# Glane Notes —— 基于 ESP32-S3 + 墨水屏的离线优先语音笔记设备

**🌐 语言 / Language**: [English](README.md) | **简体中文**

> 一台能装进口袋的「第二大脑」语音录音笔：**按下即录，松手即存**。
> 几秒钟捕捉稍纵即逝的灵感，全程离线可用——之后再按需上云转写成文字。

**Glane Notes** 是一套开源的 [ESP-IDF](https://github.com/espressif/esp-idf) 固件，
把 **微雪 Waveshare ESP32-S3-ePaper-1.54** 开发板变成一台极简、零干扰、带电子墨水屏的
语音备忘设备。灵感来自 "Pala Note" 项目，核心理念只有一个：*先捕捉，后处理*。

**按下按键** 即可把语音直接录到 SD 卡（WAV 格式）；**再按一次** 立即保存并响起提示音，
随后回到超低功耗深睡。当你主动通过 Wi-Fi 同步时，录音会用 **阿里云百炼 DashScope
`qwen3-asr-flash`** 转写成可搜索的文字，并写回到音频旁边。设备本机列表或内置的小型
Web 控制台都能浏览、回放、导出全部笔记。

<!-- 关键词：ESP32-S3 语音录音笔, 电子墨水屏笔记, e-ink 语音备忘, ESP-IDF 固件,
语音转文字, 语音识别 ASR, 阿里云百炼 DashScope qwen3-asr-flash, 微雪 Waveshare
ESP32-S3-ePaper-1.54, SD 卡录音, 离线语音笔记, 第二大脑, DIY 录音机, SSD1681 墨水屏,
ES8311 音频编解码. -->

---

## 📸 演示

<p align="center">
  <img src="docs/demo_sleeping.jpg" alt="Glane Notes 休眠界面与小狗吉祥物 Buddy" height="320">
</p>

<p align="center">
  <video src="https://github.com/qinwenshi/GlaneNotes/raw/main/docs/demo_activate.mp4" height="320" controls muted playsinline></video>
</p>

墨水屏的**休眠界面**会显示吉祥物 *Buddy*——一只犯困打盹的像素风小狗，头顶飘着
`zZ`。按下按键即可唤醒设备并开始录音。上方短片展示了唤醒/录音动画（源文件：
[`docs/demo_activate.mp4`](docs/demo_activate.mp4)）。

---

## ✨ Glane Notes 的与众不同之处

- **🔌 离线优先（Capture-first）。** 录音、本机笔记列表、扬声器回放**完全本地化**。
  Wi-Fi 永远不会阻塞开机——在路上、彻底无网络时，设备照常完整可用。网络只是「锦上
  添花」，而非前置条件。
- **☁️ 延迟、按需转写。** 音频默认只存在 SD 卡，**仅在你主动同步时才上传**。转写采用
  **基于文件的方式（每条录音一次 HTTPS POST）** 调用阿里云百炼 DashScope
  `qwen3-asr-flash`——不用 WebSocket、不做实时流式、也不依赖 OpenAI Whisper。每条笔记
  最终都同时拥有 **「可听的录音」与「可搜的文字」**。
- **📶 零门槛配网。** 全新设备开机会自动开启 `GlaneNotes-Setup` 热点。手机连上后访问
  `192.168.4.1`，在同一个页面填好 Wi-Fi 凭据 **和** DashScope API Key——彻底解决
  「要联网才能配网」的死循环。
- **🖥️ 墨水屏少闪烁。** 界面切换一律走快速**局部刷新**；只有每 60 次更新才做一次
  「单次闪屏」的全局刷新来消除残影。在墨水屏上翻页几乎无感。
- **🔋 真正的随身续航。** GPIO 电源锁存、空闲后自动深睡、屏上**电量指示**（经片上校准的
  ADC 读取），配合 500 mAh 小电池即可日常携带。
- **🔁 可复刻、易魔改。** 纯原生 ESP-IDF、整洁的模块化 C++、一条命令完成构建/烧录的
  脚本，并提交了 `dependencies.lock`——欢迎 fork 出属于你自己的版本。

## 🎬 工作流程

1. **按下 BOOT** → 开始录音（上升提示音），开口说出你的笔记。
2. **再按一次 BOOT** → 以 16 kHz 单声道 WAV 保存到 `/sdcard/notes/`（下降提示音），
   随后回到低功耗主界面。
3. **按 PWR** → 打开本机笔记列表；**BOOT** 打开某条笔记，**再按 BOOT** 通过扬声器回放。
4. **长按 BOOT** → 通过 Wi-Fi 同步：每条未转写的笔记都会上传到 DashScope，并把 `.txt`
   文字稿写回音频旁边。
5. 或打开 **Web 控制台**（设备的 IP 地址），用手机或电脑浏览、回放、下载笔记与文字稿。

## 🧩 硬件

目标开发板：**微雪 Waveshare ESP32-S3-ePaper-1.54**（ESP32-S3-PICO-1，双核
240 MHz，8 MB Flash，8 MB PSRAM）。

| 功能 | 说明 |
|---|---|
| 主控 | ESP32-S3（Wi-Fi + 低功耗蓝牙） |
| 显示 | 1.54" SSD1681 墨水屏，200×200，支持局部刷新 |
| 音频 | ES8311 编解码器，I2S 麦克风录音 + 扬声器回放 |
| 存储 | microSD（SDMMC），笔记与文字稿全部存卡 |
| 电源 | 锂电池（如 500 mAh），GPIO 锁存、深睡、电池 ADC |
| 输入 | BOOT + PWR 两个按键 |

完整引脚定义见 [`firmware/main/glane_config.h`](firmware/main/glane_config.h)。

## 🚀 构建、烧录与监视

需要 **ESP-IDF v5.5+** 与 ESP32-S3 目标芯片（开发与验证基于 **ESP-IDF v5.5.2**）。
[`firmware/scripts/`](firmware/scripts/) 下的脚本会自动加载 ESP-IDF 环境并自动探测串口。

```bash
cd firmware

# 构建
./scripts/build.sh              # 首次会设置目标芯片，然后构建
./scripts/build.sh clean        # idf.py fullclean 后再构建
./scripts/build.sh fullclean    # 删除 build/ 后再构建

# 烧录
./scripts/flash.sh              # 构建 + 烧录（自动探测端口）
./scripts/flash.sh --monitor    # 烧录后打开串口监视器
./scripts/flash.sh -p /dev/cu.usbmodemXXXX

# 监视器（Ctrl-] 退出）
./scripts/monitor.sh
```

可用环境变量覆盖 ESP-IDF 路径或串口：

```bash
IDF_DIR=/path/to/esp-idf ./scripts/build.sh
PORT=/dev/cu.usbmodem1101 ./scripts/flash.sh
```

或直接用原生 `idf.py`：

```bash
source /path/to/esp-idf/export.sh
cd firmware
idf.py set-target esp32s3        # 仅首次
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

设备以 USB CDC 串口（USB-Serial-JTAG）枚举。若深睡后串口消失，重新执行
`ls /dev/cu.usbmodem*` 并把新端口传给 `flash.sh -p`。

## ⚙️ 首次配置

固件不内置任何 Wi-Fi 或 API Key，全部存于 NVS，由内置 Web 控制台设置。首次开机
（尚未保存 Wi-Fi）时设备会自动开启 **配网热点**：

1. 开机，屏幕显示 **WIFI SETUP** 与 SSID、网址。
2. 用手机连接 **`GlaneNotes-Setup`**（开放网络，无需密码）。
3. 浏览器打开 **`http://192.168.4.1`**，进入设置页。
4. 填入 **Wi-Fi 名称 + 密码** 与 **阿里云 DashScope API Key**，点击保存。
5. 设备显示 **SAVED / Restarting** 并重启连入你的网络。

之后想重新配网：在主界面 **长按 BOOT** 触发同步；若未配置 Wi-Fi 会重新打开配网热点。
按任意键可取消。

你需要一个 [阿里云百炼](https://bailian.console.aliyun.com/) 的 API Key，并开通
`qwen3-asr-flash` 语音识别模型的访问权限。

> **离线优先：** Wi-Fi 永不阻塞开机。已保存凭据时设备在后台连接；即使连接失败，
> 录音、列表、回放仍完全离线可用，同步只会提示 *Working offline*。

## 📖 操作指南

两个按键（**BOOT** 与 **PWR**）驱动一个小型状态机；每次按键的功能取决于当前界面。
**长按** 指按住约 0.8 秒，更短即为 **短按**。

### 主界面（空闲）

| 操作 | 结果 |
|---|---|
| **短按 BOOT** | 开始录音（上升提示音）。再按一次停止并保存。 |
| **长按 BOOT** | 通过 Wi-Fi 同步（转写所有未转写的笔记） |
| **短按 PWR** | 打开本机笔记列表 |
| **长按 PWR** | 立即进入深睡 |
| 空闲约 3 分钟 | 自动深睡；按 BOOT 唤醒 |

主界面左上角显示 **电量指示**（图标 + 百分比）、笔记数量与 Wi-Fi 状态（连接后显示设备 IP）。

### 录音中

| 操作 | 结果 |
|---|---|
| **按 BOOT 或 PWR** | 停止并保存录音（下降提示音） |

录音为 16 kHz 单声道 WAV，单文件硬上限 **10 分钟**。录音计时通过快速局部刷新实时显示。
停止为非阻塞——SD 写入在后台完成，界面立即返回主界面。

### 笔记列表

| 操作 | 结果 |
|---|---|
| **短按 PWR** | 选择光标下移（到底回到顶部） |
| **短按 BOOT** | 打开所选笔记的详情 |
| **长按 BOOT** | 返回主界面 |
| **长按 PWR** | 深睡 |

笔记按最新在前排列。每行显示编号，以及——在通过 Wi-Fi 校时（SNTP）之后——录音的
日期时间（`MM-DD HH:MM`）；首次同步前显示为 `--:--`。

### 笔记详情

| 操作 | 结果 |
|---|---|
| **短按 BOOT** | 通过扬声器回放录音 |
| **短按 PWR** | 返回笔记列表 |
| **长按 BOOT** | 返回主界面 |

### 回放中

| 操作 | 结果 |
|---|---|
| **按 BOOT 或 PWR** | 停止回放 |
| （回放结束） | 自动返回详情界面 |

### 🖥️ Web 控制台

连入 Wi-Fi 后，设备在其 IP 地址上提供控制台：

| 路由 | 用途 |
|---|---|
| `/` | 笔记列表（含大小与转写状态） |
| `/note?id=...` | 查看文字稿 |
| `/dl?id=...` | 下载 WAV |
| `/del?id=...` | 删除笔记（连同文字稿） |
| `/settings` | 设置 Wi-Fi 凭据与 DashScope API Key |
| `/sync` | 请求同步（立即返回，空闲时执行） |

墨水屏仅显示英文状态；中文（或任意语言）文字稿可在 Web 控制台与 SD 卡上的 `.txt`
文件中查看。

### 🗃️ SD 卡目录结构

```
/sdcard/notes/
├── note-0000000001.wav            # 录音（16 kHz 单声道）
├── note-0000000001.txt            # 文字稿（同步后写入）
└── note-0000000001.wav.diag.txt   # 采集诊断信息
```

要在电脑上读取录音或诊断，请从设备取出 SD 卡并挂载到电脑。卡取出期间设备会显示
**SD mount fail**。

## 🛠️ 故障排查

| 现象 | 可能原因 / 处理 |
|---|---|
| 开机 **SD mount fail** | 卡未插好（或还在电脑里）。重新插入并重启。 |
| **回放无声** | 检查串口日志是否有 `ES8311 init OK`；查看该笔记的 `.diag.txt`（各候选 AC RMS 接近 0 即无麦克风数据）。 |
| **录音变快 / 变声** | 查看该笔记的 `.diag.txt`：`i2s_read_rate` 应约 48000，`ratio` 约 1.0。 |
| **时长比计时短** | 检查 `.diag.txt` 里 `ring_drops`（应为 0）与 `i2s_read_rate`（约 48000）。 |
| 同步提示 **Working offline** | Wi-Fi 未连接；在 `/settings` 检查凭据。录音/列表/回放仍可用。 |
| **同步后文字稿为空** | DashScope 鉴权/额度问题，或文件超过 3 MB 内联上限（约 90 秒）。检查 API Key 与录音时长。 |
| **串口消失** | 设备深睡并重新枚举 USB。重新执行 `ls /dev/cu.usbmodem*`。 |

更深入的技术文档（硬件引脚表、DashScope 请求格式、采集/DSP 流水线、诊断字段、完整模块
图）见 [`firmware/README.md`](firmware/README.md)（英文）。

## 🗂️ 项目结构

```
firmware/
├── main/            # 应用层：录音、回放、UI、同步、Web、Wi-Fi、电池……
├── components/      # epaper_bsp（SSD1681 墨水屏驱动）
├── scripts/         # 构建 / 烧录 / 监视 脚本
└── README.md        # 详细固件文档（英文）
```

## 🛣️ 后续规划

- 录音后的标签菜单（项目 / 待办 / 想法）
- 每天自动后台同步一次
- Web 控制台中的标签过滤与全文搜索
- 借助已接入的 AI 连接，在设备端生成笔记摘要 / 标题

## 📄 许可与致谢

基于 [ESP-IDF](https://github.com/espressif/esp-idf) 构建。墨水屏、编解码器与按键驱动
改编自同款开发板的 `ESP32-S3-EPaper-Player` 参考项目。概念灵感来自 "Pala Note" 口袋
录音笔。

欢迎贡献代码、fork 以及提出新功能建议。
