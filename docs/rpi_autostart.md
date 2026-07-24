# Raspberry Pi 開機自動啟動 DDS 與 ROS 2 節點

這套設定會在 RPi 開機後自動常駐兩個 systemd 服務：

1. `nycu-uav-dds@<user>.service`：啟動 `MicroXRCEAgent`，連接 Pixhawk uXRCE-DDS。
2. `nycu-uav-topics@<user>.service`：啟動 `serial_gps_goto.launch.py`，也就是
   `global_goto_node` 與 `lr24_command_node`。

`ros2 topic list` 只是一次性的檢查指令，不需要設成服務。Pixhawk client 與 Agent
連線後，`/fmu/*` topics 會自動出現在 ROS graph。這套開機服務不會自動 arm、
起飛、降落或送出 GOTO。

## 1. 安裝前確認

先完成 [GPS GOTO 操作指南](gps_goto_program.md)的 Agent、ROS workspace 與
`px4_msgs release/1.17` 安裝，並建置 workspace：

```bash
source /opt/ros/jazzy/setup.bash
cd /home/pi/NYCU_ROS_WS
colcon build --packages-up-to my_offboard_cpp
```

確認 Agent 路徑及兩個 USB serial by-id：

```bash
command -v MicroXRCEAgent
ls -l /dev/serial/by-id/
```

Pixhawk 與 LR24 必須是不同裝置。不要使用會在重開機後互換的
`/dev/ttyUSB0`、`/dev/ttyUSB1`。本專案實測記錄的路徑是：

```text
Pixhawk: /dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
LR24:    /dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0
```

每台機器仍應以 `ls -l` 的實際輸出為準，不能只看上面的範例。

## 2. 一次性安裝

安裝器會停止／重啟既有服務，只能在飛機停機並拆槳時執行，不能在飛行中更新。

在本 repo 根目錄執行，下列路徑請換成 RPi 的實際值：

```bash
cd /home/pi/NYCU_ROS_WS/src/NYCU_UAV_offboard

sudo bash deploy/rpi/install.sh \
  --user pi \
  --workspace /home/pi/NYCU_ROS_WS \
  --pixhawk-device /dev/serial/by-id/REPLACE_WITH_PIXHAWK_UART_ADAPTER \
  --lr24-device /dev/serial/by-id/REPLACE_WITH_AIRBORNE_LR24
```

安裝器會：

- 將 `pi` 加入 `dialout`，服務本身不以 root 執行。
- 驗證 DDS Agent、ROS overlay 與本 repo 的已建置 launch/node；若 build 過期會拒絕安裝。
- 寫入 root-owned 設定 `/etc/nycu-uav-offboard/config.env`。
- 安裝 DDS 與 ROS wrapper、systemd units。
- 啟用並立即啟動 `nycu-uav-offboard@pi.target`。
- 開機時等待 USB by-id 裝置可讀寫，避免 LR24 太晚出現而永久漏接。
- systemd 程序退出時每 10 秒重啟，ROS child 異常退出時 launch 會在 3 秒後重生它。
- 裝置尚未接上時會持續等待；LR24 開埠失敗或運行中短暫斷線時每 2 秒重連。

預設值為 ROS 2 Jazzy、`ROS_DOMAIN_ID=0`、Pixhawk `921600` baud、LR24
`115200` baud。若設備暫時不在 RPi 上，可加 `--no-start`；服務仍會在下次
開機時啟動。所有選項可用下列指令查看：

```bash
sudo bash deploy/rpi/install.sh --help
```

## 3. 驗證

先看兩個服務：

```bash
systemctl --no-pager --full status nycu-uav-dds@pi.service
systemctl --no-pager --full status nycu-uav-topics@pi.service
```

只看本次開機 log：

```bash
journalctl \
  -u nycu-uav-dds@pi.service \
  -u nycu-uav-topics@pi.service \
  -b --no-pager
```

如果 log 顯示 `Waiting for ... device`，代表該 USB 裝置尚未出現、設定中的
路徑寫錯，或執行帳號沒有讀寫權限。裝置稍晚上線時服務會自行繼續，不需要
手動重開；若有修改 `/etc/nycu-uav-offboard/config.env`，則必須重啟 target
才會重新載入設定。若持續顯示
`Serial open failed`，請檢查 by-id 路徑、`dialout` 權限，以及
ModemManager 是否占用裝置；節點本身仍會每 2 秒重試。

DDS client 連線後，以和服務相同的 ROS 環境確認資料真的流入：

```bash
export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source /home/pi/NYCU_ROS_WS/install/setup.bash

ros2 topic list | grep '^/fmu/'
timeout 10s ros2 topic echo \
  /fmu/out/vehicle_status \
  px4_msgs/msg/VehicleStatus \
  --qos-reliability best_effort --once

ros2 service call /gps_goto_status std_srvs/srv/Trigger '{}'
```

`topic list` 只代表 ROS graph 中有名稱；`topic echo --once` 收到一筆才代表
Pixhawk 至 RPi 的資料流正常。

## 4. 管理與修改

同時停止、啟動或重啟兩個服務：

```bash
sudo systemctl stop nycu-uav-offboard@pi.target
sudo systemctl start nycu-uav-offboard@pi.target
sudo systemctl restart nycu-uav-offboard@pi.target
```

更換 USB adapter、workspace 或 baud rate 時：

```bash
sudoedit /etc/nycu-uav-offboard/config.env
sudo systemctl restart nycu-uav-offboard@pi.target
```

取消開機自動啟動：

```bash
sudo systemctl disable --now nycu-uav-offboard@pi.target
```

更新 repo 或重新 `colcon build` 後，若只改 ROS node，可重啟 target。若有修改
`deploy/rpi` 內的 wrapper 或 unit，請重新執行一次安裝器。

## 5. 實機驗收

第一次測試必須拆槳，並確認獨立 RC/ELRS 接管、PX4 geofence 與 failsafe。
依 [GPS GOTO 操作指南](gps_goto_program.md)先通過 SITL 與桌面測試，再連續
重開 RPi 十次，確認：

- 兩個 service 都是 `active (running)`。
- DDS Agent 建立 PX4 client。
- `/fmu/out/vehicle_status` 每次都能收到資料。
- LR24 `PING` 與 `STATUS` 每次都有回覆。
- Pixhawk 與 LR24 的 by-id 從未互換。
