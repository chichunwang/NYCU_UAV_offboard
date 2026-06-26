# feature/lr24-communication

此分支用於開發 **地面站（Ground Control Station）與 Jetson Orin 之間的通訊模組**。

目前使用 **MicoAir LR24-F** 作為無線通訊設備。LR24-F 以 USB Serial 連接兩端電腦，目前主架構採用 **自訂 Serial 指令協定**，由地面站送低頻控制指令給 Jetson Orin，再由 Orin 本機呼叫 ROS 2 service 與 PX4 Offboard topic。

RC/ELRS 保留為獨立控制鏈路，用於手動飛行、切換 PX4 flight mode、arm/disarm 與緊急接管。

## 功能目標

- 建立地面站與 Jetson Orin 之間的無線通訊
- 傳輸飛行任務、GPS 座標或控制指令
- 回傳 Jetson Orin 的狀態與處理結果
- 為後續飛機追蹤與板外控制功能提供通訊介面

## 目前使用規格

| 項目 | 目前設定 |
|---|---|
| 通訊模組 | MicoAir LR24-F |
| 空中端設備 | Jetson Orin |
| 地面端設備 | 地面站ubuntu電腦 |
| 通訊方式 | USB Serial 自訂指令協定 |
| Serial Baud Rate | 115200 bps |
| USB Baud Rate | 115200 bps |
| 無線傳輸速率 | 8 kbps |
| 遠端控制方式 | LR24-F Serial 指令觸發 Orin ROS 2 service |
| 備用測試方式 | PPP / SSH，可參考 `docs/ppp_lr24_ssh_offboard.md` |
| 發射功率 | TBD |
| 最大發射功率 | 500 mW |

> 注意：115200 bps 為設備與電腦之間的串列埠速率，8 kbps 為 LR24-F 的實際無線傳輸速率。  
> 因此程式傳送資料時，必須避免資料量超過無線鏈路可承受的頻寬。

## 系統架構

```text
地面站電腦
    │
    │ USB Serial，115200 bps
    ▼
地面端 LR24-F
    │
    │ 無線連線，8 kbps
    ▼
空中端 LR24-F
    │
    │ USB Serial，115200 bps
    ▼
Jetson Orin
    │
    │ lr24_command_node
    │ ROS 2 service
    ▼
my_offboard_node
    │
    │ /fmu/in/offboard_control_mode
    │ /fmu/in/trajectory_setpoint
    │ /fmu/in/vehicle_command
    ▼
PX4
```

## Offboard 控制入口

目前提供兩種入口：

1. **RC/ELRS 切模式：** Orin 先持續發布 Offboard setpoint，飛手用 RC/ELRS 將 PX4 切入 Offboard。
2. **LR24 指令切模式：** 地面站送 `START_OFFBOARD`，Orin warmup 後送 PX4 Offboard mode 與 arm 指令。

Orin 端啟動：

```bash
ros2 launch my_offboard_cpp serial_elrs_offboard.launch.py port:=/dev/ttyUSB0 baud_rate:=115200 stream_on_start:=true
```

地面站送指令：

```bash
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 STATUS
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 ENABLE_STREAM
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 START_MISSION
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 START_OFFBOARD
python3 tools/send_lr24_command.py --port /dev/ttyUSB0 LAND
```

可用 service：

| Service | 功能 |
|---|---|
| `/start_offboard` | 開始 Offboard sequence，warmup 後切 Offboard 並 arm |
| `/enable_offboard_stream` | 只發布 Offboard setpoint，讓 RC/ELRS 可以切入 Offboard |
| `/start_mission` | 開始任務軌跡，不切模式、不 arm |
| `/stop_offboard` | 停止發布 Offboard setpoint |
| `/land` | 送 PX4 land 指令，並停止發布 Offboard setpoint |
| `/offboard_status` | 回傳 Offboard 節點狀態 |

詳細使用方式請看：

- [docs/serial_elrs_offboard.md](docs/serial_elrs_offboard.md)
- [docs/ppp_lr24_ssh_offboard.md](docs/ppp_lr24_ssh_offboard.md)

## 設計原則

- LR24-F 的 8 kbps 無線鏈路只傳低頻指令與少量狀態文字。
- Offboard setpoint 必須由 Orin 本機持續送給 PX4。
- 不建議把高頻 ROS topic、影像或大量 log 經 LR24-F 傳輸。
- ELRS 或其他獨立 RC 鏈路必須保留，用於手動接管與緊急保護。
