# Glane Notes —— 基于 ESP32-S3 + 墨水屏的离线优先语音笔记设备

**🌐 语言 / Language**: [English](README.md) | **简体中文**

> 一台能装进口袋的「第二大脑」语音录音笔：**按下即录，松手即存**。
> 几秒钟捕捉稍纵即逝的灵感，全程离线可用——之后再按需上云转写成文字。

**Glane Notes** 是一套开源的 [ESP-IDF](https://github.com/espressif/esp-idf) 固件，
把 **微雪 Waveshare ESP32-S3-ePaper-1.54** 开发板变成一台极简、零干扰、带电子墨水屏的
语音备忘设备。灵感来自 "Pala Note" 项目，核心理念只有一个：*先捕捉，后处理*。

长按按键即可把语音直接录到 SD 卡（WAV 格式）；松手立即保存并响起提示音，随后回到
超低功耗深睡。当你主动通过 Wi-Fi 同步时，录音会用 **阿里云百炼 DashScope
`qwen3-asr-flash`** 转写成可搜索的文字，并写回到音频旁边。设备本机列表或内置的小型
Web 控制台都能浏览、回放、导出全部笔记。

<!-- 关键词：ESP32-S3 语音录音笔, 电子墨水屏笔记, e-ink 语音备忘, ESP-IDF 固件,
语音转文字, 语音识别 ASR, 阿里云百炼 DashScope qwen3-asr-flash, 微雪 Waveshare
ESP32-S3-ePaper-1.54, SD 卡录音, 离线语音笔记, 第二大脑, DIY 录音机, SSD1681 墨水屏,
ES8311 音频编解码. -->

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

1. **长按 BOOT** → 开始录音（上升提示音），开口说出你的笔记。
2. **松手 / 再按一次** → 以 16 kHz 单声道 WAV 保存到 `/sdcard/notes/`（下降提示音），
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

## 🚀 构建与烧录

需要 **ESP-IDF v5.5+**。脚本会自动加载 ESP-IDF 环境并自动探测串口：

```bash
cd firmware
./scripts/build.sh        # 首次会设置目标芯片，然后构建
./scripts/flash.sh        # 通过 USB 烧录（自动探测端口）
./scripts/monitor.sh      # 串口监视器
```

完整的构建、烧录、配置、Web 控制台与架构文档见
[`firmware/README.md`](firmware/README.md)（英文）。

## ⚙️ 首次配置

固件不内置任何 Wi-Fi 或 API Key。首次开机时设备会开启 `GlaneNotes-Setup` 热点：

1. 用手机连接 **`GlaneNotes-Setup`**（开放网络，无需密码）。
2. 浏览器打开 **`http://192.168.4.1`**。
3. 填入你的 **Wi-Fi 名称 + 密码** 与 **阿里云 DashScope API Key**，点击保存。
4. 设备重启并连入你的网络，即可开始同步。

你需要一个 [阿里云百炼](https://bailian.console.aliyun.com/) 的 API Key，并开通
`qwen3-asr-flash` 语音识别模型的访问权限。

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
