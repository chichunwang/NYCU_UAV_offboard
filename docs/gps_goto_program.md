# LR24 GPS GOTO 與 Pixhawk 連線操作指南

本流程讓地面端透過 LR24-F 傳送單一 GPS 目標給 RPi / Jetson Orin，再由 `global_goto_node` 經 PX4 uXRCE-DDS 將重新定位指令交給 Pixhawk。

> 如果你只想知道「地面筆電接上 LR24 後到底要輸入什麼」，請先看 [LR24-F 地面站控制完整教學](lr24_ground_station_tutorial.md)。該文件以 Windows PowerShell 為主，另附 Ubuntu 指令、預期回覆與問題排查。

> **安全邊界：** `global_goto_node` 不會主動發出 arm、takeoff 或 land command。飛手必須先用 QGroundControl 或 RC/ELRS 完成固定翼起飛，並保留能立即切換模式接管的獨立 RC 鏈路。`GOTO` 只接受已解鎖、已離地且飛控/GPS 狀態符合安全條件的飛機。若操作員另送 `RTL`，PX4 是否最終降落仍取決於其 Return 參數。

## 1. 連線架構：兩條不同的序列埠

Pixhawk 與 LR24-F 是兩個獨立裝置，不能使用同一個 serial port：

```text
Pixhawk 6C Mini / TELEM2
    │ 3.3 V UART
    ▼
3.3 V USB-to-UART adapter
    │ USB serial，例如 /dev/serial/by-id/...Pixhawk...
    ▼
RPi / Jetson Orin
    │
    │ ROS 2 / Micro XRCE-DDS Agent
    ▼
global_goto_node

地面電腦
    │ USB serial
    ▼
地面端 LR24-F
    │ 無線鏈路
    ▼
空中端 LR24-F
    │ USB serial，例如 /dev/serial/by-id/...LR24...
    ▼
RPi / Jetson Orin / lr24_command_node
```

Linux 的 `/dev/ttyUSB0`、`/dev/ttyUSB1` 會依插入順序改變。兩端都應先找出自己的穩定裝置名稱：

```bash
ls -l /dev/serial/by-id/
```

在空中端設定兩個不同的變數；下列名稱只是占位字串，必須換成該電腦實際列出的項目：

```bash
PIXHAWK_SERIAL=/dev/serial/by-id/REPLACE_WITH_PIXHAWK_UART_ADAPTER
LR24_SERIAL=/dev/serial/by-id/REPLACE_WITH_AIRBORNE_LR24

readlink -f "$PIXHAWK_SERIAL"
readlink -f "$LR24_SERIAL"
```

兩個 `readlink` 結果必須是不同裝置。若使用者沒有 serial port 權限，可將帳號加入 `dialout` 群組，登出再登入後生效：

```bash
sudo usermod -aG dialout "$USER"
```

### UART 電壓與接線警告

Pixhawk 6C Mini 的 TELEM2 UART TX/RX/CTS/RTS 是 **3.3 V 邏輯**，第 1 腳則是 **+5 V 電源**。

- Pixhawk TX 接轉接器 RX，Pixhawk RX 接轉接器 TX，GND 必須共地。
- USB-to-UART 必須使用 3.3 V TTL 邏輯；RS-232 電壓不可直接接入。
- RPi / Orin 若使用板載 UART，必須先查清楚其 I/O 電壓；1.8 V UART 需要電平轉換器，否則可能損壞板子。
- 伴飛電腦若已由自己的電源供電，**不要連接 TELEM2 的 +5 V 腳，也不要用 TELEM2 供電給 Orin/RPi**。
- 初次接線先保持飛機拆槳並用萬用表確認電壓與 GND，再接上訊號線。

## 2. 版本組合

本文件使用以下彼此相符的組合：

| 元件 | 版本 |
|---|---|
| Ubuntu | 24.04 LTS |
| ROS 2 | Jazzy |
| PX4 | v1.17.x；實機與 SITL 應使用相同 minor 版本 |
| `px4_msgs` | `release/1.17` branch |
| Micro XRCE-DDS Agent | v2.4.3 |

PX4 firmware 與 `px4_msgs` 的 message definitions 必須相符。不要將 PX4 v1.17 實機直接搭配 `px4_msgs` 的 `main` branch。

## 3. 設定 Pixhawk TELEM2

先在 QGroundControl 備份現有參數，再於 Parameters 頁面設定：

| PX4 參數 | 建議值 | 說明 |
|---|---|---|
| `MAV_1_CONFIG` | `Disabled` | TELEM2 預設通常被 MAVLink instance 佔用；必須先釋放。同一連接埠不可同時給 MAVLink 與 uXRCE-DDS。 |
| `UXRCE_DDS_CFG` | `TELEM 2` | 在 TELEM2 啟動 uXRCE-DDS client。 |
| `SER_TEL2_BAUD` | `921600` | 必須和 Agent 的 `-b` 完全相同；若硬體鏈路無法穩定支援，可降低，但兩端要一起修改。 |
| `UXRCE_DDS_DOM_ID` | `0` | 必須和 ROS 2 的 `ROS_DOMAIN_ID` 相同。 |
| `UXRCE_DDS_NS_IDX` | `-1` | 使用未加 namespace 的 `/fmu/in/*`、`/fmu/out/*` topics；若改成其他值，本專案 topic 名稱也必須一起調整。 |

在 PX4 v1.17 的 QGroundControl **MAVLink Console**，前三項也可直接設定為：

```sh
param set MAV_1_CONFIG 0
param set UXRCE_DDS_CFG 102
param set SER_TEL2_BAUD 921600
param save
reboot
```

其中 `0` 代表停用該 MAVLink instance，`102` 代表 `TELEM 2`。若不是 PX4 v1.17，應先在該版本的 Parameters 頁面確認 enum 對應值，不要直接沿用數字。

儲存後重新開機 Pixhawk。再次確認 TELEM2 沒有其他 MAVLink instance 或 serial driver 佔用。

## 4. 安裝 Micro XRCE-DDS Agent v2.4.3

以下是在 RPi / Orin 以原始碼安裝的做法：

```bash
sudo apt update
sudo apt install git cmake build-essential

git clone --branch v2.4.3 https://github.com/eProsima/Micro-XRCE-DDS-Agent.git
cd Micro-XRCE-DDS-Agent
mkdir build
cd build
cmake ..
cmake --build . --parallel 2
sudo cmake --install .
sudo ldconfig
```

確認版本與指令可執行：

```bash
MicroXRCEAgent --version
MicroXRCEAgent --help
```

## 5. 準備 ROS 2 workspace 與 `px4_msgs`

本專案與 `px4_msgs` 應位於同一個 colcon workspace 的 `src/` 下。以下用 `NYCU_ROS_WS` 表示實際 workspace，請先改成自己的絕對路徑：

```bash
NYCU_ROS_WS=/absolute/path/to/nycu_ros_ws
NYCU_ROS_WS=/home/pi/NYCU_ROS_WS
cd "$NYCU_ROS_WS/src"
git clone --branch release/1.17 https://github.com/PX4/px4_msgs.git
```

將本 repo 放在同一個 `src/` 目錄後建置：

```bash
source /opt/ros/jazzy/setup.bash
cd "$NYCU_ROS_WS"
rosdep install --from-paths src --ignore-src --rosdistro jazzy -r -y
colcon build --packages-up-to my_offboard_cpp
source install/setup.bash
```

若 `px4_msgs` 已存在，先確認 branch，不要重複 clone：

```bash
cd "$NYCU_ROS_WS/src/px4_msgs"
git branch --show-current
```

輸出應為 `release/1.17`。

## 6. 啟動 Pixhawk DDS 連線

在空中端第一個 terminal 啟動 Agent。裝置名稱與 baud rate 必須和前面設定一致：

```bash
PIXHAWK_SERIAL=/dev/serial/by-id/REPLACE_WITH_PIXHAWK_UART_ADAPTER
MicroXRCEAgent serial --dev "$PIXHAWK_SERIAL" -b 921600
```

Agent 顯示 client 已建立後，在第二個 terminal 驗證 PX4 topics：

```bash
NYCU_ROS_WS=/absolute/path/to/nycu_ros_ws
export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source "$NYCU_ROS_WS/install/setup.bash"

ros2 topic list | grep '^/fmu/'
ros2 topic echo /fmu/out/vehicle_status px4_msgs/msg/VehicleStatus --qos-reliability best_effort --once
ros2 topic echo /fmu/out/vehicle_global_position px4_msgs/msg/VehicleGlobalPosition --qos-reliability best_effort --once
ros2 topic echo /fmu/out/vehicle_gps_position px4_msgs/msg/SensorGps --qos-reliability best_effort --once
ros2 topic echo /fmu/out/home_position px4_msgs/msg/HomePosition --qos-reliability best_effort --once
```

> **PX4 版本綁定注意：** PX4 v1.17.0 官方 `dds_topics.yaml` 的所有 topic 都**沒有** `_v1` 後綴，本專案節點也訂閱無後綴名稱。若 `ros2 topic list` 出現 `/fmu/out/vehicle_status_v1` 等名稱，代表該韌體是 PX4 main／較新版本（已導入 message versioning），節點會**靜默收不到任何資料**；請改用 v1.17.x 韌體，或同步修改程式碼的 topic 名稱並重新驗證。

`global_goto_node` 需要以下 PX4 topics；啟動後可用 `ros2 topic list` 逐一確認：

```text
/fmu/out/vehicle_global_position
/fmu/out/vehicle_gps_position
/fmu/out/home_position
/fmu/out/vehicle_status
/fmu/out/vehicle_land_detected
/fmu/out/failsafe_flags
/fmu/out/vehicle_command_ack
/fmu/out/position_setpoint_triplet
/fmu/in/vehicle_command
```

至少要能持續看到 `vehicle_status`、`vehicle_global_position` 與 `vehicle_gps_position`，並取得有效 `home_position`。若完全沒有 `/fmu/` topics，依序檢查：

1. Pixhawk 與 LR24 是否誤用了同一個 Linux 裝置。
2. TX/RX 是否交叉、GND 是否共地、邏輯電壓是否正確。
3. Agent baud 與 `SER_TEL2_BAUD` 是否相同。
4. `MAV_1_CONFIG` 是否已停用，`UXRCE_DDS_CFG` 是否為 TELEM2。
5. Pixhawk 是否已在參數修改後重新開機。
6. `ROS_DOMAIN_ID` 與 `UXRCE_DDS_DOM_ID` 是否相同。
7. `px4_msgs` 是否為 `release/1.17`。

## 7. 啟動 GPS GOTO 節點

在空中端第三個 terminal 啟動專用 launch：

```bash
NYCU_ROS_WS=/absolute/path/to/nycu_ros_ws
export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source "$NYCU_ROS_WS/install/setup.bash"

LR24_SERIAL=/dev/serial/by-id/REPLACE_WITH_AIRBORNE_LR24
ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:="$LR24_SERIAL" \
  lr24_baud_rate:=115200
```

此 launch **只啟動**：

- `global_goto_node`：檢查 PX4/GPS/高度/距離後送出 global reposition 或 RTL command。
- `lr24_command_node`：接收 LR24 低頻文字指令並呼叫 GPS GOTO services。

它不會啟動舊的 `my_offboard_node`，因此不會送舊版的本地 NED 方形軌跡，也不會自動切 Offboard 或自動 arm。

先從空中端直接確認狀態 service 可用：

```bash
ros2 service call /gps_goto_status std_srvs/srv/Trigger '{}'
```

可先列出 launch 參數與目前值：

```bash
ros2 launch my_offboard_cpp serial_gps_goto.launch.py --show-args
ros2 param list /global_goto_node
ros2 param dump /global_goto_node
```

launch 公開的 global node 參數如下：

| Launch / ROS 參數 | 預設值 | 用途 |
|---|---:|---|
| `telemetry_timeout_s` | `1.0` | PX4 狀態、GPS 等必要 telemetry 的最長允許資料年齡。 |
| `gps_min_fix_type` | `3` | 至少需要 3D fix。 |
| `gps_min_satellites` | `8` | 最少使用衛星數。 |
| `gps_max_horizontal_accuracy_m` | `5.0` | GPS 最大允許水平誤差。 |
| `gps_max_vertical_accuracy_m` | `8.0` | GPS 最大允許垂直誤差。 |
| `max_target_distance_m` | `2000.0` | 同時限制目前位置至目標、以及 Home 至目標的最大水平距離。 |
| `min_relative_altitude_m` | `30.0` | 目標相對 Home 的最低高度。 |
| `max_relative_altitude_m` | `120.0` | 目標相對 Home 的最高高度。 |
| `ack_timeout_s` | `2.0` | 等待 PX4 `VehicleCommandAck` 的時間。 |
| `confirmation_timeout_s` | `3.0` | 等待 PX4 接受並反映新 navigation setpoint 的時間。 |
| `setpoint_horizontal_tolerance_m` | `5.0` | 比對 PX4 setpoint 與要求座標時的水平容許差。 |
| `setpoint_altitude_tolerance_m` | `2.0` | 比對 PX4 setpoint 與要求高度時的垂直容許差。 |
| `arrival_horizontal_threshold_m` | `100.0` | 判定進入目標區的水平距離；固定翼不可設成不合理的小數值。 |
| `arrival_vertical_threshold_m` | `15.0` | 抵達判定的高度容許差。 |
| `arrival_hold_time_s` | `2.0` | 必須連續位於抵達門檻內的時間。 |
| `monitor_rate_hz` | `5.0` | 健康與抵達狀態監控頻率。 |
| `source_system_id` | `1` | 發送給 PX4 的 command source system ID。 |
| `source_component_id` | `191` | 發送給 PX4 的 command source component ID。 |

`lr24_service_response_timeout_s` 預設為 `7.0` 秒，屬於
`lr24_command_node`：若 ROS service 沒有回覆，client 會移除該 pending request、向地面端回
`ERR`，並解除 busy 狀態。地面工具預設等待 8 秒，因此通常能收到這個明確的 timeout
回覆，而不是只在地面端無限等待。

也可在 launch 時覆寫，例如把場測最大距離限制成 500 m：

```bash
ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:="$LR24_SERIAL" \
  max_target_distance_m:=500.0
```

安全參數必須依實際空域、固定翼轉彎半徑、地理圍欄及法規調整；在 SITL 通過前不要放寬。這些應用層限制不能取代 PX4 geofence 與 failsafe。

## 8. 地面端指令

本節是指令參考。第一次操作請改由 [LR24-F 地面站控制完整教學](lr24_ground_station_tutorial.md) 依序完成接線、Windows／Ubuntu serial port、PING、STATUS、GOTO 與 RTL 測試。

地面電腦也應使用自己的 `/dev/serial/by-id/...`，不要照抄空中端名稱：

```bash
sudo apt install python3-serial

NYCU_REPO=/absolute/path/to/NYCU_UAV_offboard
GROUND_LR24_SERIAL=/dev/serial/by-id/REPLACE_WITH_GROUND_LR24
cd "$NYCU_REPO"
```

### 查詢狀態

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" STATUS
```

只有狀態顯示 Pixhawk topics 新鮮、home/global position 有效、飛機已 arm 並離地時，才可接受 GOTO。

`ready_for_goto=true` 還代表：目前是固定翼且不在 VTOL transition、RC/manual control link 未失效、沒有 battery/geofence/navigator/wind/critical failure、fused position 不是 dead reckoning、GPS fix/衛星數與 raw/fused eph/epv 都符合 launch 門檻。任一項不符時，新 GOTO 會被拒絕。這個檢查只會拒絕命令，不能取代飛手與 PX4 failsafe。

### 相對 Home 高度 GOTO

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
```

例如 `REL_HOME_ALT_M=120` 代表目標高度是 Home AMSL 高度加 120 m，不是海拔 120 m。節點會在接受指令當下以有效的 Home altitude 換算為 AMSL。

### AMSL 高度 GOTO

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" GOTO_AMSL LAT_DEG LON_DEG ALT_AMSL_M
```

`ALT_AMSL_M` 是平均海平面高度。它不是橢球高，也不是相對地面 AGL。若不能明確確認 AMSL 數值，使用相對 Home 的 `GOTO` 較不容易混淆。

`GOTO_AMSL` 也會先換算成相對 Home 高度，再套用 `min_relative_altitude_m` 與 `max_relative_altitude_m`；不能用 AMSL 指令繞過高度限制。

### 返航

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" RTL
```

`RTL` 只要求 PX4 進入 Return mode；實際返航高度、盤旋與是否最終降落由 PX4 的 RTL 參數與任務設定決定。此節點本身不會發出 LAND command。

為了保留緊急 escape path，伴飛端只要求能取得新鮮的 `VehicleStatus` 與有效 PX4 system/component ID，就會嘗試送 RTL；它不會因 GPS 品質、RC loss、battery warning 或其他 GOTO safety gate 而先行封鎖。是否接受 Return mode 仍由 PX4 的 ACK、狀態與自身 failsafe 邏輯決定。

若 `GOTO` 或其他 ROS service 尚在等待回覆，新的 checksummed `RTL`／`ABORT`
會先取消空中端對舊 service response 的等待，讓舊 sequence 收到 `ERR`，再優先要求
`global_goto_node` 中止 GOTO 等待並送出 RTL。PX4 已經收到的 command 無法由 ROS client
「收回」，因此仍須以 RTL 的 ACK、`AUTO_RTL` 狀態與 RC 接管結果為準。若已經有一筆
RTL／ABORT 在等待，後續不同 sequence 的重複緊急指令會回 `ERR`，不會再堆疊多個
blocking callbacks；原本那一筆仍繼續等待 PX4 確認。

工具會產生以下正式封包；`CS` 是 `$` 後、`*` 前完整 payload 的 XOR checksum：

```text
$CMD,<seq>,GOTO,<lat_deg>,<lon_deg>,<alt_rel_home_m>*<CS>
$CMD,<seq>,GOTO_AMSL,<lat_deg>,<lon_deg>,<alt_amsl_m>*<CS>
$CMD,<seq>,RTL*<CS>
```

GPS GOTO 與 RTL 不接受 `--simple`，也不能手動省略 checksum。sequence 重送只能用於重取同一筆指令的既有結果；同一 sequence 搭配不同 payload 會被拒絕。

> 目前兩位元十六進位 XOR checksum 是為了相容既有 LR24 協定，錯誤偵測能力不等同 CRC-16/CRC-32。實飛必須同時保留 PX4 geofence、嚴格的距離/高度限制與獨立 RC/ELRS 接管；若底層鏈路本身沒有可靠的封包 CRC，下一版協定應先升級 CRC，再進行實飛。

### LR24 / Orin 失聯時的重要語意

GOTO 是交給 PX4 執行的一次性 reposition command，不是從地面持續串流的 Offboard setpoint。因此：

- PX4 接受 GOTO 後，即使 LR24 隨後斷線，飛機通常仍會繼續執行已接受的目標；LR24 斷線本身不等於取消任務。
- 這個 launch 不使用舊版 Offboard stream，所以不能把 `COM_OF_LOSS_T` 當成 LR24 失聯保護。
- Agent 或 Orin 完全失效後，伴飛程式已無法再送 RTL；此時只能依賴 RC/ELRS 接管及 PX4 自身已設定的 geofence、RC-loss、position-loss 等 failsafe，但 Agent/LR24 失聯本身不保證會觸發其中任何一項。
- 若要取消目前 GOTO，鏈路正常時送 `RTL`，或由飛手直接用 RC 切換至事先演練過的安全模式。不要等到失聯後才規劃處置。

### 指令回覆語意

- `ACK ... GOTO accepted`：PX4 已回覆 command accepted，節點也確認飛控進入 `AUTO_LOITER` 且 `PositionSetpointTriplet.current` 符合要求目標；這時狀態是 `ENROUTE`，不代表飛機已抵達。
- `ERR ...`：指令被 parser、安全 gate、ROS service 或 PX4 拒絕。先處理錯誤原因，不要連續重送。
- `STATUS`：狀態查詢 service 的 `success` 固定為 true，所以 LR24 會回 ACK，即使飛機尚未 ready。是否真的可送 GOTO 要看 `ready_for_goto=true|false`；若為 false，讀取 `readiness_reason`。通訊剛啟動時需等待所有必要 topics 更新。
- PX4 拒絕或 command ACK 逾時時，節點不會假裝執行成功。
- 飛手用 RC 切換模式接管後，以 RC 狀態為準；系統不會自行重新 arm 或起飛。

`STATUS` 的主要 state 如下：

| State | 語意 |
|---|---|
| `IDLE` | 尚未接受 GOTO。`IDLE` 不等於可飛，仍須看 `ready_for_goto`。 |
| `COMMAND_PENDING` | 輸入已通過應用層驗證，正在等待 PX4 command ACK、`AUTO_LOITER` 與 matching setpoint confirmation。 |
| `ENROUTE` | PX4 已接受 reposition，飛機正飛向目標並準備/執行固定翼 loiter。 |
| `ARRIVED` | 水平與高度誤差連續 `arrival_hold_time_s` 都在門檻內；飛機仍會繼續盤旋，不會懸停或自動降落。 |
| `ABORTED` | ACK/confirmation timeout 或拒絕、模式被飛手覆寫、telemetry/GPS/failsafe/arming/airborne 條件失效、PX4 setpoint 不再符合，或已確認 RTL。監控器造成的 `ABORTED` 只停止 node-side tracking，不會另送 Hold/RTL 或改變 PX4 mode；必須讀 `detail` 並確認實際 flight mode。 |

狀態回覆還會包含 `nav_state`、`armed`、`fixed_wing`、`landed`、GPS fix/衛星數/eph/epv；有活動目標時另包含 target、目標距 Home 距離、目前水平距離與高度誤差。

### 固定翼抵達目標後的行為

固定翼不能在 GPS 點懸停。PX4 的 reposition 行為會飛往目標，接近後以 loiter 半徑繞點盤旋；本節點未指定 command loiter radius，因此使用 PX4 的固定翼 loiter 設定（例如 `NAV_LOITER_RAD`）。目標點及其完整盤旋圓都必須位於地理圍欄內，並預留風、定位誤差與轉彎半徑的安全裕度。

目前節點會限制高度與目標距目前位置/Home 的距離，也會拒絕「已經 breach」的狀態，但不會解析 PX4 geofence polygon、禁航區或沿途 terrain/AGL。飛手仍須先在 QGroundControl 驗證完整航路與盤旋圓，不可只檢查終點座標。

## 9. 固定翼 SITL 必須先通過

### 9.1 啟動 PX4 v1.17 Cessna SITL

在 PX4 terminal：

```bash
cd /absolute/path/to/PX4-Autopilot
git checkout v1.17.0
make px4_sitl gz_rc_cessna
```

SITL 預設以 UDP port 8888 啟動 uXRCE-DDS client。另一個 terminal 啟動 Agent：

```bash
MicroXRCEAgent udp4 -p 8888
```

### 9.2 用 pseudo-TTY 模擬 LR24

如果 SITL 電腦沒有兩顆實體 LR24，可用 `socat` 建立一對暫時 serial endpoints：

```bash
sudo apt install socat
socat -d -d \
  pty,raw,echo=0,link=/tmp/nycu_lr24_air \
  pty,raw,echo=0,link=/tmp/nycu_lr24_ground
```

保持 `socat` 執行。在另一個 terminal 啟動：

```bash
NYCU_ROS_WS=/absolute/path/to/nycu_ros_ws
export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source "$NYCU_ROS_WS/install/setup.bash"
ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:=/tmp/nycu_lr24_air \
  lr24_baud_rate:=115200
```

地面端測試工具改用另一端：

```bash
python3 tools/send_lr24_command.py --port /tmp/nycu_lr24_ground STATUS
```

使用 QGroundControl 或模擬 RC 依固定翼正常流程完成 arm、起飛並爬升到安全高度；本專案不代替這一步。從 `/fmu/out/vehicle_global_position` 或 QGroundControl 取得目前位置，再選擇一個位於測試 geofence 內、距離足以讓固定翼轉入的目標，依序測試 `GOTO`、`GOTO_AMSL` 與 `RTL`。

### 9.3 SITL 驗收

- 未 arm 或尚未離地時，GOTO 必須回 ERR，且不能讓飛機自行 arm。
- latitude/longitude 超界、NaN、過低/過高或超過最大距離的目標必須被拒絕。
- 相對 Home 與 AMSL 高度必須到達相同的預期物理高度，不可正負號顛倒。
- 抵達目標後固定翼應穩定盤旋，而不是嘗試懸停。
- RC/QGroundControl 切換模式後必須能立即接管。
- 停止 `lr24_command_node`、Agent 或 `global_goto_node` 後，觀察 PX4 是否繼續既有 reposition；確認飛手仍能以 RC 切換 Hold/RTL/手動安全模式，且系統不會自行解鎖或改送新目標。
- 模擬 GPS/position loss，確認 PX4 依已設定的 position-loss failsafe 執行預期行為。
- `RTL` 的飛行路徑、高度與最終動作符合 PX4 參數設定。

## 10. 實機分階段檢查表

以下各階段必須依序通過；前一階段失敗就停止：

1. **SITL：** 完成上一節所有正常與故障測試。
2. **桌面通訊：** Pixhawk 使用獨立供電、飛機拆槳；確認兩個 `/dev/serial/by-id` 不會互換，連續重開機十次仍可收到 PX4 topics 與 LR24 ACK。
3. **拆槳命令測試：** 未 arm、GPS 無效、home 無效、座標超界及高度超界時，任何 GOTO 都不能造成解鎖或致動器輸出。
4. **失聯與 failsafe：** 在拆槳狀態逐一拔除 LR24、Pixhawk serial、停止 Agent 與關閉 Orin；確認不會產生新 command，記錄 PX4 是繼續既有 reposition 或由自身 failsafe 接管，並驗證 RC/ELRS 始終可以切換到預定安全模式。
5. **RC 接管：** 飛手先在安全環境演練模式切換與 kill/abort 流程，確認 LR24 或 Orin 不會自動搶回控制權。
6. **低風險實飛：** 空曠、合法空域、良好天候，先由飛手起飛到安全高度，再送 fence 內且保守的單一 GOTO；全程保留目視與 RC 接管。
7. **記錄檢查：** 每次測試保存 PX4 ULog、節點 log、實際參數與 LR24 回覆；確認高度基準、command ACK、failsafe 與盤旋半徑後才擴大距離。

## 參考資料

- [PX4 v1.17 uXRCE-DDS](https://docs.px4.io/v1.17/en/middleware/uxrce_dds)
- [PX4 ROS 2 User Guide](https://docs.px4.io/main/en/ros/ros2_comm)
- [PX4 v1.17 Gazebo Simulation](https://docs.px4.io/v1.17/en/sim_gazebo_gz/)
- [PX4 Offboard Mode and failsafe](https://docs.px4.io/v1.17/en/flight_modes/offboard)
- [PX4 Fixed-Wing Hold Mode](https://docs.px4.io/v1.17/en/flight_modes_fw/hold)
- [Holybro Pixhawk 6C Mini port pinout](https://docs.holybro.com/autopilot/pixhawk-6c-mini/pixhawk-6c-mini-ports)
