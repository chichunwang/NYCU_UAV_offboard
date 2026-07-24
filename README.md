# NYCU UAV Offboard Control

NYCU UAV 的第一代 ROS 2 板外控制與 LR24-F 地面指令系統。現行主線已從早期的 local-NED Offboard 方形範例，演進為「固定翼單點 GPS GOTO」流程：地面端透過 LR24-F 傳送命令，空中端檢查 PX4 遙測與飛行安全條件後，以 `DO_REPOSITION` 要求 PX4 飛向目標並進入 Auto Loiter。

> [!CAUTION]
> 這是飛航控制研究軟體，不是已完成認證的飛行產品。

## 目前狀態

「已實作」只表示 repository 內已有程式，不等於完成 SITL 或實機驗證。

| 功能 | 實作 | 自動測試 | 目前驗證狀態 |
|---|---|---|---|
| LR24 透明串列雙向鏈路測試 | 完成 | 無硬體自動測試 | 已完成近距雙向 30/30、0% loss 實測 |
| XOR checksum、ACK/ERR、sequence 去重 | 完成 | Python 已覆蓋 | `PING`／`HELP` 已通過實機鏈路 |
| 地面 CLI 與 Tk GUI | 完成 | Python 已覆蓋 | 基本通訊已驗證 |
| LR24 serial 斷線重連 | 完成 | 靜態檢查 | 待完整拔插與重啟驗收 |
| 單點 `GOTO`／`GOTO_AMSL` | 完成 | 尚無 C++ 行為測試 | DDS、SITL、實飛待驗證 |
| `RTL`／`ABORT` | 完成 | 尚無 C++ 行為測試 | SITL、實飛待驗證 |
| Raspberry Pi systemd 自動啟動 | 完成 | Python 靜態檢查 | 待實機驗收 |
| Legacy local-NED Offboard 方形範例 | 保留 | 尚無 C++ 行為測試 | 僅供 SITL／拆槳受控測試 |
| 多航點任務 | 未實作 | — | — |
| 雲台相機目標追蹤 | 未實作 | — | — |

實測紀錄與尚未完成項目請以 [LR24-F 快速測試與系統啟動實測手冊](docs/lr24_quick_test_guide.md) 為準。

## 安全邊界

現行 GPS GOTO 主線的 `global_goto_node`：

- 不會 arm、起飛或降落；飛機必須已由飛手安全起飛、armed 且確認離地。
- 只接受固定翼狀態，並檢查 GPS、Home、遙測新鮮度、RC、電池與 PX4 failsafe flags。
- 預設限制目標距目前位置及 Home 均不超過 `2000 m`，相對 Home 高度為 `30–120 m`。
- 以一次性 `VEHICLE_CMD_DO_REPOSITION` 將目標交給 PX4，不持續串流 Offboard setpoint。
- `RTL` 要求 PX4 進入 `AUTO_RTL`；`ABORT` 目前是 `RTL` 的別名，不是 Kill、Hold 或立即停止。
- 節點監控中止只會停止 ROS 端追蹤，不會自動取消 PX4 已接受的 GOTO。LR24、伴飛電腦或 ROS node 失聯時，PX4 可能仍繼續飛向既有目標。

目前尚未實作完整目標 geofence polygon、terrain／AGL 檢查、最大速度／轉彎率限制，以及通訊失聯後自動取消目標。實飛時必須保有有效的 RC／ELRS 人工接管方式，並以另一條 MAVLink 路徑讓 QGroundControl 監看飛機；LR24 serial 與 Pixhawk DDS serial 都不能同時被 QGC 占用。

舊版 `my_offboard_node` 不具備上述 GPS GOTO safety gate，且 `START_OFFBOARD` 會在 warmup 後要求切換 Offboard 並 arm。它只能用於 SITL、拆槳或其他已建立獨立安全措施的環境。

## 現行系統架構

### GPS GOTO 主流程

```text
地面端 Windows / Ubuntu
  send_lr24_command_ui.py 或 send_lr24_command.py
                │  $CMD,<seq>,...*<XOR>
                ▼
        地面 LR24-F  ⇄  空中 LR24-F
                           │ 透明 USB Serial
                           ▼
                  lr24_command_node
                    │ ROS 2 services
          ┌─────────┼──────────────────┐
          │         │                  │
     /goto_global   /gps_goto_status   /return_to_launch
          └─────────┼──────────────────┘
                    ▼
               global_goto_node
            │ DO_REPOSITION / RTL
            │ /fmu/in/vehicle_command
            ▼
       Micro XRCE-DDS Agent
            │ 獨立 Pixhawk Serial
            ▼
       Pixhawk / PX4 fixed-wing
            │ /fmu/out/* telemetry
            └──────────────────────────► global_goto_node safety gate
```

主線 launch 為 `serial_gps_goto.launch.py`，只啟動 `global_goto_node` 與 `lr24_command_node`。兩個 node 發生異常時會在 3 秒後 respawn，但目前狀態與 sequence cache 只存於記憶體；node 重啟後不可假設舊 GOTO 已取消或舊 sequence 仍可安全去重。

### Legacy local-NED Offboard 流程

`serial_elrs_offboard.launch.py` 會啟動 `my_offboard_node` 與 `lr24_command_node`，以 10 Hz 發布本地 NED setpoint，執行 5 m 高的方形軌跡。這不是現行固定翼 GPS GOTO 主線。

## Repository 結構

```text
NYCU_UAV_offboard/
├── CMakeLists.txt
├── package.xml
├── README.md
├── lr24_link_test.py
├── src/
│   ├── global_goto_node.cpp       # 固定翼 GPS GOTO、RTL、安全檢查與狀態監控
│   ├── lr24_command_node.cpp      # 空中端 LR24 serial ↔ ROS service bridge
│   └── my_offboard_node.cpp       # Legacy local-NED Offboard 範例
├── srv/
│   └── GotoGlobal.srv             # GPS 目標與高度基準介面
├── launch/
│   ├── serial_gps_goto.launch.py  # 現行主線
│   └── serial_elrs_offboard.launch.py
├── tools/
│   ├── send_lr24_command.py       # 地面站 CLI
│   └── send_lr24_command_ui.py    # 地面站 Tk GUI
├── deploy/rpi/
│   ├── install.sh
│   ├── config.env.example
│   ├── runtime/
│   └── systemd/
├── test/
│   ├── test_send_lr24_command.py
│   ├── test_send_lr24_command_ui.py
│   └── test_rpi_autostart.py
└── docs/
```

`tools/` 內的地面程式不會由 CMake 安裝；地面電腦必須保留此 repository，並從 repo 根目錄執行。

## 相容環境

| 元件 | 現行版本／要求 |
|---|---|
| 空中端 OS | Ubuntu 24.04 LTS（Raspberry Pi 或 Jetson Orin 類 Linux 伴飛電腦） |
| ROS 2 | Jazzy |
| PX4 | v1.17.x |
| `px4_msgs` | `release/1.17` branch，必須和 PX4 message definitions 相符 |
| Micro XRCE-DDS Agent | v2.4.3 |
| C++ | C++17；CMake 3.8 以上 |
| 地面端 | Windows 或 Ubuntu、Python 3.10 以上、pyserial |
| GUI 額外需求 | Tkinter；Ubuntu 可安裝 `python3-tk` |
| 飛控／通訊 | Pixhawk、兩顆設定相同的 MicoAir LR24-F、兩條不同的空中端 serial device |

本專案沒有使用 MAVSDK 或 MAVROS。PX4 v1.17 實機不要搭配 `px4_msgs` 的 `main` branch。

## 建置

本 repo 與 `px4_msgs` 應放在同一個 colcon workspace 的 `src/` 下：

```bash
NYCU_ROS_WS=/home/pi/NYCU_ROS_WS
cd "$NYCU_ROS_WS/src"

# 尚未安裝 px4_msgs 時才執行
git clone --branch release/1.17 https://github.com/PX4/px4_msgs.git

source /opt/ros/jazzy/setup.bash
cd "$NYCU_ROS_WS"
rosdep install --from-paths src --ignore-src --rosdistro jazzy -r -y
colcon build --packages-up-to my_offboard_cpp
source install/setup.bash
```

完整的 PX4 TELEM2 參數、UART 電壓與 Micro XRCE-DDS Agent 安裝方式請先閱讀 [GPS GOTO 與 Pixhawk 連線操作指南](docs/gps_goto_program.md)。

## Quick Start：GPS GOTO

以下只提供入口；第一次接線或實飛前必須完成文件中的逐階段檢查。Pixhawk DDS 與空中 LR24 是兩個不同裝置，建議都使用穩定的 `/dev/serial/by-id/...` 路徑：

```bash
ls -l /dev/serial/by-id/

PIXHAWK_SERIAL=/dev/serial/by-id/REPLACE_WITH_PIXHAWK_UART_ADAPTER
LR24_SERIAL=/dev/serial/by-id/REPLACE_WITH_AIRBORNE_LR24
```

### 1. 空中端啟動 DDS Agent

```bash
source /opt/ros/jazzy/setup.bash
export ROS_DOMAIN_ID=0

MicroXRCEAgent serial \
  --dev "$PIXHAWK_SERIAL" \
  -b 921600
```

確認 PX4 topic 已出現：

```bash
ros2 topic echo /fmu/out/vehicle_status \
  px4_msgs/msg/VehicleStatus \
  --qos-reliability best_effort --once
```

### 2. 空中端啟動 GPS GOTO 與 LR24 node

另一個 terminal：

```bash
source /opt/ros/jazzy/setup.bash
source "$NYCU_ROS_WS/install/setup.bash"

ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:="$LR24_SERIAL" \
  lr24_baud_rate:=115200
```

`lr24_port` 是必填參數。`115200` 必須與空中端、地面端及兩顆 LR24-F 的設定一致；若硬體設定為 `57600`，所有端點必須一起修改。同一個 serial port 同時只能由一個程式開啟。

### 3. 地面端啟動 GUI 或 CLI

Windows PowerShell：

```powershell
py -m pip install pyserial
py .\tools\send_lr24_command_ui.py
```

Ubuntu：

```bash
sudo apt install -y python3-serial python3-tk
python3 tools/send_lr24_command_ui.py
```

GUI 目前預設 timeout 是 `2.0 s`，CLI 預設是 `8.0 s`。GPS GOTO 可能依序等待最多 2 秒 PX4 ACK 與 3 秒目標確認，因此使用 GUI 執行 GOTO 前，應手動將 timeout 設為至少 `8.0 s`。任何 timeout 都代表「結果未知」，不能直接重送飛航指令。

### 4. 最短操作順序

```text
PING
  ↓
STATUS
  ↓
明確確認 ready_for_goto=true
  ↓
GOTO 或 GOTO_AMSL
  ↓
持續用 STATUS、QGC 與 RC 監看
  ↓
RTL 或由飛手接管
```

Windows CLI 範例；請先把 `COM7`、座標與高度換成實際值：

```powershell
py .\tools\send_lr24_command.py --port COM7 --timeout 8 PING
py .\tools\send_lr24_command.py --port COM7 --timeout 8 STATUS
py .\tools\send_lr24_command.py --port COM7 --timeout 8 GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
py .\tools\send_lr24_command.py --port COM7 --timeout 8 RTL
```

收到 GOTO 的 `ACK` 只表示命令已被接受及確認，不表示飛機已抵達目標。必須持續查詢 `STATUS` 並監看飛機實際狀態。

## 指令表

### GPS GOTO 主線

| 指令 | 參數 | 說明 |
|---|---|---|
| `PING` | 無 | 測試 LR24 雙向通訊，預期回覆 `PONG` |
| `HELP` | 無 | 列出 command node 認得的名稱；不保證目前 launch 已啟動對應 service |
| `STATUS` | 無 | 查詢 GOTO 狀態、安全 gate 與 `ready_for_goto` |
| `GOTO` | `lat lon relative_home_alt_m` | 相對 Home 高度的單點目標 |
| `GOTO_AMSL` | `lat lon altitude_amsl_m` | AMSL 高度的單點目標；仍會換算並套用 relative-Home 限制 |
| `RTL` | 無 | 要求 PX4 進入 `AUTO_RTL` |
| `ABORT` | 無 | 目前與 `RTL` 相同 |

正式飛航命令必須使用 `$CMD,...*XX` checksummed frame；CLI／GUI 會自動建立並驗證。不要對 `GOTO`、`RTL` 等命令使用 `--simple`。

### Legacy Offboard 範例

只有在空中端啟動 `serial_elrs_offboard.launch.py` 時，`ENABLE_STREAM`、`START_MISSION`、`START_OFFBOARD`、`STOP_OFFBOARD`、`LAND` 才有對應 service。GPS GOTO 主線未啟動 `my_offboard_node`，使用這些按鈕通常會收到 `ROS service not available`。

## 主要 launch 參數

| 參數 | 預設值 | 用途 |
|---|---:|---|
| `lr24_port` | 必填 | 空中端 LR24 serial device |
| `lr24_baud_rate` | `115200` | LR24 USB serial baud |
| `lr24_service_response_timeout_s` | `7.0` | command bridge 等待 ROS service 的上限 |
| `telemetry_timeout_s` | `1.0` | PX4 telemetry 新鮮度上限 |
| `gps_min_fix_type` | `3` | 最低 GPS fix type |
| `gps_min_satellites` | `8` | 最少衛星數 |
| `gps_max_horizontal_accuracy_m` | `5.0` | 最大水平誤差 |
| `gps_max_vertical_accuracy_m` | `8.0` | 最大垂直誤差 |
| `max_target_distance_m` | `2000.0` | 目標距目前位置及 Home 的上限 |
| `min_relative_altitude_m` | `30.0` | 最低相對 Home 高度 |
| `max_relative_altitude_m` | `120.0` | 最高相對 Home 高度 |
| `ack_timeout_s` | `2.0` | 等待 PX4 command ACK |
| `confirmation_timeout_s` | `3.0` | 等待 Auto Loiter 與 matching setpoint |
| `arrival_horizontal_threshold_m` | `100.0` | 水平抵達門檻 |
| `arrival_vertical_threshold_m` | `15.0` | 垂直抵達門檻 |
| `arrival_hold_time_s` | `2.0` | 持續符合門檻後才判定抵達 |

查看完整參數：

```bash
ros2 launch my_offboard_cpp serial_gps_goto.launch.py --show-args
```

## Raspberry Pi 開機自動啟動

在手動流程已驗證、workspace 已完成 build 後，可安裝 systemd 服務：

```bash
sudo bash deploy/rpi/install.sh \
  --user pi \
  --workspace /home/pi/NYCU_ROS_WS \
  --pixhawk-device /dev/serial/by-id/REPLACE_WITH_PIXHAWK_UART_ADAPTER \
  --lr24-device /dev/serial/by-id/REPLACE_WITH_AIRBORNE_LR24
```

這會管理 Micro XRCE-DDS Agent 與 GPS GOTO launch，使用非 root 帳號、等待 serial device，並在程序異常後重啟。安裝與驗收步驟請見 [RPi 開機自動啟動 DDS 與 ROS 2 節點](docs/rpi_autostart.md)。

目前 installer 的 build freshness 檢查沒有涵蓋 `global_goto_node`；修改核心 GOTO 程式後，安裝前必須自行重新執行 `colcon build --packages-up-to my_offboard_cpp`。

收工時應先執行 `sudo shutdown -h now`，等待系統完全關機後再斷電。不要直接拔除 Raspberry Pi 電源。

## 測試

不依賴 ROS 的 Python 測試：

```bash
python3 -B -m unittest discover -s test -p 'test_*.py' -v
```

ROS 2 workspace 測試：

```bash
source /opt/ros/jazzy/setup.bash
source "$NYCU_ROS_WS/install/setup.bash"
colcon test --packages-select my_offboard_cpp --event-handlers console_direct+
colcon test-result --verbose
```

本次 repository review 在 Windows 執行 26 個 Python tests，結果全部通過；也通過 Python syntax compile 與 `git diff --check`。目前環境沒有 ROS 2／colcon，因此這不包含 C++ Linux build、ROS launch、SITL、systemd 或實機飛行驗證。

所有飛航修改仍應依序通過：單元測試、假資料／SITL、地面拆槳、人工接管演練、低風險飛行，最後才進入完整任務測試。

## 文件導覽

| 文件 | 用途與狀態 |
|---|---|
| [LR24-F 快速測試與系統啟動實測手冊](docs/lr24_quick_test_guide.md) | 最短 bring-up 路徑、實測紀錄與常見問題 |
| [LR24-F 地面站控制完整教學](docs/lr24_ground_station_tutorial.md) | Windows／Ubuntu 操作員完整流程；其中 GUI timeout 敘述仍待與目前 `2.0 s` 預設同步 |
| [GPS GOTO 與 Pixhawk 連線操作指南](docs/gps_goto_program.md) | PX4、DDS、ROS workspace、SITL 與實機步驟 |
| [RPi 自動啟動指南](docs/rpi_autostart.md) | systemd 安裝、管理與驗收 |
| [LR24-F Link Test 使用教學](docs/lr24_link_test_tutorial.md) | 不含 ROS 的透明串列 loss／RTT 測試 |
| [LR24 Serial／RC Offboard 操作](docs/lr24_serial_program.md) | 含 legacy local-NED Offboard 流程 |
| [LR24 PPP／SSH 架構](docs/lr24_ppp_program.md) | 實驗性替代連線方案，不是現行 GPS GOTO 主線 |
| [控制架構](docs/control_architecture.md) | 早期概念與硬體分工；部分 MAVLink／Jetson-only 描述已過時 |
| [LR24 通訊設計](docs/lr24_communication.md) | 早期 feature／legacy 背景；主線行為以本 README 與 GPS GOTO 文件為準 |

## 已知限制與 Roadmap

- 修正 GUI timeout 預設值、文件與 regression test 的不一致。
- 補強 RPi installer 對 `global_goto_node` build freshness 的檢查。
- 為 node respawn 加入 PX4 狀態重建、`UNKNOWN/RECOVERING` 狀態及跨程序 sequence 去重策略。
- 為 `global_goto_node` 的 safety gate、ACK、RTL、preemption 建立 C++／ROS integration tests。
- 驗證 PX4 v1.17 uXRCE-DDS、固定翼 SITL GOTO／RTL、serial 重連與 systemd 冷開機。
- 為 legacy `STOP_OFFBOARD`／`LAND` 增加 PX4 ACK 與安全 mode 確認。
- 實作多航點、雲台目標資料介面、座標濾波與追蹤狀態機。
- 完整定義 geofence、terrain／AGL、速度／轉彎率及通訊失聯策略。
- 完成 `package.xml` 的 description、maintainer、version、license，並加入 repository license。

## 協作規範

- 不直接在 `master` 開發；每個功能使用獨立 `feature/`、`fix/`、`test/` 或 `docs/` branch。
- 合併前執行可用的自動測試，並清楚註明未執行的 C++、SITL 或實機項目。
- 通訊格式、PX4 message version、安全參數及座標／高度基準的修改必須同步更新程式與文件。
- Commit 使用簡短、明確的英文現在式，例如 `Add GPS status validation` 或 `Fix LR24 response timeout`。
- 任何外部座標都必須標明單位與高度基準，且不得繞過安全檢查直接送入 PX4。
