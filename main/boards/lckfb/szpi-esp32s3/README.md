# 立创·实战派 ESP32-S3（LCKFB SZPI）

## 构建

```sh
# 板载 ES7210/ES8311 音频（默认）
python3 scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev

# USB UAC 1.0 耳机音频（服务端 AEC）
python3 scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev-uac
```

## UAC 耳机

`lichuang-dev-uac` 使用 `Ab13xAudioCodec`，按现有 `kayzh-ml307-headphone` 的 UAC 1.0
实现方式工作，支持 AB13X 一类的 USB 声卡/耳机转接头。

> **硬件注意**：原厂板子的 Type-C 接口接在 CH334F USB HUB 的上行口（DPU/DMU），
> ESP32-S3 的 USB-OTG 接在 HUB 的下行口 1（DP1/DM1）。Type-C 的 CC1/CC2 还有
> 5.1k 下拉，是标准 USB 设备口。也就是说，ESP32-S3 在原厂接线下是 USB 设备而不是
> USB 主机，软件无法让它在原厂 Type-C 上枚举 UAC 耳机。默认构建不受影响，仍使用
> 板载 ES7210/ES8311。

### 硬件改法（可选，需动板子）

原理图第 2 页 U5（CH334F）：

- `DP1`（pin 12）→ `IO20 USB D+` → ESP32-S3 U1 pin 14
- `DM1`（pin 11）→ `IO19 USB D-` → ESP32-S3 U1 pin 13

最直接的做法是切断这两根从 U5 到 ESP32-S3 的走线，然后增加一个 USB-A 母座：

| USB-A 母座 | 接到 |
| --- | --- |
| VBUS | 板子的 VBUS/5V（建议串 500mA 自恢复保险丝） |
| D- | ESP32-S3 `IO19 USB D-` |
| D+ | ESP32-S3 `IO20 USB D+` |
| GND | GND |

这样 ESP32-S3 就变成一个真正的 USB 主机，UAC 耳机可以直接枚举；Type-C、CH334F
和 CH340K 仍保留，串口烧录/日志继续走 CH340K。代价是 ESP32-S3 原生的
USB-Serial/JTAG 不再接到 HUB（`/dev/cu.usbmodem*` 会消失），但这块板子平时用
CH340K 的 `/dev/cu.wchusbserial*`，不影响烧录。

如果想同时保留“PC USB 设备”和“UAC 主机”两种模式，可以在 ESP32-S3 的 D+/D-、
HUB 的 DP1/DM1、新增 USB-A 母座之间加一个 USB 2.0 二选一模拟开关（如
TS3USB221A/FSUSB42），用机械开关切换。注意 CH334F 剩余的 DP2/DP3 仍然是 HUB
下行口，接耳机只会让 PC 看到耳机，ESP32-S3 不能用它们做主机。

## SD 卡语音笔记

插入 FAT32 格式的 Micro SD 卡后重启设备。只有 SD 卡挂载成功时，才会注册下面两个
设备端 MCP 工具：

- `self.inspiration.capture`：灵感胶囊
- `self.insight.capture`：洞察抽屉

工具参数：

- `content`（必填）：要保存的内容，建议保留用户原话和必要上下文。
- `title`（可选）：标题；不填时取内容第一行。
- `tags`（可选）：逗号分隔的标签。

SD 卡目录结构：

```text
/sdcard/xiaozhi-notes/
├── README.md
├── inspiration/
│   └── 2026-10-21/
│       └── 20261021-143012-123.md
└── insight/
    └── 2026-10-21/
        └── 20261021-143512-456.md
```

每个 Markdown 文件包含 YAML front matter（类型、标题、创建时间、标签）和正文，
按“类型 / 日期”两层目录即可快速浏览。时间来自设备系统时间；如果设备尚未同步时间，
文件会落到错误日期，日志中会有提示。

## 验证状态

- SD 卡挂载、目录创建、中文 Markdown 写入/读回已在真实板子上通过自检。
- 两个语音工具已在启动日志中确认注册；真实对话触发和文件落盘仍待联网验证。
- 原厂 LCKFB 板子的 UAC 主机枚举受上述 HUB 接线限制，需要完成硬件改法后再验证。
