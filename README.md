# 双模式板卡：Thread Border Router / Zigbee Gateway（BOOT 键切换）

一块 **ESP Thread Border Router / Zigbee Gateway Board v1.2**（ESP32-S3 + ESP32-H2），
用 **S3 的 BOOT 键**在上电启动时选择本次运行模式，并由 S3 **经板内 UART 间接刷写 H2**，
使同一块板子在「Thread USB RCP」与「Zigbee UART RCP 网关」两种角色之间切换，
无需任何外部烧录器。

本文件是本工程的**需求与验收指标**。实现可以改，本文件里的指标不能悄悄放宽。

---

## 1. 目标

| 模式 | H2 角色 | S3 角色 | 对外接口 |
| --- | --- | --- | --- |
| **Thread 模式** | USB RCP（spinel/HDLC over USB-Serial-JTAG） | 引导程序空转常驻（不参与无线电，保持在线以便按 RESET 重入选择窗口） | HAOS 的 OpenThread Border Router add-on 直连 H2 的 `/dev/ttyACM*` |
| **Zigbee 模式** | UART RCP（spinel over 板内 UART @460800） | Zigbee 网关 + MQTT 桥（Wi-Fi 回程） | MQTT → HAOS MQTT 集成，自动发现 `light` 实体 |

**非目标**（已论证不可行，不再尝试）：

- 两种协议**同时**运行（H2 只有一颗 802.15.4 射频，且只能被一个主机协议栈独占）
- 把 Zigbee 做成 USB 协调器交给 ZHA / Z2M（Linux 无 ESP 主机栈；ESP 的 `ezbee` 只给 ESP32/FreeRTOS）
- S3 的 Wi-Fi 经 USB 变成 HAOS 网卡（`esp-hosted` 的 Linux 主机只支持 SDIO/SPI，本板未引出）
- 运行时热切换（切换必然重启，且伴随 H2 重刷）

---

## 2. 硬件事实

| 项 | 值 |
| --- | --- |
| 主机 SoC | ESP32-S3-WROOM-1，**8 MB flash**，2 MB PSRAM |
| 无线电 SoC | ESP32-H2-MINI-1，**2 MB flash**，32 MHz 晶振 |
| S3 ↔ H2 | UART @460800（S3 GPIO17=RX / GPIO18=TX ↔ H2 GPIO24/23） |
| RCP 控制线 | RESET = S3 GPIO7，BOOT = S3 GPIO8 ↔ H2 GPIO9 |
| USB | 两个 USB-C：**USB1 → H2**、**USB2 → S3**，均为片内 USB-Serial-JTAG（`/dev/ttyACM*`，**不是** USB-UART 桥） |
| 按键 | S3 独立 BOOT(GPIO0) / RESET |
| ESP-IDF | v5.5.4（esp-thread-br 与 esp-zigbee-sdk v2 共同推荐） |

⚠ `ttyACM` 编号会变化。
一切脚本只能走 `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`。

---

## 3. 功能需求

### R1 — BOOT 键切换模式

| 参数 | 规格 |
| --- | --- |
| 开始采样 | app 启动后 **1 s** 才开始采样；0 ~ 1 s 内的按下**不计入** |
| 监听窗口 | **5 s**（app 启动后 1 s ~ 6 s） |
| 触发条件 | 窗口内 BOOT(GPIO0，按下为低) **连续保持低 ≥ 2 s** → 切换到**另一个**模式 |
| 未触发 | 保持当前模式，正常启动 |
| 生效方式 | 立即执行切换流程（刷 H2 + 切 app 分区 + 重启），**不**支持热切换 |
| 窗口结束后 | **不再响应按键**；要重选模式就按 S3 的 RESET 键重走一遍 1 s ~ 6 s 窗口 |
| 状态持久化 | 当前模式存 NVS，掉电保持 |

窗口在 app 启动后计时，因此：**刚上电时不要按住 BOOT**——上电瞬间 GPIO0 为低会让 S3 进 ROM 下载模式，
我们的代码根本不会跑。操作约定是「上电/复位时松开，板子起来 1 s 之后再长按 2 s」。

**自激不处理**：切换动作执行完毕后应松开按键。若一直按着，重启后的窗口内会再次满足
「≥ 2 s 低电平」而再切一次，表现为模式来回反复；这是已知且接受的行为，不做额外保护。

### R2 — Thread 模式：H2 烧 USB RCP

- H2 固件 = `ot_rcp` 变体，`CONFIG_OPENTHREAD_RCP_USB_SERIAL_JTAG=y`，spinel 走片内 USB。
- **由 S3 间接烧录**，不使用外部烧录器：S3 用 GPIO7(RESET) / GPIO8(BOOT) 让 H2 进下载模式，经 UART 写入。
- 烧录完成后 H2 以 USB RCP 身份对 HAOS 呈现串口，HAOS 的 OTBR add-on 接管协议栈；
  **S3 不参与任何无线电工作**，也不占用 UART0（否则无法再次刷 H2）。

### R3 — Zigbee 模式：H2 烧 UART RCP

- H2 固件 = `ot_rcp` 变体，`CONFIG_OPENTHREAD_RCP_UART=y`（默认 UART RCP）。
- **同样由 S3 间接烧录**（同一套 GPIO 与 UART 通路）。
- S3 运行 Zigbee 网关固件（基于 `esp_zigbee_sdk` 的 `zigbee_gateway` 例程改造），
  经板内 UART 与 H2 建立 spinel 通信，Wi-Fi 回程，并通过 MQTT 把子设备暴露给 HAOS（R6、R7）。

### R4 — S3 存储布局

S3 内需容纳 3 类内容：

| # | 内容 | 分区 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | **引导程序** | `factory` (app) | 1.5 MB | 检测 BOOT 键、决定本次模式、驱动 H2 烧录；Thread 模式下它就是空转常驻的 app |
| 2 | **Zigbee 网关 app** | `zb_app` (app/ota_0) | 2 MB | Zigbee 模式下 S3 作为 UART 主机控制 H2 |
| 3 | **两份 H2 固件** | `rcp_fw` (data/spiffs) | 1 MB | 同一分区下的两个目录：`/rcp_fw/rcp_0/`（Thread USB，≈195 KB）、`/rcp_fw/rcp_1/`（Zigbee UART，≈207 KB） |

两份 H2 镜像**不是两个独立分区**，而是同一个 SPIFFS 分区里的两个镜像目录——这是官方
`esp_rcp_update` 的原生形态（见 §7）。刷写失败的回退由 §4 的「重试最多 3 次后停在当前模式」负责，
不使用组件的 seq 轮换回退（本工程把 seq 当作模式标志用，verified 恒置位）。

系统必需分区 `nvs` / `otadata` / `phy_init` 必须有：`otadata` 是 `factory ↔ ota_0` 之间切 app 的唯一手段。

**不保留 OTA 升级能力**：没有 `ota_1`，app 分区只有 `factory` 与 `ota_0` 两个。

分区表（S3 8 MB，分区表放 `0x8000`）：

```
# Name,     Type, SubType, Offset,    Size
nvs,        data, nvs,     0x9000,    0x5000
otadata,    data, ota,     0xE000,    0x2000
phy_init,   data, phy,     0x10000,   0x1000
factory,    app,  factory, 0x20000,   0x180000
zb_app,     app,  ota_0,   0x1A0000,  0x200000
rcp_fw,     data, spiffs,  0x3A0000,  0x100000
```

`rcp_fw` 里是 SPIFFS 文件系统，两份镜像以目录区分，不是两个分区。

尾部 `0x4A0000 ~ 0x800000` 约 3.4 MB 未分配。

### R5 — SoftAP 首次配网

| 场景 | 行为 |
| --- | --- |
| NVS 无 Wi-Fi 凭据 | 直接开热点（不做无意义重试） |
| 有凭据，连接成功 | 连上后进入正常流程 |
| 有凭据，连不上 | 重试 10 次 / 20 s 超时 → 转 SoftAP |
| 配网模式 | 起 HTTP 服务，**不进 Zigbee 栈**、不 panic、不重启循环 |
| 提交表单 | 写 NVS → 1.5 s 后重启 |

- 热点名 `ESP-ZB-GW-<MAC 后 3 字节>`（开放网络），浏览器打开 `http://192.168.4.1/`。
- 表单字段：Wi-Fi SSID、密码、**MQTT 地址**（可留空，留空则沿用已存值）。
- **STA 模式也常驻配置页**（`http://<设备IP>/`），用来只改 MQTT 而不必重新配网。
- 仅支持 **2.4 GHz**；路由器 2.4G/5G 同名时首轮 auth 可能被拒，靠重试自动上去。

### R6 — Zigbee 子设备管理

| 环节 | 实现 |
| --- | --- |
| 发现 | `EZB_ZDO_SIGNAL_DEVICE_ANNCE` → `ezb_zdo_active_ep_req` → 逐端点 `ezb_zdo_simple_desc_req` |
| 设备表 | 以 **IEEE 64-bit** 为键（不是短地址），最多 16 台；短地址变化时按 IEEE 匹配更新 |
| 能力判定 | 只看端点 cluster：`0x0006 OnOff` / `0x0008 Level` / `0x0300 ColorControl`；颜色是否支持 XY / CT 由**读属性实测**决定 |
| 绑定 | 对 OnOff / Level / ColorControl 建 Binding（源 = 设备 IEEE+EP，目的 = 本机 IEEE + EP10） |
| 上报配置 | `Configure Reporting`：OnOff、Level、ColorX、ColorY、ColorTempMireds、ColorMode |
| 状态读取 | 读 OnOff `0x0000`、Level `0x0000`、Color `0x0003/0x0004/0x0007/0x0008/0x400b/0x400c` |
| 探活 | 网络就绪 5 s 后 + 每 60 s；8 s 无响应标记离线 |
| 持久化 | NVS `zigbee`/`devs`（带 version）；只存 IEEE/short/ep/caps，**状态不持久化** |

**设备类型范围（当前）**：只映射成 HA 的 `light`。
任何带 OnOff 的 ZCL 设备都能被控制，但 discovery 一律按 `light` 发布；
传感器类设备（无 OnOff/Level/Color）会被发现并记入设备表，但**不发布任何实体**。

### R7 — MQTT 桥

| 项 | 约定 |
| --- | --- |
| broker | 配网页填写的 URI，存 NVS `wifi_cfg`/`mqtt_uri`；未配置则跳过 MQTT |
| Discovery | `homeassistant/light/zbgw_<ieee>/config`（retained） |
| 状态 | `zbgw/light/<ieee>/state`（retained，JSON） |
| 命令 | `zbgw/light/<ieee>/set`（JSON） |
| 可用性 | `zbgw/light/<ieee>/avail`（`online`/`offline`）；网关级 LWT 为 `zbgw/status` |
| schema | `json`；`brightness_scale: 254`（直接对齐 ZCL CurrentLevel） |
| 能力声明 | `supported_color_modes` 按实测能力生成：`["color_temp","xy"]` / `["brightness"]` / `["onoff"]` … |
| 色温范围 | `min_mireds`/`max_mireds` 取自设备 `0x400b`/`0x400c`，读不到才用 153/500 兜底 |
| birth | 订阅 `homeassistant/status`，收到后重发全部 Discovery |

命令与状态示例：

```json
/* set */    {"state":"ON"}
/* set */    {"brightness":128}          /* 0-254 */
/* set */    {"color":{"x":0.42,"y":0.35}}
/* set */    {"color_temp":300}          /* mireds */
/* state */  {"state":"ON","brightness":128,"color_mode":"xy","color":{"x":0.42,"y":0.35}}
```

---

## 4. 启动与切换流程

```
上电/复位
   └─ bootloader 按 otadata 选 app
        ├─ factory（引导程序，Thread 模式下即终态）
        │    ├─ 等 1 s → 5 s 窗口内长按 BOOT ≥ 2 s ?
        │    ├─ 否 → 目标模式 = NVS 里的当前模式
        │    │    ├─ Thread → 版本比对，一致则跳过刷写；不一致则刷 rcp_0（USB 变体）
        │    │    │           → 空转常驻（不 deep sleep）
        │    │    └─ Zigbee → 版本比对，不一致则刷 rcp_1（UART 变体）
        │    │                → set_boot_partition(zb_app) → 重启
        │    └─ 是 → 目标模式 = 另一模式 → 写 NVS → 刷对应镜像 → 切 app 分区 → 重启
        └─ zb_app（Zigbee 网关）
             ├─ 等 1 s → 5 s 窗口内长按 BOOT ≥ 2 s ?（与引导程序同规格，共享组件）
             ├─ 否 → 版本比对，不一致则刷 rcp_1 → 起网关（Wi-Fi + Zigbee 栈）
             └─ 是 → 目标模式 = Thread → 写 NVS → 刷 rcp_0 → set_boot_partition(factory) → 重启
```

要点：

- **按钮逻辑必须两处行为一致**，否则切换链路会断（共享组件 `mode_select`）。
- 判断「S3 现在是哪个模式」只看 NVS 里的模式字段，不看 app 自己是哪个；两者不一致说明上次切换中断，
  按 NVS 重新刷 H2 并切 app。
- 刷 H2 是幂等动作：镜像版本字符串一致时跳过，避免每次上电白刷 5~10 s（见 §7）。
- 「先刷 H2，再切 app 分区，最后重启」的顺序不能反：刷写失败时保持当前 app 与当前模式，
  下次上电重试，最多 3 次后停在当前模式（不进死循环）。

---

## 5. S3 侧工程

| 工程 | 来源 | 作用 |
| --- | --- | --- |
| `firmware/s3_boot` | 自研（ESP-IDF 基础模板） | R4-#1 引导程序 |
| `firmware/s3_zigbee_gateway` | `esp-zigbee-sdk` v2 `examples/zigbee_gateway` 改造 | R4-#2 Zigbee 网关 |
| `components/mode_select` | 自研 | 按钮窗口检测（1 s 起采、5 s 窗、长按 ≥2 s）+ 模式状态 + app 分区切换，两个 app 共用 |

`firmware/s3_zigbee_gateway/main/` 模块划分：

| 文件 | 职责 |
| --- | --- |
| `zigbee_gateway.c` | 例程主体：Zigbee 栈启动、信号处理、ZCL action 回调分派 |
| `wifi_prov.c` | R5 配网：NVS 凭据、SoftAP、HTTP 表单、运行期配置页 |
| `zb_dev.c` | R6 设备表、发现流程、状态缓存、探活、属性回报处理 |
| `zb_ctrl.c` | 命令下发（OnOff / Level / Color）、Binding、Configure Reporting |
| `mqtt_bridge.c` | R7 MQTT 连接、Discovery、命令解析、状态发布 |

脚本（`script/<命令>/<命令>.sh`，每个命令一个目录）：

| 命令 | 用途 |
| --- | --- |
| `script/probe/probe.sh` | `env` / `ports` / `list`：by-id 解析 H2、S3 端口 |
| `script/build-h2/build-h2.sh` | 编译 H2 的 `thread_usb` / `zigbee_uart` 变体 |
| `script/build-s3/build-s3.sh` | 编译 Zigbee 网关（目标 `ota_0`） |
| `script/build-boot/build-boot.sh` | 编译引导程序（目标 `factory`） |
| `script/pack-rcp/pack-rcp.sh` | 打包两份 H2 镜像为 SPIFFS 镜像 `out/rcp_fw.bin` |
| `script/flash-s3/flash-s3.sh` | 烧写 S3 全部分区（`--erase` 先整片擦除） |
| `script/verify-thread/verify-thread.sh` | 用官方 otbr-agent 验证 Thread 模式 USB RCP |
| `script/gen-cpp-index/gen-cpp-index.sh` | 生成 `.vscode/c_cpp_properties.json`（每个工程一个 configuration） |

**禁止在 `$IDF_PATH/examples/` 下直接编译**：例程一律拷贝到本仓库 `firmware/` 内再构建，
IDF 仓库保持只读参考。

---

## 6. H2 侧工程

| 工程 | 来源 | 变体 |
| --- | --- | --- |
| `firmware/h2_rcp` | `$IDF_PATH/examples/openthread/ot_rcp` | `sdkconfig.defaults.thread_usb` / `sdkconfig.defaults.zigbee_uart` |

- 两个变体**必须用彼此独立的 `SDKCONFIG`**：
  `idf.py -B build_usb -D SDKCONFIG=$PWD/build_usb/sdkconfig`（`export SDKCONFIG=` 不生效）。
- 体积控制在 ≤512 KB（`LOG_DEFAULT_LEVEL_NONE` + size 优化），实测 195 KB / 207 KB。
- H2 只有 2 MB flash，**不存在**「在 H2 上放两套固件切分区」的方案，切换只能由 S3 重刷。

---

## 7. 例程与依赖清单（拉取范围）

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| ESP-IDF | v5.5.4 | 全部构建 |
| `esp-thread-br` | 与 IDF v5.5 配套 | `esp_rcp_update` 组件（S3 刷 H2 的核心） |
| `esp-zigbee-sdk` | v2（git 子模块/`EXTRA_COMPONENT_DIRS`） | S3 Zigbee 网关 |
| `esp-serial-flasher` | `^1.8.0`（`esp_rcp_update` 依赖） | 实际执行 UART 烧录 |

`esp_rcp_update`（v1.7.0）的镜像机制——已按组件源码核实：

- 镜像文件路径固定为 `<firmware_dir>_<seq>/rcp_image`，`seq` ∈ {0,1}。本工程两份镜像分别为
  `/rcp_fw/rcp_0/rcp_image`（Thread USB）与 `/rcp_fw/rcp_1/rcp_image`（Zigbee UART）。
- `seq` 由组件从 NVS 读取：namespace `storage`、key `rcp_seq`（int8，bit5 为 verified 标志）。
  **本工程直接把 `seq` 当作模式标志**：Thread = 0，Zigbee = 1，写入时置 verified 位；
  刷写前先写 `seq`，再调 `esp_rcp_update_init()` / `esp_rcp_update()`。
- 版本比对：`esp_rcp_load_version_in_storage()` 读**当前 seq 目录**镜像内嵌的版本子文件，
  与 NVS 记录的「H2 已刷入模式 + 已刷入版本」比对，两者都一致才跳过刷写。
- `firmware_dir` 是 `char[20]`，写死 `"/rcp_fw/rcp"`（组件自己拼 `_<seq>` 后缀，不接受完整文件路径）。
- `CONFIG_AUTO_UPDATE_RCP=n`：关闭组件的启动自刷，流程完全由引导程序显式控制。
- 镜像文件由组件自带的 `create_ota_image.py` 从 H2 的 `build_*/` 产物打包生成（单文件 `rcp_image`，
  内含 flash args 与版本子文件），由构建脚本写入 SPIFFS 镜像。

---

## 8. 验收标准

状态标记：✅ 已实测通过 ｜ ⚠ 部分通过 ｜ ⬜ 未验证

| 编号 | 验收项 | 判据 | 状态 |
| --- | --- | --- | --- |
| A1 | 纯上电不按 BOOT | 3 次上电，模式保持不变，H2 固件与模式匹配 | ✅ |
| A2 | 长按切换 | 窗口内长按 BOOT 2 s，Thread→Zigbee→Thread 各 5 次，每次重启后模式正确、H2 固件正确 | ✅ |
| A3 | 间接烧录 | 全程不使用 esptool 直连 H2 烧固件（Thread 模式下 USB1 仅作为 HAOS 的数据口），H2 仍能复位并升级 | ✅ |
| A4 | Thread 闭环 | HAOS OTBR add-on 日志出现 `RCP_HOS` + `Set state callback: OK` + `BorderAgent Registering service` | ⚠ 仅验证到 otbr-agent 侧，HAOS 的 OTBR add-on 未验证 |
| A5 | Zigbee 闭环 | S3 日志 `ESP_RADIO_SPINEL ... enabled`、RCP 版本为 `esp32h2`、Sta IP 获取成功、Zigbee 网络形成 | ✅ |
| A6 | 刷写失败可恢复 | 切换途中拔电/超时，下次上电自动重试，不死循环、不变砖 | ⬜ |
| A7 | 稳定性 | 反复上电 20 次 + 切换 20 次，无卡死模式 | ⬜ |
| A8 | 窗口外不响应 | 启动 10 s 后长按 BOOT 5 s，模式不变 | ⬜ |
| A9 | 版本比对生效 | 同一模式连续上电 3 次，第 2、3 次串口日志无 H2 刷写记录 | ✅ |
| A10 | SoftAP 配网 | 无凭据时热点可达、表单可提交、重启后连上 | ✅ |
| A11 | 子设备发现 | 入网后打印端点与簇列表，颜色能力由属性探测得出 | ✅ |
| A12 | 灯控制 | HA 里开关 / 亮度 / 色温 / 颜色四项均生效 | ✅ |
| A13 | MQTT 发现 | HA 自动出现 `light` 实体且状态可用 | ✅ |
| A14 | Binding 主动上报 | 用手机 App 或墙壁开关改灯，网关 1~2 s 内收到 `REPORT_ATTR` | ⬜ |
| A15 | 换设备通用性 | 换另一台同类 ZCL 灯，无需改代码即可被识别与控制 | ⬜ |

---

## 9. 定稿决策

| 编号 | 议题 | 定稿 |
| --- | --- | --- |
| D1 | 两份 H2 固件的存放形态 | **单个 SPIFFS 分区** `rcp_fw`，内含 `/rcp_fw/rcp_0/`、`/rcp_fw/rcp_1/` 两个镜像目录，直接用官方 `esp_rcp_update` |
| D2 | 「点击」的定义 | **长按 ≥ 2 s**（窗口内连续低电平），不是短按 |
| D3 | 1 s 延迟的语义 | 启动后 **1 s 才开始采样**，1 s 内的按下**不计入** |
| D4 | 切换后按键未松开的自激 | **不处理**，写进操作说明要求松手；自激行为已接受（见 R1） |
| D5 | Thread 模式下 S3 做什么 | **空转常驻**，不进 deep sleep；要重选模式按 S3 RESET 重入窗口 |
| D6 | 每次上电是否重刷 H2 | **比对 RCP 镜像版本字符串 + 已刷入模式**，一致则跳过，不一致才刷 |
| D7 | Zigbee 侧范围 | **含 MQTT 桥 + HA Discovery** |
| D8 | 是否保留 OTA | **不保留**，不设 `ota_1`；app 分区只有 `factory` 与 `ota_0` |
| D9 | 颜色模式（XY/CT）如何判定 | **通用规则**：只看本轮**值真的变化**的颜色属性——只有 `CurrentX/Y` 变→XY，只有 `ColorTempMireds` 变→CT，都变或都没变则保持。**不用 `ColorMode` 属性判断模式**，仅在它与推导值不一致时告警 |
| D10 | MQTT 暴露哪些设备类型 | **只做 `light`**。带 OnOff 的 ZCL 设备都能控制，但一律按 light 发布；新增 `switch`/`sensor` 属后续扩展 |
| D11 | 设备不合规怎么处理 | **告警，不改逻辑**。不做设备型号特化分支，不用"因为某厂商会这样"而改变通用规则 |


---

## 10. 约束与已知坑

1. **上电瞬间 GPIO0 为低 → S3 进 ROM 下载模式**，我们的代码不运行。文档与操作步骤必须写明「起来 1 s 后长按 2 s」。
2. **`ttyACM` 编号会变**，一律走 `/dev/serial/by-id/`。
3. **USB RCP 与 OpenThread console 抢 USB 通道**：启用 USB RCP 时 console 不能占 USB，次要 console 需设 `USB_SERIAL_JTAG`。
4. **Thread 模式下 H2 的 UART0 必须空闲**（不给它开日志），否则 S3 无法再刷 H2。
5. **`CONFIG_AUTO_UPDATE_RCP=n` 时，Kconfig 里 `RCP_SRC_DIR` / `RCP_PARTITION_NAME` / `RCP_PATH_NAME` 都不可见**
   （它们 `depends on AUTO_UPDATE_RCP || CREATE_OTA_IMAGE_WITH_RCP_FW`）。镜像打包与目录名一律在构建脚本和代码里显式指定，不要从 Kconfig 取值。
6. **H2 只有 2 MB flash**，`ot_rcp` factory 分区就是 1 MB，别再往 H2 上加东西。
7. **不要自己实现 spinel 做验证**：TID 在 header 的 bit4-6，RESET 应答是不带 response 位的 `PROP_VALUE_IS(112)`；
   验证一律用官方 `otbr-agent`。
8. **`openthread/otbr` 容器默认 entrypoint 很慢**，验证时用 `--entrypoint bash` 直调 `/usr/sbin/otbr-agent`，`-B` 用真实网卡名。
9. **切换会造成网络短时中断**，HA 内实体短暂 unavailable 属预期。
10. **IDF 的 `export.sh` 在 `set -u` 下会中断调用方**，脚本里 `source` 前需 `set +u`、事后 `set -u`。
11. **SPIFFS 分区必须一次写入两份镜像**：切到缺镜像的模式会直接 `ESP_ERR_NOT_FOUND`，表现为该模式永远起不来。
12. **每次切换都写一次 `otadata`**（`esp_ota_set_boot_partition`），是切 app 的必付代价；切换频率低，磨损可忽略，但不要在循环里反复调。
13. **只配 Configure Reporting 不够，必须先建 Binding**：ZCL 的 `Report Attributes` 是经 binding 表投递的，
    没有 binding 时设备只会"看心情"发给最后配置它的一方，表现为上报时有时无。
14. **`ColorMode (0x0300/0x0008)` 在实践中不可靠**：多数灯具在 `MoveToColor` 后不同步该属性；
    部分设备还会以 `0x89 INSUFFICIENT_SPACE` 拒绝为它配置上报。因此模式判定不用它（D9），只做一致性告警。
15. **探活会反复读属性，写 NVS 必须按"变化"判断**：无条件 `dev_save()` 会一天写上千次 flash。
16. **子设备不会因为网关重启而重新 Announce**：协调器沿用 NVS 里的网络参数，设备认为网络没变。
    网关重启后必须靠**主动探活**恢复，不能等 `DEVICE_ANNCE`。
17. **短地址会变**：设备 rejoin 后短地址可能变化，设备表必须用 IEEE 作键，收到 annce 时按 IEEE 更新短地址。
18. **状态采用"命令即状态 + 读取覆盖"**：命令下发成功即更新本地并推 MQTT（否则延迟应答设备会让 HA 卡住），
    之后由上报/探活覆盖。`MoveToLevelWithOnOff` 会开灯，所以设亮度时同时置 `on=true`。
19. **多端点设备目前只取第一个带 OnOff 的端点**（`dev->ep` 只存一个），多路开关/多路灯只会控制第一路。
20. **设备表上限 16 台**，单网关。
21. **MQTT URI 里的特殊字符必须 URL 编码**：密码含 `@ : / #` 时写成 `%40 %3A %2F %23`，
    否则 `mqtt://user:pass@host:port` 会被错误拆分。
22. **HA 实体依赖 `unique_id`**：没有它则 HA 重启后不会恢复实体；Discovery 必须 retained 或响应 birth（`homeassistant/status`）。

---

## 11. 参考

- esp-thread-br：<https://docs.espressif.com/projects/esp-thread-br/>
- `esp_rcp_update` API：<https://docs.espressif.com/projects/esp-thread-br/en/latest/api-reference/esp_rcp_update.html>
- RCP 更新机制：<https://docs.espressif.com/projects/esp-thread-br/en/latest/dev-guide/rcp_update.html>
- esp-zigbee-sdk：<https://github.com/espressif/esp-zigbee-sdk>
- esp-zigbee-sdk API（ZDO / ZCL / 各 cluster）：<https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/api-reference/esp_zigbee_core/zcl/zcl_general_command.html>
- HA MQTT 集成与 Discovery：<https://www.home-assistant.io/integrations/mqtt/>
- HA MQTT Light（json schema）：<https://www.home-assistant.io/integrations/light.mqtt/>
