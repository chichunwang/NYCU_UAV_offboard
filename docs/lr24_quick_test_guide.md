# LR24-F 快速測試與系統啟動實測手冊

本文件是 **2026-07-20 以實機驗證過的最短測試路徑**：地面 Windows 筆電 ↔ 空中端 Raspberry Pi（LR24-F 一邊一顆）。所有指令都以當時實測的硬體為例，可直接複製貼上；換硬體時只需依 §0-3 重查一次埠名。

與其他文件的關係：

- 第一次接觸 LR24、想了解每個參數 → 先看 [LR24-F Link Test 使用教學](lr24_link_test_tutorial.md)
- 地面站完整操作（含 Ubuntu 地面端）→ [LR24-F 地面站控制完整教學](lr24_ground_station_tutorial.md)
- Pixhawk 接線、SITL、實機工程 → [GPS GOTO 操作指南](gps_goto_program.md)
- 本文件 → **照著打就能複現的驗證流程與預期輸出**，附實測踩雷排查表

---

## 0. 序列埠是什麼、要打什麼（本機實測值）

### 0-1. 名詞解釋

- **序列埠（serial port）**：LR24-F 或 USB 轉接器插上電腦後，作業系統會產生一個「埠」讓程式收發資料。**Windows 叫 `COM3`、`COM10` 這種；Linux 叫 `/dev/ttyUSB0`、`/dev/ttyUSB1` 這種。** 兩者是同一個概念，只是兩套命名。
- **`/dev/ttyUSBx` 的數字不可靠**：它是照「插入順序」編的，重開機或拔插後 0 和 1 **可能互換**。插錯埠＝指令打到 Pixhawk 轉接器去，對方永遠不會回話。
- **`/dev/serial/by-id/...`（穩定別名，正式指令一律用它）**：Linux 依「晶片出廠唯一序號」提供的固定路徑。路徑的組成是：

  ```text
  usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0
      └────────── 廠牌與晶片型號 ──────────┘└────────── 這顆晶片的唯一序號 ──────────┘└─ 介面編號 ─┘
  ```

  中間那串十六進位就是俗稱的「長序號」——**每一顆晶片出廠時燒死的、全世界唯一**。同一顆模組不管插哪個孔、重開幾次機，路徑都不變；但**換一顆 LR24-F 模組或換一台電腦，序號就不同，必須重查**（§0-3）。

### 0-2. 本機實測對應表（2026-07-20；換硬體後失效，要重查）

| 位置 | 裝置 | 要打的埠名（可直接複製） |
|---|---|---|
| 地面 Windows 筆電 | 地面端 LR24-F | `COM10` |
| Raspberry Pi | **空中端 LR24-F** | `/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0` |
| Raspberry Pi | Pixhawk TELEM2 轉接器 | `/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0` |

一眼分辨要訣：**LR24-F 用 CP2102N 晶片（序號很長一串）；Pixhawk 轉接器是 CP2102（序號只有 `0001`）**。

### 0-3. 換硬體後如何重查埠名

Pi 端（SSH 進去後）：

```bash
ls -l /dev/serial/by-id/
# 輸出範例（箭頭指向它目前對應到哪個 ttyUSB）：
# usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611...f5ab-if00-port0 -> ../../ttyUSB1
# usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0        -> ../../ttyUSB0
```

Windows 端：裝置管理員 →「連接埠 (COM 和 LPT)」→ 找 **「Silicon Labs CP210x USB to UART Bridge (COMx)」**，括號裡的 COMx 就是要打的埠。名稱寫「透過藍牙連結的標準序列」的都不是。

還是不確定哪個是 LR24 時，用原始資料實測分辨（不用猜）：

```bash
# Pi 端：兩埠都設好速率後讀原始 bytes；同時地面端持續發 PING
stty -F /dev/ttyUSB0 115200 raw -echo
stty -F /dev/ttyUSB1 115200 raw -echo
timeout 4 cat /dev/ttyUSB0 | od -c | head    # 哪個埠印出 ASCII 的 "PING,..." 哪個就是 LR24
timeout 4 cat /dev/ttyUSB1 | od -c | head
```

### 0-4. 建議打法：先把埠名設成變數

長路徑每次貼很累也容易貼錯。**在 Pi 上每開一個新 terminal，先貼這一行**，之後所有指令都用 `$LR24_SERIAL` 代替長路徑：

```bash
LR24_SERIAL=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0
```

（這跟 [GPS GOTO 操作指南](gps_goto_program.md) 的 `PIXHAWK_SERIAL=` / `LR24_SERIAL=` 是同一套慣例。變數只在當前 terminal 有效，開新視窗要重設。）

本文件以下所有 Pi 端指令都假設你已經設好 `$LR24_SERIAL`。

---

## 1. 鐵律：一個埠同時只能有一個程式

- **Windows 的 COM 埠是獨占的**：第二個程式開同一埠會直接報「存取被拒」，容易發現。
- **Linux 預設不擋**：兩個程式可同時開同一個埠，不會報錯，但收到的 bytes 會被兩個程式**隨機瓜分**，兩邊都解析失敗、時好時壞，極難排查。

所以在 Pi 上跑任何測試前，先確認沒有別的程式佔著 LR24：

```bash
pgrep -af 'lr24'              # 列出正在跑的 lr24_command_node 或 lr24_link_test.py
pkill -f lr24_command_node    # 需要時停掉節點（測完記得開回來）
```

---

## 2. 測試 A：鏈路健檢（`lr24_link_test.py`，不含 ROS）

目的：確認兩顆 LR24-F 的無線鏈路雙向都通、量掉包率與 RTT。**先照第 1 節停掉會佔埠的程式。**

### A-1. Pi 端啟動 responder（收 PING 回 PONG）

```bash
LR24_SERIAL=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0
python3 ~/NYCU_ROS_WS/src/NYCU_UAV_offboard/lr24_link_test.py responder --port "$LR24_SERIAL"
# 預期輸出：
# Responder listening on /dev/serial/by-id/... at 115200 bps. Press Ctrl+C to stop.
```

### A-2. Windows 端啟動 initiator（發 PING 量測）

```powershell
# 在 repo 目錄下（例如 C:\Users\user\NYCU_UAV_offboard）：
python lr24_link_test.py initiator --port COM10 --count 30 --interval 0.5 --payload 32
```

參數意義：`--count 30` 共發 30 個封包、`--interval 0.5` 每 0.5 秒一發、`--payload 32` 每包夾帶 32 bytes 資料。

### A-3. 實測預期結果（2026-07-20 室內近距）

```text
seq=0001  RTT=   47.00 ms      ← 每一行代表一個封包來回成功，RTT=來回耗時
...
=== LR24 Link Test Summary ===
Sent:       30
Received:   30
Lost:       0 (0.00%)          ← 掉包率
RTT average:44.50 ms
```

| 項目 | 本次實測 | 合格線（Link Test 教學 §12） |
|---|---:|---:|
| 掉包率 | 0 % | < 2 % |
| RTT 平均 | 44.5 ms | < 300 ms |
| RTT 最大 | 47 ms | P95 < 500–1000 ms |

### A-4. 角色互換（完整雙向驗證）

把兩邊角色對調再跑一次——Windows 改跑 responder：

```powershell
python lr24_link_test.py responder --port COM10
```

Pi 改跑 initiator：

```bash
python3 ~/NYCU_ROS_WS/src/NYCU_UAV_offboard/lr24_link_test.py initiator \
  --port "$LR24_SERIAL" --count 30 --interval 0.5 --payload 32
```

實測反向結果：30/30、0% 掉包、RTT 平均 39.5 ms。**兩方向都過才算鏈路健檢通過**（可同時驗證兩端 USB、上下行是否對稱）。

---

## 3. 測試 B：正式指令管線（`lr24_command_node` + `send_lr24_command.py`）

目的：驗證正式的框架協定（`$CMD,seq,...*checksum`）整條路。此測試**不需要 Pixhawk**。

### B-1. Pi 端啟動空中指令節點

```bash
source /opt/ros/jazzy/setup.bash
source ~/NYCU_ROS_WS/install/setup.bash
LR24_SERIAL=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0
ros2 run my_offboard_cpp lr24_command_node --ros-args \
  -p port:="$LR24_SERIAL" \
  -p baud_rate:=115200
# 預期 log：
# [INFO] [...] [lr24_command_node]: LR24 command node listening on ... at 115200 baud.
```

兩行 `source` 的意義：第一行載入 ROS 2 Jazzy 本體，第二行載入我們自己 colcon build 出來的
`my_offboard_cpp`。**少 source 任何一行，`ros2 run` 會找不到套件**（`Package 'my_offboard_cpp' not found`）。

節點啟動瞬間會主動向地面發一筆開機通告（地面若在收原始資料可看到）：

```text
$STAT,0,BOOT,LR24 command node ready*2C
```

### B-2. 地面 Windows 發 PING / HELP

```powershell
python tools\send_lr24_command.py --port COM10 PING
# 預期：
# > $CMD,<seq>,PING*XX            ← 「>」是我們送出的框架
# < $ACK,<seq>,PING,PONG*XX       ← 「<」是空中端的回覆；秒回、seq 原樣回帶、checksum 工具已驗過

python tools\send_lr24_command.py --port COM10 HELP
# < $ACK,<seq>,HELP,PING ENABLE_STREAM START_MISSION START_OFFBOARD STOP_OFFBOARD LAND RTL ABORT GOTO GOTO_AMSL STATUS*XX
```

框架各欄位的意義：`$` 開頭、`CMD`＝方向（地面→空中）、`<seq>`＝流水號（工具自動用奈秒時間戳）、
`*XX`＝checksum（`$` 到 `*` 之間所有字元的 XOR，防雜訊）。`PING`/`HELP` 由節點就地回覆、
不經任何 ROS service，是驗證「無線鏈路＋框架協定」最乾淨的指令。

> 此時若送 `STATUS` / `GOTO`，會等約 7 秒後回 `ERR ... ROS service not available` 或 timeout——
> 這是**預期行為**（`global_goto_node` 還沒啟動），不是鏈路壞掉。

---

## 4. 完整系統啟動（GPS GOTO 流程）

前提：Pixhawk 已依 [GPS GOTO 操作指南](gps_goto_program.md) §3 設好 TELEM2 uXRCE-DDS 參數、接上 Pi 的 CP2102 轉接器。**未過 SITL 前不得實飛（README 測試規範）。**

空中端依序開三個 terminal（或用 tmux；每個 terminal 都要重設變數與 source）：

```bash
# ── Terminal 1：DDS Agent（Pixhawk ↔ ROS 2 的橋）──
PIXHAWK_SERIAL=/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
MicroXRCEAgent serial --dev "$PIXHAWK_SERIAL" -b 921600
# 注意：-b 921600 是 Pixhawk TELEM2 的速率（SER_TEL2_BAUD），跟 LR24 的 115200 無關

# ── Terminal 2：確認 PX4 topics 有出現 ──
export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source ~/NYCU_ROS_WS/install/setup.bash
ros2 topic list | grep '^/fmu/'
# v1.17.0 的 topic 名稱「沒有」_v1 後綴；若看到 vehicle_status_v1 代表韌體版本不對（見 §5）
ros2 topic echo /fmu/out/vehicle_status px4_msgs/msg/VehicleStatus --qos-reliability best_effort --once

# ── Terminal 3：GPS GOTO launch（同時啟動 global_goto_node + lr24_command_node）──
export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source ~/NYCU_ROS_WS/install/setup.bash
LR24_SERIAL=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0611199860e7ed11b2b7d7770b2af5ab-if00-port0
ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:="$LR24_SERIAL" \
  lr24_baud_rate:=115200
```

地面端依序驗證（每一步過了才做下一步）：

```powershell
# 1. 鏈路通不通
python tools\send_lr24_command.py --port COM10 PING

# 2. 讀飛行整備狀態；重點看回覆內容裡的 ready_for_goto 與 readiness_reason
python tools\send_lr24_command.py --port COM10 STATUS

# 3. 只有 ready_for_goto=true（已 arm、已離地、GPS 合格）才可送目標
#    GOTO 緯度 經度 相對Home高度(m)
python tools\send_lr24_command.py --port COM10 GOTO 24.78694 120.99762 80

# 4. 返航
python tools\send_lr24_command.py --port COM10 RTL
```

`STATUS` 回覆是 ACK 且 `success` 恆為 true；**能不能送 GOTO 要看內容裡的 `ready_for_goto=true|false`**，false 時讀 `readiness_reason` 找原因。`GOTO` 的高度是「相對 Home」，要用海拔請改 `GOTO_AMSL`（兩者都受 30–120 m 相對高度限制）。

---

## 5. 踩雷排查表（實測案例）

### 症狀：`No matching checksummed response before timeout.`

依序檢查四件事：

| # | 原因 | 檢查方式 |
|---|---|---|
| 1 | **跑錯邊**：`send_lr24_command.py` 是地面站工具，在 Pi 上跑等於方向顛倒 | 指令從地面電腦發；Pi 上跑的應該是 `lr24_command_node` |
| 2 | **埠錯**：沒帶 `--port` 時預設 `/dev/ttyUSB0`，在本機那是 Pixhawk 轉接器 | 一律明確帶 `--port`（Windows `COM10`、Pi 用 §0-2 的 by-id 路徑） |
| 3 | **對面沒人聽**：空中端節點沒啟動 | SSH 進 Pi 打 `pgrep -af lr24_command_node` |
| 4 | **埠被瓜分**：Pi 上兩個程式同時開同一埠（Linux 不報錯！） | `pgrep -af 'lr24'`，多殺到只剩一個 |

（實例：在 Pi 上直接打 `python3 .../send_lr24_command.py ping` 同時踩中 1+2+3。）

### 其他已知注意事項

- **ACK timeout ≠ 指令沒執行**。序號重送機制：同一個 sequence 重送會拿到**快取的原回覆**，
  所以 timeout 後想確認結果，用 `--seq <原序號>` 重送同一筆，不要直接重打（新 seq = 新指令）。
- **弱訊號下可能出現「假 TIMEOUT」**：Python 工具用 `readline()`＋短 timeout，封包若在無線重傳中
  被切成兩段（間隔 >200ms），兩段都會解析失敗而整包丟棄（空中端 C++ 節點有緩衝重組、無此問題）。
  近距實測 0% 掉包不受影響；距離測試看到零星 TIMEOUT 時先想到這點，配合 `--seq` 重查。
- **PX4 版本綁定**：本專案程式碼訂閱的是 v1.17.0 的無後綴 topic（`/fmu/out/vehicle_status` 等，
  已比對官方 `dds_topics.yaml`）。PX4 main／未來版導入 message versioning 後部分 topic 會變成
  `..._v1`；**若升級韌體，節點會靜默收不到任何資料**（安全面 fail-safe，但系統完全不動作），
  升版前必須同步改程式與重驗。
- **舊流程（`serial_elrs_offboard.launch.py`）兩個雷**：
  (1) launch 的 `port` 預設 `/dev/ttyUSB0`，在本機是 Pixhawk 轉接器——**一定要帶 `port:=`**；
  (2) 該流程沒有 `global_goto_node`，LR24 送 `RTL`/`ABORT` 只會得到
  `ERR ... ROS service not available`——舊流程的中止手段是 `LAND`／`STOP_OFFBOARD`／RC 接管。

---

## 6. 本次實測驗收紀錄（2026-07-20）

- [x] 鏈路健檢 地面→空中：30/30、0%、RTT avg 44.5 ms
- [x] 鏈路健檢 空中→地面：30/30、0%、RTT avg 39.5 ms
- [x] 節點開機通告 `$STAT,0,BOOT,...` 地面實收
- [x] 正式框架 `PING`→`ACK,PONG`、`HELP`→指令表
- [x] 框架單元測試 `python -m pytest test/`：16 passed
- [ ] Pixhawk uXRCE-DDS 橋接（`/fmu/` topics）
- [ ] SITL `GOTO`/`RTL` 全流程
