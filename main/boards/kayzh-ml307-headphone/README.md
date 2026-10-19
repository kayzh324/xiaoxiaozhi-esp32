# kayzh-ml307-headphone

音频走 USB-OTG 上的 UAC 1.0 耳机（AB13X 适配器），驱动实现在
`main/audio/codecs/ab13x_audio_codec.cc`。设备以两个 logic device 出现：
TX（喇叭，OUT 等时端点）+ RX（麦克风，IN 等时端点）。

## DWC FIFO 配置

ESP32-S3 的 USB-OTG 数据 FIFO 预算来自只读寄存器 `GHWCFG3.dfifodepth`：

    OTG_DFIFO_DEPTH (256) - EP_LOC_CNT (56) = 200 行   （1 行 = 4 字节 = 800 字节）

`usb_dwc_hal_fifo_config_is_valid()` 在**每次端口复位时**校验
`rx + nptx + ptx <= fifo_size`。超预算会中止复位：

    E HCD DWC: Invalid FIFO config
    E HUB: Root port reset failed

端口复位失败后设备永远不会枚举，因此不会有 TX/RX connect 事件，
喇叭和麦克风同时失效。

AB13X 的等时端点 MPS：IN（麦克风）208 字节，OUT（喇叭）384 字节。
两个官方 bias 都覆盖不了：

| bias | RX | PTX | 问题 |
| --- | --- | --- | --- |
| Balanced（默认） | Total−96 = 104 行 (416 B) | 32 行 (128 B) | 放不下 384 B 的喇叭 MPS |
| Bias IN | Total−48 = 152 行 (608 B) | 32 行 (128 B) | 同上 |
| Periodic OUT | 34 行 (136 B) | Total−50 = 150 行 (600 B) | 放不下 208 B 的麦克风 MPS |

所以必须在 `fifo_settings_custom` 里手工切分，且合计不超过 200 行：

    rx   = 56  行 (224 B)   // 麦克风 MPS 208 + 2 行状态信息
    ptx  = 104 行 (416 B)   // 喇叭 MPS 384
    nptx = 32  行 (128 B)   // 控制/批量 OUT
    合计 = 192 / 200

**注意**：设置 `fifo_settings_custom` 后 Kconfig 的 `USB_HOST_HW_BUFFER_BIAS_*`
会被完全忽略——`_is_fifo_config_by_bias()` 只在三个字段全为 0 时才走 bias 分支。
不要再试图通过 menuconfig 或 `config.json` 的 `sdkconfig_append` 调 bias 来解决
FIFO 问题，那只对清空自定义配置的情况有效。

## 麦克风采集引擎依赖真正的断电

现象：喇叭永远正常，但麦克风**时好时坏**——同一份固件换个启动就可能完全收不到音
（`rx=0B mic_events=0 mic_peak=0`，而句柄是 `mic=up`）。

判据是 `read_calls` 的节奏：`read_calls / 运行秒数 ≈ 101 ms`，也就是每次 `Read()` 都耗满
100 ms 超时。如果接口不是 ACTIVE，驱动会立刻返回 `ESP_ERR_INVALID_STATE`，调用会瞬间返回、
`read_calls` 会远高于这个值。所以流是活的、URB 也提交了，只是**设备一个入站包都不发**。

已确认的成因：**只重新烧录（ESP32 重启）不会给耳机断电**，只是重枚举 USB，AB13X 的采集
引擎因此没有被重新武装；而 OUT 方向由主机驱动，所以喇叭不受影响。实测**不换固件、只把板子
重新上电**即可恢复，且连续多轮收音正常。

对策：`LogStatus()` 里的采集看门狗——句柄在、但 `mic_events` 连续 10 秒不增长时，调用
`ReArmMicrophone()` 执行 `uac_host_device_suspend()`（SET_INTERFACE → alt 0）+
`resume()`（→ alt N 并重新提交 IN URB），也就是驱动里唯一能制造真实 0→N 切换的路径。
日志里 `mic_rearms=` 是尝试次数，驱动会额外打 `Suspend Interface N-0` / `Resume Interface N-M`。

注意：断电重启能修好，只证明了"设备需要一次真正的复位"，**尚未证明较轻的 alt 0→N 切换就
足够**——所以看门狗是反复重试，而不是只武装一次。若 `mic_rearms` 一直增长却始终无数据，
请改查物理原因（耳机线控上的麦克风静音开关会让部分适配器直接停止发包）。

## 麦克风增益与信噪比

实测（去掉软件增益后的原始电平）：

| 距离 | `mic_peak` | 折合 | `mic_avg` |
| --- | --- | --- | --- |
| 正常距离 | 1145 | −29 dBFS | 171 |
| 贴近嘴 | 4199 | −18 dBFS | 214 |

`mic_avg`（|样本| 均值）两档几乎不变，说明**本底噪声大约在 −44 dBFS**：正常距离时语音只比
噪声高约 15 dB，贴嘴时约 26 dB——这正是"贴嘴稳、远处崩"的原因。一个健康录音的语音峰值应在
−6～−3 dBFS，所以信号低了约 20 dB。

**模拟增益已经拉满，这是关键事实。** 实测开机日志：

    Microphone capture gain after set_volume(100): 8191 (1/256 dB)   → 约 +32 dB
    Device accepted +30 dB and now reports 7680 (1/256 dB)

`8191/256 ≈ +32 dB`，也就是 `set_volume(100)` 已经把设备增益推到范围顶。上面那张表里的
`1145`/`4199` 就是在**最大模拟增益下**测到的——**ADC 之前的增益已经没有余量了**。

所以增益策略是：**先把设备增益拉满（`set_volume(100)`，不要写死某个 dB），再按
`MIC_TOTAL_GAIN_DB = 52`（模拟 + 软件总量）用软件补足**，实测补上约 +20 dB：

- 正常距离 `1145 × 10.0 ≈ 11,450`（约 −9 dBFS）

这副耳机的麦克风挂在喉结附近、离嘴有距离，不存在贴嘴爆音的场景，所以把软件增益用满。
即便如此，`Read()` 末尾仍会经过一个峰值限幅器（约 30000 / 32767，瞬时 attack、约 100 ms
release），保证咳嗽、大声说话等瞬态不会顶到 16-bit 满幅。软件增益**不改变 SNR**（信号噪声
同比例放大），它的作用只是让 ASR/Opus 在小信号上不要白白损失有效位数——信号只有 −29 dBFS
时，两者都吃亏。

> **坑：不要用固定 dB 去请求设备增益。** 设备的顶在 +32 dB，一旦写 `set_volume_db(12*256)`
> 会把它**降到 +12 dB**，等于整体变安静 20 dB。用百分比 `set_volume(100)` 才是取范围顶。

设备实际生效的增益写在**每一条心跳日志**的 `mic_hw_gain=`（单位 1/256 dB），预期 `8191`。

**软件增益也已经用满，剩下的杠杆是降噪或换麦克风。** 模拟 + 软件合计 52 dB 后，正常距离
的峰值已经到 −9 dBFS 左右；再往上只会把本底噪声一起放大，不会改善约 11–15 dB 的 SNR。
这属于 3.5mm 耳机本身的硬件属性，host 侧无法改变。

**结论（已实测验证）：瓶颈在 3.5mm 耳机本身的麦克风。** AB13X 是 **Type-C 转 3.5mm 的转接头**，
它提供的增益是固定的 +32 dB，所以采集端的 SNR 完全由插在 3.5mm 口上的模拟耳机决定。本次把
`MIC_TOTAL_GAIN_DB` 提到 52 dB 是为了让现有信号进入 ASR/Opus 的合适电平；如果还想明显改善，
换一副麦克风灵敏度正常的耳机仍然是最有效的办法。**所以：**

- **换模拟耳机不影响 USB 侧。** 描述符（VID/PID、接口、MPS 208/384、48 kHz）都由 AB13X 自己的
  固件决定，与插什么耳机无关——上面的 FIFO 切分和写死的 48000 依然有效，不需要重调。
- 只有**换掉整个 USB 适配器**（换成另一个 UAC 设备）时，才需要重新核对下面两项：
  MPS 变大可能撞 `Invalid FIFO config`（见上文 DWC FIFO 一节），以及只支持 44100 的设备会导致
  变速变调。
- 另外，3.5mm 耳机的 **CTIA / OMTP 引脚顺序不匹配**会让麦克风完全无声或极噪，这与 USB 无关，
  换耳机后麦克风若有异常可以先怀疑这一点。

如果要继续改善（针对差耳机），唯一手段是开启降噪（见上）。但仅为廉价配件而改所有板级共用的
核心代码不划算，当时的决定是不动。

其余两个踩过的坑：

1. **不要用 `mic_peak` 的单点值判断电平。** 早期版本只取单次 `Read()`（约 10 ms）的峰值，
   `154`/`664` 那类数字严重低估语音包络，据此加 +30 dB 软件增益纯属误判。现在 `mic_peak`
   是自上次打印的保持峰值，并新增 `mic_avg`（抗瞬态）。
2. **不要用 `set_volume(百分比)` 的返回值判断成功。** 它把百分比映射到
   `vol_min_db..vol_max_db`，而这两个值只在 `uac_host_device_open` 里那次
   `get_volume_range` 成功时才会被填；若查询失败或该 feature unit 没有音量控制，范围就是
   `0..0`，此时 `set_volume(100)` 写入的是 **0 dB**，却照样返回 ESP_OK、不打任何警告。
   要判断真实生效值，必须用 `uac_host_device_get_volume_db()` 读回。

## 播放中插话（服务器端 AEC）

本板没有播放参考通道（`input_reference_ = false`），因此无法在设备端做 AEC。插话使用
**服务器端 AEC**：

- 板级 `config.json` 里设置 `CONFIG_USE_SERVER_AEC=y`
- hello 消息会带上 `features.aec = true`，默认进入 `realtime` 模式
- TTS 播放期间麦克风继续上传，由服务器消除 TTS 回声并检测用户插话
- 服务器确认插话后会下发 `tts stop`，设备回到 Listening，不需要等 TTS 播完

按钮触发仍然是普通的 `listen start`，只是模式从 `auto` 变成了 `realtime`。如果服务器不支持
AEC，播放期间的麦克风音频会被当成用户说话，可能自打断，所以这个选项必须和服务器配套。

## 长 TTS 播放

下行的 `audio_decode_queue_` 不再只保留 1.2 秒（20 个 60 ms 包），而是 60 秒（1000 个包）。
服务器可以把一整段 TTS 突发推下来，设备按自己的节奏慢慢解码播放，不会因为队列满而静默丢掉
后半段。队列满时仍会打印限速警告并计数，便于发现异常的超长积压。

## 已知缺陷

拔插竞态：`Read`/`Write` 取到 handle 后释放 `handle_mutex_` 才调用可能阻塞 100 ms 的
`uac_host_device_read/write`，而 `CloseInterface` 是持锁直接 `uac_host_device_close()`。
拔线瞬间存在 use-after-free 风险。
