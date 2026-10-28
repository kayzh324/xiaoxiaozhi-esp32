# 上游关系与同步说明

本仓库是 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的**个人 fork**，在官方固件上叠加了自建板级、UAC 耳机支持和多身份（一机多人设）能力。

## 三个仓库的关系

| 角色 | 仓库 | 用途 |
|---|---|---|
| 官方上游 | `78/xiaozhi-esp32`（remote 名 `upstream`） | 只读。**推送已被禁用**（`upstream` 的 push URL 置为 `DISABLED`），避免误推 |
| 我的 fork | `kayzh324/xiaoxiaozhi-esp32`（remote 名 `origin`） | 我的工作仓库，从这里 clone / 推送 |
| 本地工作副本 | `~/GitHubProject/xiaozhi-esp32` | 开发目录（目录名保留旧名，不影响仓库名） |

## 分支策略

| 分支 | 含义 | 规则 |
|---|---|---|
| `main` | **上游镜像** | 只做 fast-forward 到 `upstream/main`，不放自己的提交 |
| `feat/identity-switch` | 功能分支（多身份轮换 + 板级 + UAC） | 自己的提交都在这里；每次同步上游后在这里合并 |

## 已跟踪的上游版本

| 项 | 值 |
|---|---|
| 跟踪对象 | `upstream/main` |
| 当前跟踪到 | `c435f99` — `say how many tools were registered and how many the model can see (#2366)`(2026-10-26) |
| 对应固件版本 | `PROJECT_VER = 2.5.3`（`CMakeLists.txt`） |
| 最近一次上游同步 | 2026-10-28，把 `c435f99` 合入 `feat/identity-switch`，合并后编译通过 |
| 上一次同步 | 2026-10-25，把 `03a7c65`(2.5.3) 合入 `main`，合并提交 `66ec74b`（当时跨 126 个上游提交） |
| 更早的合并基线 | `4f7c00f`(2026-10-08) |

同步前先跑下面「查看落后多少」那组命令确认，再看是否需要。

## 日常同步流程

```bash
cd ~/GitHubProject/xiaozhi-esp32

# 1) 拉上游（只读）
git fetch upstream main --tags

# 2) 看落后多少、影响面
git rev-list --count feat/identity-switch..upstream/main
git log --oneline feat/identity-switch..upstream/main | head -20
git diff --stat feat/identity-switch...upstream/main

# 3) 让 main 保持上游镜像
git checkout main && git reset --hard upstream/main

# 4) 把上游合进功能分支（冲突在这里解）
git checkout feat/identity-switch
git merge upstream/main

# 5) 验证：编译必须过
~/.hermes/profiles/tech/cache/scratch/build61.sh "kayzh-ml307-headphone" "" zh-CN nihaoxiaozhi
# 或等价命令：
eim run "python scripts/build.py kayzh-ml307-headphone --language zh-CN --wake-word nihaoxiaozhi" v6.1

# 6) 推送
git push origin main
git push origin feat/identity-switch
```

## 我们在上游之上叠加了什么（同步时要盯住的地方）

| 改动 | 位置 | 说明 |
|---|---|---|
| 多身份轮换 | `main/identity.{h,cc}`、`main/ota.cc`、`main/mcp_server.cc`、`main/system_info.cc`、`main/boards/common/board.cc`、`main/boards/kayzh-ml307-headphone/kayzh_ml307_headphone.cc` | 4 个固定槽位、`02:` 前缀合成 MAC、凭据按槽隔离 `mqtt<N>`/`websocket<N>`、L 键长按选择界面、`self.identity.*` MCP 工具 |
| UAC 耳机支持 | `main/audio/codecs/ab13x_audio_codec.{h,cc}`、`main/audio/audio_codec.h`、`main/audio/audio_service.*`、`main/boards/kayzh-*`、`main/boards/lckfb/szpi-esp32s3/*` | AB13X USB 声卡，主用板 `kayzh-ml307-headphone` |
| 自建板级 | `main/boards/kayzh-ml307-headphone/`、`main/boards/kayzh-robot-mak/`、`CMakeLists.txt`、`Kconfig.projbuild` | 板级映射与 Kconfig 项 |
| 依赖版本 | `main/idf_component.yml` | 保持上游版本，**不要再手动降级**（曾误降 `esp-ml307`/`uart-uhci`/`uart-eth-modem`，实测不需要） |

上游最容易和我们冲突的文件（同步时优先看）：`main/mcp_server.cc`、`main/ota.cc`、`main/application.cc`、`main/CMakeLists.txt`、`main/Kconfig.projbuild`。

## 已知的上游陷阱（踩过的）

- **`AddUserOnlyTool` 不等于"模型可见"**：`tools/list` 只在客户端传 `with_user_tools=1` 时列出它们；而且服务端对工具数量有上限（见过 `The number of tools has reached the limit: 32`，超出的被静默丢弃）。上游 `c435f99` 为此加了 `LogToolInventory()`，开机日志会打 `N tools registered: X visible to the model, Y user-only` —— 加工具后先看这行。
- **IDF 用 `eim run`**：`source export.sh` / EIM 的 `activate_idf_*.sh` 在非交互 shell 里会失败，`idf_tools.py export` 在本机会误报 `esp-rom-elfs has no installed versions`。
- **上游要求 ESP-IDF ≥ 6.0.1，推荐 `release/v6.1` 分支**（`v6.1` tag 有 ESP32-C3 Wi-Fi 初始化问题，见上游 issue #2344）。本机用 EIM 装的 v6.1。
- 本板 eFuse 无文本 serial，因此一直走 **Activation v1**（无 serial/HMAC），这也是多身份方案能work的前提。

## 设计文档

- `docs/analysis/device-binding-identity.md`：身份链分析、方案对比、真机验证记录、4 个 bug 复盘。
