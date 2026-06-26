# LR24-F Serial 指令協定與 RC/ELRS Offboard 操作

本專案目前建議的實機控制架構是：

```text
地面筆電
    │
    │ USB Serial, 115200 bps
    ▼
地面端 LR24-F
    │
    │ 無線鏈路, 8 kbps
    ▼
空中端 LR24-F
    │
    │ USB Serial, 115200 bps
    ▼
Jetson Orin / lr24_command_node
    │
    │ ROS 2 service
    ▼
my_offboard_node
    │
    │ PX4 /fmu/in/*
    ▼
Pixhawk / PX4
```

RC/ELRS 保留為獨立安全鏈路：

```text
ELRS 遙控器
    ↓
ELRS 接收機
    ↓
Pixhawk / PX4
```

LR24-F 只送低頻指令，例如啟用 Offboard stream、開始任務、降落或查詢狀態。Offboard setpoint 永遠由 Orin 本機持續發布，不從地面站即時傳送。

## Orin 端啟動

先建置：

```bash
colcon build --packages-select my_offboard_cpp
source install/setup.bash
```

確認 LR24-F 的 serial port：

```bash
ls /dev/ttyUSB*
dmesg -w
```

一次啟動 Offboard 節點與 LR24 指令節點：

```bash
ros2 launch my_offboard_cpp serial_elrs_offboard.launch.py port:=/dev/ttyUSB0 baud_rate:=115200 stream_on_start:=true
```

`stream_on_start:=true` 代表 Orin 一啟動就會開始發布 Offboard setpoint，但不會自動切 PX4 模式，也不會自動 arm。這是為了讓 RC/ELRS 切 Offboard 時，PX4 已經先收到超過 1 秒的 Offboard 訊號。

如果你想完全由 LR24 指令啟用 stream，可以改成：

```bash
ros2 launch my_offboard_cpp serial_elrs_offboard.launch.py port:=/dev/ttyUSB0 baud_rate:=115200 stream_on_start:=false
```

## 地面站送指令

地面站安裝 pyserial：

```bash
sudo apt install python3-serial
```

用 repo 內的工具送指令：

```bash
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 PING
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 STATUS
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 ENABLE_STREAM
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 START_MISSION
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 LAND
```

也可以先用簡單文字模式測鏈路：

```bash
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 --simple PING
```

正式封包會長這樣：

```text
$CMD,123,START_OFFBOARD*CS
```

其中 `CS` 是 payload `CMD,123,START_OFFBOARD` 的 XOR checksum，十六進位兩碼。Orin 回覆：

```text
$ACK,123,START_OFFBOARD,message*CS
$ERR,123,START_OFFBOARD,reason*CS
```

## 指令表

| 指令 | 對應 ROS 2 service | 用途 |
|---|---|---|
| `PING` | 無 | 測試 LR24-F 雙向通訊 |
| `STATUS` | `/offboard_status` | 查詢 Orin Offboard 節點狀態 |
| `ENABLE_STREAM` | `/enable_offboard_stream` | 只啟用 Offboard setpoint stream，不切模式、不 arm |
| `START_MISSION` | `/start_mission` | 開始任務軌跡，但不切模式、不 arm |
| `START_OFFBOARD` | `/start_offboard` | 啟用 stream 和任務，約 1 秒後由 Orin 發 PX4 Offboard mode + arm |
| `STOP_OFFBOARD` | `/stop_offboard` | 停止發布 Offboard setpoint |
| `LAND` | `/land` | 送 PX4 land 指令，並停止發布 Offboard setpoint |

## 推薦操作流程 A：RC/ELRS 切 Offboard

這是比較推薦的實機流程，飛手保留最大控制權。

1. Orin 啟動 launch，使用 `stream_on_start:=true`。
2. 地面站送 `STATUS`，確認 `stream_active=true`。
3. 飛手用 ELRS arm，並用 RC flight mode switch 切到 PX4 Offboard。
4. 確認飛機已進入 Offboard 且狀態穩定。
5. 地面站送 `START_MISSION`，Orin 開始任務軌跡。
6. 若要中止，飛手先用 RC 切回 Manual / Stabilized / Position 等安全模式。
7. 切回 RC 安全模式後，再送 `STOP_OFFBOARD` 或 `LAND`。

這條流程中，Orin 不會幫你切模式或 arm。模式切換與解鎖都由 RC/ELRS 完成。

## 操作流程 B：LR24 指令直接切 Offboard

這條流程比較適合 SITL、拆槳地面測試或受控測試環境。

```bash
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 START_OFFBOARD
```

Orin 收到後會：

1. 開始發布 Offboard setpoint
2. 開始任務軌跡
3. 等約 1 秒 Offboard warmup
4. 對 PX4 送 Offboard mode 指令
5. 對 PX4 送 arm 指令

實機若使用此流程，必須先確認 RC/ELRS 能立即接管。

## 安全注意事項

- 不要從地面站透過 LR24-F 傳高頻 setpoint。
- 不要在飛行中直接 `STOP_OFFBOARD`，除非 PX4 已經切回安全模式或你確定 failsafe 行為。
- LR24-F 斷線時，PX4 必須靠 RC/ELRS、Offboard loss failsafe 或其他安全邏輯接管。
- 初次測試請用 SITL 或拆槳測試。
- 固定翼機的 Offboard setpoint 與任務軌跡需要依實際飛控參數和任務型態重新設計，目前程式內的座標只是範例。
