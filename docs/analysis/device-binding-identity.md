# 设备绑定身份链分析 + 「一机多服务端」可行性判定

- 仓库：`78/xiaozhi-esp32`（本目录 fork，HEAD `66ec74b`，已合上游 2.5.3）
- 方法：只读代码调查 + GitHub issue 检索（2026-10-28，GitHub Search API）
- 标注约定：`代码事实` / `推断（依据…）` / `未验证（缺…）`
- 本次未改任何源码；唯一新增文件即本文。

---

## 1. 结论先行

1. **官方服务端上「一机多账号」做不到。** 六位码不是身份，只是一张「把当前正在轮询的这台设备认领到某个账号」的短期票据；真正的设备主键在每次请求的 header 里：`Device-Id`（= Wi-Fi MAC）+ `Client-Id`（= NVS 里的随机 UUID）。（`代码事实`：`main/ota.cc:71-72`、`main/system_info.cc:42`、`main/boards/common/board.cc:15-19`）
2. **不改服务端能拿到的最好结果是「一机轮流接多个服务端」**：把 `wifi/ota_url` 指向另一个后端，重启后由那个后端重新下发它自己的凭据。配网页面本来就有这个入口，维护者已确认它会覆盖编译期默认值（issue **#1783**，`managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc:605-608`）。这是**切换**，不是并行，也不是同一账号多个人设。
3. **只改 `endpoint` 不行。** `endpoint` 每次开机都会被 `/check-version` 的响应整体覆写（`main/ota.cc:160-175`，`main/application.cc:378-393`）。能长期生效的只有 `ota_url`。
4. **要「官方账号 A + 官方账号 B」只能伪造第二套 `Device-Id`/`Client-Id`（方案 A，约 120–180 行）。** 协议层允许（v1 激活没有任何签名，MAC 只是客户端自报的 header）；但服务端几乎肯定按 MAC/serial 去重，第二条绑定会被拒绝或归并——**推断（依据 issue #1674「设备已经添加过」、#2181「unlink / device 0」、#359 求硬件解绑、#1785 的 client_id 里直接含 MAC）**，`未验证（缺一条真实双账号绑定实验）`。
5. **参考讨论**：#1783（换 OTA URL 到自建服务端可行）、#1785（贴出真实 OTA 响应：`client_id` = `GID_test@@@<mac>@@@<uuid>`、`publish_topic=device-server`、`subscribe_topic=null`）、#1674、#2181、#359、#1650（角色切换，仍 open）。**没有找到实现了「多服务端/多身份」的 PR**（`多设备`/`device-id mac`/`uuid clone`/`switch server`/`multiple devices`/`激活`/`绑定`/`解绑` 等关键词均无命中）。
6. **最省的一条路**：先用方案 B（零代码，配网高级选项改 OTA URL）验证「第二个后端是否接受这台设备」；只有确认官方服务端也接受第二套身份时，方案 A 才值得写。方案 C（并行双会话）不建议：本代码库只有一份 `protocol_` 和一份 `audio_service_`（`main/application.h:135,144`）。

**对用户原话的直接回答：绑定除了六位码，确实还绑了硬件唯一标识。** 六位码之外，`Device-Id` 是芯片出厂的 Wi-Fi MAC（eFuse），`Client-Id` 是软件 UUID（可擦除重生），v2 设备还多一个 eFuse 序列号。六位码只是让服务端把「这台已经在轮询的设备」和「你的账号」连起来的临时口令。

---

## 2. 绑定时序（发了什么 / 收到什么）

入口：联网成功 → `HandleNetworkConnectedEvent` 起 `activation` 任务（`main/application.cc:298-318`）→ `ActivationTask`：`CheckAssetsVersion` → `CheckNewVersion` → `InitializeProtocol`（`main/application.cc:378-393`）。

### 步骤 1：`POST/GET {ota_url}`（`/check-version`）

`ota.cc:101-110`。body 为 `Board::GetSystemInfoJson()`，非空所以用 POST（`main/ota.cc:103-104`）。

| 方向 | 字段 | 来源 |
|---|---|---|
| 请求 header | `Activation-Version: 1\|2` | `has_serial_number_`（`main/ota.cc:70`） |
| 请求 header | `Device-Id` | `SystemInfo::GetMacAddress()`（`main/ota.cc:71`） |
| 请求 header | `Client-Id` | `Board::GetUuid()`（`main/ota.cc:72`） |
| 请求 header | `Serial-Number` | eFuse USER_DATA 文本（仅 v2，`main/ota.cc:73-75`） |
| 请求 header | `User-Agent` | `BOARD_NAME/version`（`main/system_info.cc:53-57`） |
| 请求 body | `version/language/flash_size/minimum_free_heap_size/mac_address/uuid/chip_model_name/chip_info/application/partition_table/ota/display/board` | `main/boards/common/board.cc:101-171` |
| 响应 | `activation{message, code, challenge, timeout_ms}` | 解析于 `main/ota.cc:135-157` |
| 响应 | `mqtt{...}` | 原样落盘，`main/ota.cc:159-178` |
| 响应 | `websocket{url, token, version}` | 原样落盘，`main/ota.cc:180-199` |
| 响应 | `server_time{timestamp, timezone_offset}` | `main/ota.cc:201-224` |
| 响应 | `firmware{version, url, force}` | `main/ota.cc:226-254` |

注意：`Activation-Version: 2` 时服务端回 `challenge`，v1 时通常只回 `code`；`activation_timeout_ms_` 被解析但全仓没有读取点（`main/ota.h:52`、`main/ota.cc:155`）——`代码事实`。

### 步骤 2：显示/播报六位码

`application.cc:520-523`。`ShowActivationCode` 用服务端下发的 `activation.message` 作为屏幕文本（`Alert` → `SetChatMessage`，`main/application.cc:764-775`），并把 `code` 逐位播放提示音（`main/application.cc:740-762`）。

**六位码文本本身来自服务端**，固件里唯一和 `xiaozhi.me` 有关的代码是把含该域名的消息交给表情屏（`main/display/emote_display.cc:205`）；编译期默认后端只有 `https://api.tenclass.net/xiaozhi/ota/`（`main/Kconfig.projbuild:3-6`）。→ `代码事实`

### 步骤 3：用户在服务端网页填码

客户端无感知、无对应代码。填码动作发生在服务端，客户端只负责轮询。

### 步骤 4：轮询 `POST {ota_url}/activate`

- URL 拼接：`main/ota.cc:510-515`；**复用同一套 header**（`SetupHttp()`，`main/ota.cc:517` → `70-72`），所以这一跳同样携带 `Device-Id`/`Client-Id`。
- body：v1 为 `{}`；v2 为 `{algorithm:"hmac-sha256", serial_number, challenge, hmac}`（`main/ota.cc:467-502`）。
- 返回码：`200` 成功；`202` 视为「还没填码」，3 秒后重试；其它错误 10 秒后重试；最多 10 次（`main/ota.cc:532-537`，`main/application.cc:527-540`）。

### 步骤 5：再查一次 `/check-version` 拿凭据

`CheckNewVersion` 是 `while(true)`：激活成功后回到循环顶部重新 `CheckVersion()`；这次响应里没有 `activation` 才 `break`（`main/application.cc:461-541`，判定在 `515-518`）。凭据在此时才落盘（`main/ota.cc:159-199`）。

### 步骤 6：连协议

`InitializeProtocol` 按**最近一次响应**的 flag 选协议：有 `mqtt` 段 → MQTT；否则有 `websocket` 段 → WebSocket；都没有 → 默认 MQTT（`main/application.cc:544-558`，flag 见 `main/ota.h:22-23`）。两者同时下发时 **MQTT 优先**（`main/application.cc:551-553`）。

- MQTT：从 NVS `mqtt` 读 `endpoint/client_id/username/password/keepalive/publish_topic`（`main/protocols/mqtt_protocol.cc:80-86`）→ 连 broker（`:180`）→ 向 `publish_topic` 发 `hello`（`:387-411`，发送见 `:192-201`）→ 收服务端 `hello`（`session_id`、UDP server/port/key/nonce、音频参数，`:413-503`）→ 建 UDP（`:274-385`）。
- WebSocket：从 NVS `websocket` 读 `url/token/version`（`main/protocols/websocket_protocol.cc:80-86`），带 `Device-Id`/`Client-Id`（`:105-106`）连接并发 `hello`（`:210-234`）。

---

## 3. 身份因子表

| 因子 | 来源 / 算法 | 可否修改 | 改了会怎样 | 证据 |
|---|---|---|---|---|
| `Device-Id` | `esp_read_mac(ESP_MAC_WIFI_STA)`（以太网板走 `ESP_MAC_ETH`，P4 走 `esp_wifi_get_mac`），格式化成小写冒号 MAC | 代码里无覆盖入口；但只是 HTTP header，可加 NVS 覆盖 | 服务端视为另一台设备；system info JSON 的 `mac_address` 必须同步改才自洽 | `main/system_info.cc:35-47`、`main/ota.cc:71`、`main/boards/common/board.cc:105` |
| `Client-Id` | NVS 命名空间 `board` 的 `uuid`；首次开机 `esp_fill_random` 生成 UUID v4 并落盘 | **可改**：直接写 NVS `board/uuid`；没有 Kconfig/配网页入口 | 服务端视为换机；擦 NVS 会重新生成 | `main/boards/common/board.cc:15-19,24-43`、`main/boards/common/board.h:69` |
| eFuse serial number | `ESP_EFUSE_USER_DATA` 32 字节，必须是「可打印 + 非空白 + 零填充」文本 | 一次性熔丝，不可改 | 决定 `Activation-Version`；非文本/空白时降级 v1 | `main/ota.cc:29-51` |
| `Activation-Version` | `has_serial_number_ ? 2 : 1` | header 可改，但服务端据此选校验方式 | 强行改 1 等于放弃 HMAC（服务端可能拒绝或降级信任） | `main/ota.cc:70` |
| `HMAC(challenge)` | `esp_hmac_calculate(HMAC_KEY0, challenge)`，输出 hex | 不可改（密钥在 eFuse） | 只证明「这颗芯片」，**输入里没有 Device-Id** | `main/ota.cc:477,483-494` |
| `User-Agent` | `BOARD_NAME/固件版本` | 编译期 | 服务端可据此决定下发哪个固件 | `main/system_info.cc:53-57` |
| `mac_address` / `uuid`（body 内） | 同 `Device-Id` / `Client-Id` | 同上 | 服务端同时从 header 和 body 看到同一组值 | `main/boards/common/board.cc:105-106` |

关于 `HMAC_KEY0`：仓库内只有调用点，没有烧写逻辑；「必须有 purpose=HMAC_UP 的 eFuse key block 才能算成功」是 ESP-IDF API 语义 → `推断（依据 ESP-IDF HMAC API 约定，仓库内无对应代码）`；具体板子是否已烧 `未验证（缺 eFuse 读出的 key purpose 记录）`。

---

## 4. 服务端主键推断

### 代码事实

1. 凭据**全部由服务端下发、客户端原样保存**，客户端不参与计算：`main/ota.cc:160-175`（mqtt）、`180-196`（websocket）。
2. 发布 topic 由服务端给，客户端不构造：`main/protocols/mqtt_protocol.cc:86`。
3. 会话 id 由服务端在 `hello` 响应里给：`main/protocols/mqtt_protocol.cc:420-424`、`main/protocols/websocket_protocol.cc:247-251`。
4. 服务端在 `/check-version` 与 `/activate` 两跳都能看到同一组 `Device-Id`/`Client-Id`：`main/ota.cc:70-72,517`。
5. 客户端**当前代码不订阅任何 topic**：`main/protocols/mqtt_protocol.cc` 全文无 `Subscribe` 调用；历史上 2025-01 的提交 `e2c5578` 删掉了 `subscribe_topic_` 的读取与 `mqtt_->Subscribe(subscribe_topic_, 2)`。→ 这意味着下行控制消息的投递方式在服务端/broker 侧，**`未验证（缺 broker 侧订阅或按 client_id 推送的机制证据）`**。

### 推断

- **P1：服务端的设备主键 = MAC，client_id 里还拼进了 UUID。**
  `推断（依据：issue #1785 贴出的真实响应 `"client_id": "GID_test@@@f4_12_fa_b9_3b_94@@@bad76396-…"`，第二段正是 MAC 的 `_` 分隔形式，第三段是 UUID；代码侧两处都发送这两个值）`。
- **P2：账号 ↔ 设备是 1:1，同一 MAC 不能同时挂两个账号。**
  `推断（依据：#1674 从未用过验证码却被提示「该设备已经添加过」；#2181 第二台设备在控制台显示 unlink/device 0；#359 要求「通过硬件解绑」；#1785 维护者让走解绑流程）`。
- **P3：六位码只是认领票据，不是身份。**
  `推断（依据：码由服务端生成并在响应里下发 `main/ota.cc:143-147`；`/activate` 复用同一组身份 header `main/ota.cc:517`；固件不保存码）`。反向证据也成立：擦 NVS 后 UUID 变了但 MAC 不变，服务端仍认得出这台设备。
- **P4：同一颗芯片伪造两个 Device-Id，在 v2 上会被 `serial_number` 串起来。**
  `推断（依据：HMAC 只覆盖 challenge `main/ota.cc:477`，但 payload 同时上报 `serial_number` `main/ota.cc:492`，服务端可据此做唯一性约束）`；是否真的拒绝 `未验证（缺双身份 + 同 serial 的实测）`。
- **P5：同一 `client_id` 的并发连接会被 broker 踢掉。**
  `推断（依据 MQTT 3.1.4-2 的 ClientId takeover 语义；本仓库无任何接管/踢下线处理，只有断线后固定 60s 重连 `main/protocols/mqtt_protocol.h:22`）`；broker 实际行为 `未验证（缺 broker 日志/双连实验）`。

---

## 5. 三个方案可行性

### 方案 A：多身份切换（本地 N 套身份，运行时切换 + 重激活）

| 项 | 内容 |
|---|---|
| 改哪些文件 | `main/system_info.h/.cc`（新增 `GetDeviceId()`：读 NVS `identity/device_id`，空则回落 MAC，约 25 行）；5 个调用点换成它：`main/ota.cc:71`、`main/protocols/websocket_protocol.cc:105`、`main/boards/common/esp32_camera.cc:280`、`main/boards/common/esp_video.cc:1010`、`main/sensecap-watcher/sscma_camera.cc:701`（约 5 行）；`main/boards/common/board.cc:15-19` + `board.h`（uuid 按 profile 取，加 `SetUuid`，约 15 行）；`main/ota.cc:57,162,183`、`main/protocols/mqtt_protocol.cc:80`、`main/protocols/websocket_protocol.cc:80`（命名空间加 profile 后缀，约 30 行）；`main/mcp_server.cc` 加一个 `self.identity.select` 工具（写 active slot + `esp_restart`，约 30 行） |
| 行数量级 | ~120–180 行 |
| 风险 | ① 官方服务端按 MAC/serial 去重（P1/P2/P4），第二条绑定可能直接被拒；② 每套身份的固件 OTA 源不同，切换可能被推不同固件；③ 身份串会写进 INFO 日志，`main/ota.cc:500` 明文打印整个 HMAC payload；④ 两套 profile 共用同一 NVS 分区，需算好空间 |
| 验证方法（一次实验证死） | 在**无 eFuse serial 的 v1 板**上：slot B 的 `device_id` = 出厂 MAC+1（只改 header 来源，不动 Wi-Fi 实际 MAC），`uuid` 用新随机值；重启应出现**新的六位码**，用账号 B 绑定。判据：绑定成功且 A/B 两个账号各看到一台设备 → A 可行；若第二步返回「设备已添加」或无验证码 → MAC/serial 去重成立，A 对官方服务端不可行。最后切回 slot A，确认账号 A 对话正常 |
| 并行/切换 | **只能切换**（同一时刻一套身份、一条连接） |

### 方案 B：单身份多服务端（保持一套身份，只换 `endpoint`）

| 项 | 内容 |
|---|---|
| 改哪些文件 | **零代码可先验证**：配网页面「高级选项 → Custom OTA URL」写入 NVS `wifi/ota_url`（`managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc:605-608`，页面入口 `assets/wifi_configuration.html:552-555`，是否展示由 `main/boards/common/wifi_board.cc:59` 控制）；或编译期改 `CONFIG_OTA_URL`（`main/Kconfig.projbuild:3-6`）。要保存多个后端再加 ~40–60 行（NVS 列表 + `self.server.select` 工具） |
| 能否只改 `endpoint` | **不能。** `endpoint` 每次 `/check-version` 都被服务端响应覆写（`main/ota.cc:160-175` + `main/protocols/mqtt_protocol.cc:80-86`），而 `/check-version` 每次开机都跑（`main/application.cc:378-393,455-465`）。手改的 endpoint 只能活到下一次开机/检查 |
| 行数量级 | 0 行（配网）／40–60 行（多后端列表） |
| 风险 | ① 每个后端各自一套绑定记录，切换后旧凭据被同一 `mqtt`/`websocket` 命名空间覆盖，回切要重新走激活；② 自建后端必须自己实现 `/check-version` 与 `/activate`（#1783 表明可跑通，能出六位码）；③ 固件升级 URL 也跟着换；④ 如果两个后端都下发 `mqtt` 与 `websocket`，仍固定选 MQTT（`main/application.cc:551-553`） |
| 验证方法（一次实验证死） | 配网高级选项把 OTA URL 指向自建服务端 → 重启 → 应出现该服务端的六位码并绑定成功；再切回 `https://api.tenclass.net/xiaozhi/ota/` → 重启。判据：原账号**无需重新填码**即可对话 → 旧绑定仍按同一身份保留（B 可用）；若又出新码/被要求重新绑定 → 该后端不认旧身份，B 只能当「重新绑定」用 |
| 并行/切换 | **只能切换**（重启生效） |

### 方案 C：并行双会话（两套身份/协议栈同时在线，音频复用一路 codec）

| 项 | 内容 |
|---|---|
| 改哪些文件 | `main/application.h:135`（`protocol_` 单实例 → 双实例/容器）、`main/application.cc`（`InitializeProtocol`、`OnIncomingAudio`/`OnIncomingJson` 按来源路由、`ContinueOpenAudioChannel`、`HandleToggleChatEvent`）、`main/device_state.h` + `device_state_machine`（全局单一状态 → 每会话状态）、`main/audio/`（`audio_service_` 单实例 `main/application.h:144`，解码队列与播放需打来源标签/仲裁）。协议类本身已按实例持有状态，可复用 |
| 行数量级 | 400–800 行，且要动状态机与音频队列 |
| 风险 | ① 两个后端可能同时推 TTS，`PushPacketToDecodeQueue` 没有来源标记（`main/application.cc:567-571`），只能硬切/丢弃；② 唤醒词与 `kDeviceStateSpeaking/Listening` 是全局单值，两个会话互斥；③ 同一 `client_id` 并发会被 broker 踢（P5），所以必须先做方案 A 的身份隔离，成本叠加；④ UDP 音频通道各自独立，但 codec 只有一路输出 |
| 验证方法（一次实验证死） | 两个自建后端、两套 client_id，同时触发 TTS；判据：两路音频是否互相打断/丢帧、broker 是否出现同 id 断连。若无法做到「任一会话随时可用」→ C 不成立 |
| 并行/切换 | 设计目标才是并行；按当前结构只能先做**切换**，真并行需要重写状态/音频层 |

---

## 6. file:line 索引表

| # | 断言 | 位置 |
|---|---|---|
| 1 | `Activation-Version` 由 `has_serial_number_` 决定 | `main/ota.cc:70` |
| 2 | `Device-Id` = MAC 字符串 | `main/ota.cc:71` |
| 3 | `Client-Id` = `Board::GetUuid()` | `main/ota.cc:72` |
| 4 | `Serial-Number` 仅 v2 发送 | `main/ota.cc:73-75` |
| 5 | `/check-version` 的 URL 取 NVS `wifi/ota_url`，空则用 `CONFIG_OTA_URL` | `main/ota.cc:56-62` |
| 6 | `/check-version` 请求 body 为系统信息 JSON | `main/ota.cc:103-105` |
| 7 | `activation{message,code,challenge,timeout_ms}` 解析 | `main/ota.cc:135-157` |
| 8 | mqtt 段所有键原样落盘 NVS | `main/ota.cc:159-178` |
| 9 | websocket 段所有键原样落盘 NVS | `main/ota.cc:180-199` |
| 10 | `/activate` URL 拼接 | `main/ota.cc:510-515` |
| 11 | `/activate` 复用同一套身份 header | `main/ota.cc:517` |
| 12 | 202 视为未完成、其它码失败 | `main/ota.cc:532-537` |
| 13 | eFuse USER_DATA 读 serial | `main/ota.cc:29-49` |
| 14 | HMAC 输入只有 challenge | `main/ota.cc:477` |
| 15 | HMAC payload 字段（含 serial） | `main/ota.cc:490-494` |
| 16 | HMAC payload 明文打日志 | `main/ota.cc:500` |
| 17 | MAC 来自 `esp_read_mac(WIFI_STA)` | `main/system_info.cc:42` |
| 18 | MAC 格式化 | `main/system_info.cc:45` |
| 19 | UUID 从 NVS `board/uuid` 读 | `main/boards/common/board.cc:16` |
| 20 | 首次开机生成并落盘 UUID | `main/boards/common/board.cc:17-20` |
| 21 | UUID 由 `esp_fill_random` 生成 v4 | `main/boards/common/board.cc:24-43` |
| 22 | `GetUuid()` 是 virtual、全仓无 override | `main/boards/common/board.h:69` |
| 23 | 系统信息 JSON 同时含 `mac_address` 与 `uuid` | `main/boards/common/board.cc:105-106` |
| 24 | 六位码显示/播报入口 | `main/application.cc:520-523,740-762` |
| 25 | 激活轮询 10 次、202 等 3s、错误等 10s | `main/application.cc:527-540` |
| 26 | 无 activation 段才退出循环 | `main/application.cc:515-518` |
| 27 | 协议选择由 OTA 响应 flag 决定，MQTT 优先 | `main/application.cc:544-558` |
| 28 | 单 `protocol_` 成员 | `main/application.h:135` |
| 29 | 单 `audio_service_` 成员 | `main/application.h:144` |
| 30 | MQTT 凭据/topic 全部读 NVS | `main/protocols/mqtt_protocol.cc:80-86` |
| 31 | MQTT 用 server 给的 client_id/username/password 连接 | `main/protocols/mqtt_protocol.cc:180` |
| 32 | 断线后固定 60s 重连（无退避/无踢下线处理） | `main/protocols/mqtt_protocol.cc:100-107`、`mqtt_protocol.h:22` |
| 33 | `goodbye` 只比 session_id 后关音频通道 | `main/protocols/mqtt_protocol.cc:129-143` |
| 34 | session_id 由服务端 hello 下发 | `main/protocols/mqtt_protocol.cc:420-424` |
| 35 | 客户端只 publish，不 subscribe | `main/protocols/mqtt_protocol.cc:192-201`（全文无 `Subscribe`） |
| 36 | WebSocket 凭据来自 NVS `websocket/*` | `main/protocols/websocket_protocol.cc:80-86` |
| 37 | WebSocket 也带 `Device-Id`/`Client-Id` | `main/protocols/websocket_protocol.cc:105-106` |
| 38 | 配网高级选项保存 `wifi/ota_url` | `managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc:605-608` |
| 39 | 配网页展示 OTA URL 开关 | `main/boards/common/wifi_board.cc:59` |
| 40 | 默认 OTA 后端 | `main/Kconfig.projbuild:3-6` |
| 41 | 恢复出厂擦 NVS（UUID 会重生，MAC 不会） | `main/boards/common/system_reset.cc:41` |
| 42 | 表情屏特判含 `xiaozhi.me` 的消息 | `main/display/emote_display.cc:205` |

---

## 7. 必答问题 Q1–Q7

**Q1. `Device-Id` / `Client-Id` 在哪里设置、值怎么算？**
`Device-Id` 在 `Ota::SetupHttp()` 设置（`main/ota.cc:71`），值 = `SystemInfo::GetMacAddress()`：Wi-Fi 板走 `esp_read_mac(ESP_MAC_WIFI_STA)`（来自芯片出厂 eFuse MAC），以太网板走 `ESP_MAC_ETH`，P4 走 `esp_wifi_get_mac`，格式化为 `xx:xx:xx:xx:xx:xx`（`main/system_info.cc:35-47`）。
`Client-Id` 在 `main/ota.cc:72` 设置，值 = `Board::GetUuid()`（`main/boards/common/board.h:69`），实现为构造时从 NVS `board/uuid` 读，空则 `esp_fill_random` 生成 UUID v4 并写回（`main/boards/common/board.cc:14-43`）。
`Client-Id` 可被覆盖（NVS 直接写 `board/uuid`；擦 NVS 会重新生成，`main/boards/common/system_reset.cc:41`）。`Device-Id` 代码里**没有任何覆盖入口** → `代码事实`。

**Q2. `Board::GetUuid()` 到底返回什么？有没有已存在的覆盖入口？**
返回 NVS `board/uuid` 里的字符串（随机 UUID v4），与 MAC/eFuse 无关（`main/boards/common/board.cc:14-43`）。它是 `virtual`（`main/boards/common/board.h:69`），但全仓没有任何 board override（grep 仅此一处声明）。已有覆盖入口只有 NVS 本身（`Settings("board", true).SetString("uuid", ...)`）；Kconfig / 配网页 / MCP 工具都没有暴露它 → `代码事实`。

**Q3. `Activation-Version: 2` 的触发条件？HMAC 覆盖哪些输入？同一 eFuse 能否为两个 Device-Id 生成有效响应？**
触发条件：`Ota` 构造时从 `ESP_EFUSE_USER_DATA` 读到「可打印、非空白、零填充」文本，置 `has_serial_number_ = true`（`main/ota.cc:29-49`），header 处用三元表达式（`:70`）。非文本数据会打 warning 并退回 v1（`:46-48`）。
HMAC 输入**只有 `activation_challenge_`**，算法 `esp_hmac_calculate(HMAC_KEY0, challenge.data(), challenge.size())`（`main/ota.cc:477`），hex 后放进 payload，与 `serial_number`、`challenge` 并列上报（`:490-494`）。
所以：**从代码看，同一个 eFuse（同一 HMAC key）完全可以为任意 `Device-Id` 生成有效响应**——Device-Id 根本不参与 HMAC。但服务端会同时看到 `Serial-Number`（header，`:74`）和 `serial_number`（body，`:492`），可以据此把两个 Device-Id 归并/拒绝。服务端是否真的这么做：`未验证（缺真实双身份 + 同 serial 的绑定实验）`。

**Q4. 激活后落盘哪些 settings key？是否与 device id 绑定？**
两处（就是 `main/ota.cc` 里两组 `settings.SetString`）：
- `mqtt` 命名空间：遍历响应 `mqtt` 对象，字符串写 `SetString`、数字写 `SetInt`（`main/ota.cc:160-175`）。实际键即 `endpoint`/`client_id`/`username`/`password`/`keepalive`/`publish_topic`（读侧见 `main/protocols/mqtt_protocol.cc:81-86`）。
- `websocket` 命名空间：同样整体落盘（`main/ota.cc:180-196`），读侧 `url`/`token`/`version`（`main/protocols/websocket_protocol.cc:80-83`）。
另外 `server_time` 只 set 系统时间不落盘（`:201-224`），`firmware` 只用于升级（`:226-254`）。
**是否与 device id 绑定：客户端不构造这些值，全部由服务端下发**（`代码事实`）。从 #1785 的真实响应看，服务端生成的 `client_id` 里直接拼了 MAC 和 UUID → 是服务端把凭据和设备身份绑在一起（`推断，依据 issue #1785`）。`publish_topic` 由服务端给（`main/protocols/mqtt_protocol.cc:86`），topic 内容是否含设备标识：`未验证（缺真实 topic 样例；#1785 只看到共享的 device-server）`。

**Q5. publish/subscribe topic 由谁决定？topic 里是否含会话级 id？**
publish：由服务端决定，客户端只读取并原样 publish（`main/protocols/mqtt_protocol.cc:86,192-201`）。subscribe：**当前代码完全没有订阅**（HEAD `mqtt_protocol.cc` 无 `Subscribe` 调用；2025-01 提交 `e2c5578` 删除了 `subscribe_topic_` 的读取与订阅），服务端也不再需要它（#1785 的响应里 `subscribe_topic` 是字符串 `"null"`）→ 下行消息如何到达 `未验证（缺 broker 侧推送机制证据）`。
会话级 id：`session_id` 由服务端在 `hello` 响应中下发（`main/protocols/mqtt_protocol.cc:420-424`；WS 同理 `websocket_protocol.cc:247-251`），只出现在 JSON 消息体里（如 `main/protocols/protocol.cc:67,98,104`），**不参与 topic 拼接**。

**Q6. 有没有「同一 client_id 重复连接被踢」的痕迹？**
代码里**没有**主动检测或处理：断线只做 `on_disconnected_` 回调 + 固定 60 秒后重连（`main/protocols/mqtt_protocol.cc:100-107`，`mqtt_protocol.h:22`），没有任何「被接管/被踢」错误码分支（`NetworkError` 只有通用 `ServerDisconnected`，`managed_components/78__esp-ml307/include/network_error.h:44`）。唯一的服务端主动断开痕迹是对 `goodbye` 的兼容处理（`mqtt_protocol.cc:129-143`）。
「同 client_id 会被 broker 踢」属于 MQTT 规范行为 → `推断（依据 MQTT 3.1.4-2；broker 实际日志未验证）`。这直接否掉「同一身份并行双会话」的想法。

**Q7. 协议选择是否由服务端 OTA 配置决定？换服务端是否必须换一整套 OTA 配置？**
是。`InitializeProtocol` 只看 `ota_->HasMqttConfig()` / `HasWebsocketConfig()`（`main/application.cc:551-558`），这两个 flag 只在 `CheckVersion()` 解析响应时被置位/清空（`main/ota.cc:159,175,180,196`），与本地 Kconfig 无关。所以协议、broker、凭据、topic 是一整套、同源、由当前 `ota_url` 那个后端下发；换服务端 = 换这套配置。旧的 `mqtt`/`websocket` 键**不会被删除**，只是在新响应缺段时不再被选中（flag 为 false），下次切回仍能复用 → `代码事实`。`未验证（缺「旧键残留是否影响新后端」的实测）`。

---

## 8. 未完成 / 未验证清单

1. 没有真实设备做「第二套 Device-Id 绑第二个官方账号」实验——这是方案 A 成立与否的唯一判据（见 5.A 验证方法）。
2. 没有 broker 侧日志，无法确认「同 client_id 接管」行为与下行消息投递机制（当前客户端不订阅）。
3. 没有读出板子 eFuse 的 HMAC key purpose / USER_DATA 实际内容，`Activation-Version: 2` 在目标板上是否生效未确认。
4. 没有服务端代码，P1/P2 只是基于 #1785 的响应样例与多条 issue 的交叉推断；若要证死，需要一次抓包或服务端管理页截图。

---

## 9. 技术部评审附注（抽验记录）

**抽验结论：通过。** 抽查 8 条 `file:line` 断言与 6 个 issue 编号，全部复核一致：

| 抽验项 | 复核结果 |
|---|---|
| `main/system_info.cc:35-47` MAC 来源 | ✓ 与报告一致（`esp_read_mac(ESP_MAC_WIFI_STA)`，P4/以太网分支均存在） |
| `main/boards/common/board.cc:15-20` UUID 读 NVS `board/uuid` | ✓ |
| `main/boards/common/board.cc:105-106` body 同时含 mac_address/uuid | ✓ |
| `main/ota.cc:29-49` eFuse USER_DATA 文本判定 | ✓ |
| `main/ota.cc:477` HMAC 输入只有 challenge | ✓ |
| `main/protocols/mqtt_protocol.cc` 全文 0 处 `Subscribe` | ✓ |
| `main/protocols/mqtt_protocol.h:22` 固定 60s 重连 | ✓ |
| `main/ota.cc:155` `activation_timeout_ms_` 只写不读 | ✓ |
| issue #1783 / #1785 / #1674 / #2181 / #359 / #1650 | ✓ 编号真实、标题与引用一致 |
| #1785 的 `client_id: GID_test@@@f4_12_fa_b9_3b_94@@@bad76396-…` | ✓ 原文可查，`subscribe_topic` 确为 `"null"` |

**两处需要修正/补充的口径：**

1. 第 1 节把 **#1783 概括为「换 OTA URL 到自建服务端可行」偏强**。该 issue 的完整事实是：78 确认「配网高级选项的 OTA URL 会覆盖编译期默认值」（这一点成立），但楼主后续自述是**自己的服务端没搭好**，改好后才出六位码并绑定成功，且绑定后功能仍不通。#1783 仍 open。
   → 正确口径：**配网 OTA URL 覆盖机制成立（代码 + 维护者确认）；"换后端能正常对话"未在官方后端上被任何证据支持。**
2. **#1785 的 `GID_test` 是「未绑定设备的兜底测试凭证」**（该 issue 的特征是响应里没有 `activation` 段）。这补充了一条对方案 A 很关键的事实：**官方服务端对"已绑定设备"直接不下发 `activation`**，所以方案 A 的判据要写死成「是否出现新的 `activation.code`」——不出码就等于该身份被服务端认定为已绑定/已存在。

**对方案 A 验证实验的收敛（我改一处判据）：**

原报告说「slot B 的 device_id = 出厂 MAC+1」。改成 **`device_id` 用与真实 MAC 完全无关的随机值**更干净：因为如果服务端按 MAC 前缀/厂商段做校验，MAC+1 仍落在同一 OUI；而随机值若被接受，反而证明服务端**完全不校验** Device-Id 与真实 MAC 的对应关系，方案 A 的可行性上限更高。两种都行，但归档时要写明用的是哪一种。

---

## 10. 方案 A 真机验证结果（2026-10-28，通过）

**结论：方案 A 成立。** 一台硬件在官方服务端眼中变成多台设备，一次绑定后可在设备端来回切换，不再需要 web 操作。

### 实现（实验版，约 79 行 diff + 新增 `main/identity.{h,cc}`）

| 项 | 实现 |
|---|---|
| 身份存储 | NVS `identity`：`slot` / `count`(默认3) / `id<N>_mac` / `id<N>_uuid` / `id<N>_mode` |
| 槽 0 | 真实 MAC + `board/uuid`，`mode=hw`，**不 provisioning**，现有绑定完全不受影响 |
| 槽 1..N | `02:` 前缀 locally-administered MAC + 随机 v4 UUID，`mode=1` → 强制激活 v1（无 serial / 无 HMAC） |
| 凭据隔离 | `mqtt`/`websocket` → `mqtt<N>`/`websocket<N>`；启动时把旧无后缀凭据一次性迁到槽 0 |
| 切换 | M 键长按 3s 轮换 + 重启；另加 MCP 工具 `self.identity.select` |
| 可观测 | 启动打 `Identity: slot=N device_id=… uuid=… mode=… activation_version=…` |

### 真机日志（板子 `kayzh-ml307-headphone`，固件 2.5.3，IDF v6.1）

```
# 首次切到槽 1：服务端当它是一台新设备，下发新码
I (2263)  Identity: Provisioned slot 1: device_id=02:09:6d:48:b0:ae uuid=b32fa949-…
I (2263)  Identity: slot=1 … mode=v1 activation_version=1
W (4703)  Application: Alert [link] 激活设备: xiaozhi.me
I (33643) Ota: Activation successful
I (43643) MQTT: Session ID: ac9f234c        → connecting -> listening

# 切回槽 0：不出码，直接连旧身份
I (2193)  Identity: slot=0 device_id=98:a3:16:cb:20:fc uuid=dee00792-… mode=hw
I (3723)  Application: Activation done      （无 activation 告警）
I (19133) MQTT: Session ID: 74eec4a2        → listening -> speaking -> listening（对话成功）

# 再切回槽 1：不出码，直接连第二个身份
I (2193)  Identity: slot=1 device_id=02:09:6d:48:b0:ae …
I (3753)  Application: Activation done      （无 activation 告警）
I (19993) MQTT: Session ID: c137f82a        → listening -> speaking -> listening（对话成功）
```

**关键点：槽 0 与槽 1 的第二个 boot 起都不再出现 `activation` 告警**，说明凭据按槽复用成功，切换完全在设备端完成。

### 顺带确认的事实

- 该板 **eFuse USER_DATA 没有文本 serial**，`activation_version` 一直是 1 —— 即官方服务端**本来就接受 v1 激活**，serial/HMAC 这层对该板不存在。
- 槽位切换会重启（约 3 s 到可对话），用户接受。
- 未绑定的槽（如只切过去看看的槽 2）会一直停在激活码界面并轮询 `/activate`，期间出现过一次 `Ota: Failed to read HTTP status: Server disconnected` —— 轮询中的瞬时断连，自动重试，非故障。

### 上正式版前还缺的（当前是实验版）

1. 未使用槽位会一直轮询激活码 → 需要"mark unused / skip"或把 `count` 收敛到实际绑定数
2. 屏幕只能显示 `ID:N`（设备端拿不到 agent 名字，OTA 响应无该字段），想显示"工作/家里"得靠本地映射表
3. 切换入口只有 M 键 + MCP 工具，没有配网页/App 入口
4. 槽位上限 8（`kMaxCount`），NVS 空间够但没做容量核算

---

## 11. 正式版（身份选择浮层）交付与两个 bug 复盘

### 交付形态

| 项 | 实现 |
|---|---|
| 入口 | L 键长按 3 秒 → 全屏 LVGL 浮层（`lv_obj_create(lv_screen_active())`，暗底 90% 不透明，垂直居中 flex） |
| 列表 | 恒定 6 行：标题 + 4 槽 + 提示行；`LV_LABEL_LONG_DOT` 单行不换行；高度核算 `6×21 + 5×4 + 8 = 154px < 170px` |
| 键位 | 菜单内 F/B 移动（不循环）、M 确认、L 取消、10s 超时；菜单外 F/B 音量、R 亮度/配网、L 主题、M 对话/长按打断播报 |
| 槽名 | NVS `identity/name<N>`，默认 情绪聊天/顾问咨询/思考伙伴/自定义 |
| 防误激活 | 浏览不写 NVS；未配置槽需二次确认；已配置槽提示「换绑需后台删设备后重启」 |

### bug 1：菜单渲染走了聊天通道（我给的规格错）

第一版让下属用 `display->SetChatMessage("system", ...)` 渲染菜单。`SetChatMessage` 是把气泡 widget 追加到 `content_` 容器（`lcd_display.cc:542+`），天生贴在屏幕底部且有高度上限 → 菜单被裁切、屏幕中央仍是表情图，用户只能摸黑按键。

**教训：菜单/对话框这类"覆盖式 UI"不能用聊天消息通道渲染。** 这块板是 LVGL，正解是自建浮层 + `DisplayLockGuard`，子对象自动继承 `SetupUI()` 设在 screen 上的主题字体（`lcd_display.cc:404`），中文无需额外字体工作。

### bug 2：已配置槽分支忘了落盘槽位（最隐蔽）

```cpp
// 未配置分支：写了 NVS
if (!Identity::SelectSlot(slot)) return;
RebootSoon(300);

// 已配置分支（最初）：只弹提示 + 重启，槽位从未写入
display->ShowNotification("切换到 " + name);
RebootSoon(800);
```

症状：**选任何已配置槽 → 重启后仍是原槽**；连带"后台删了设备重启却不出码"（因为根本没切到那个身份）。

**定位方法（可复用）：`SelectSlot()` 里有 `ESP_LOGI("Identity slot selected: %d")`，日志里连续 4 次 `Application: Rebooting...` 却一次都没有这行 → 说明"重启发生了但槽位没写"**，一眼锁定分支漏调用。

**教训：凡是"改变持久状态 + 重启"的路径，必须在两条（或以上）分支上都留一条能区分"写没写"的日志，并且重启前先落盘。**

### 真机验证（2026-10-28，全部通过）

```
slot=0 情绪聊天 → Alert 激活设备 → Ota: Activation successful(22.5s) → Session 72a2d465   ← 后台删设备后重新绑定
slot selected: 2 → slot=2 思考伙伴 → Alert 激活 → Activation successful → Session 53deb4fc ← 新槽绑定
slot selected: 0 → slot=0 → Activation done（无 Alert）→ Session 93dfbcab                  ← 来回切换不出码
slot selected: 1 → slot=1 顾问咨询 → Activation successful → Sessions 7bac4753/3f2219e4/b6d9432d
E 行：仅 1 条 uac-host 枚举瞬态（已知良性）
```

### bug 3：MCP 身份工具「模型说改好了，其实没改」

现象：对设备说「把第 4 个槽改名」，小智回「改好了」，但设备 UI 没变化。

排查卡住的点：**代码里没有任何一条成功路径的日志**，所以当时无法区分「模型根本没调用」和「调用了并成功」。先补观测再判断，是唯一正确的动作。

修完日志后一次就定位到两个独立缺陷：

1. **`identity.*` 原本是 `AddUserOnlyTool`**：`tools/list` 只在客户端带 `with_user_tools=1` 时列出这类工具（`mcp_server.cc:602`），是否下发由云端决定 → 模型可能压根不知道有这个工具。改成普通 `AddTool`（D 设备私人使用、无声纹门禁，用户明确接受该风险）。
2. **编号错位**：内部 0..3，而人对模型说「第 4 个」→ `slot=4` 被 `slot >= count` 直接拒绝。改成对外 1..4（内部 −1），`list` 输出 1-based，**菜单行也加序号** `1.情绪聊天`，让人和模型使用同一套编号。

修好后的真机证据：

```
I (37078) MCP: tools/call: id=3 name=self.identity.rename args={"slot":4,"name":"工作"}
I (37078) Identity: Identity slot 3 renamed to 工作
I (58998) MCP: tools/call: id=3 name=self.identity.rename args={"slot":4,"name":"灵感胶囊"}
I (59008) Identity: Identity slot 3 renamed to 灵感胶囊
```

**教训（三条，写进技能）：**
- 模型自述 ≠ 结果。凡是"设备会执行某个动作"的能力，**必须在执行入口打一条带参数的日志**，否则永远无法对账。
- 面向人的编号和面向机器的编号必须统一。对 LLM 暴露的 `slot` 用 1-based，屏幕上的行也标 1..4，否则"第 4 个"必然错位。
- `AddUserOnlyTool` 不等于"模型可见"：它取决于云端是否请求 user tools。给对话助手用的工具就该用 `AddTool`。
