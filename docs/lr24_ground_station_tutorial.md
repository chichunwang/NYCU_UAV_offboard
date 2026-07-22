# LR24-F 地面站控制完整教學

這份教學只回答一件事：**人在地面端時，如何透過 LR24-F 輸入 GPS 座標，讓空中的 Orin / RPi 把目標交給 Pixhawk。**

如果 Pixhawk TELEM2、ROS 2、Micro XRCE-DDS Agent 尚未安裝完成，先依照 [LR24 GPS GOTO 與 Pixhawk 連線操作指南](gps_goto_program.md) 完成空中端設定。

> 首次測試一律拆槳。完成 SITL、桌面通訊、PX4 geofence、failsafe 與獨立 RC/ELRS 接管驗證前，不可實飛。

## 1. 先弄清楚：指令在哪裡輸入

目前的地面控制介面不是 QGroundControl 按鈕，也不是搖桿。你要在地面筆電的 **PowerShell 或 Terminal** 執行：

```text
send_lr24_command.py
```

每執行一次，就透過 LR24 傳送一筆低頻指令，例如：

- `PING`：測試兩顆 LR24 是否雙向相通。
- `STATUS`：讀取 Pixhawk、GPS 與飛行狀態。
- `GOTO`：送出一個 GPS 目標和相對 Home 高度。
- `GOTO_AMSL`：送出一個 GPS 目標和 AMSL 海拔。
- `RTL`：要求 PX4 進入 Return mode。
- `ABORT`：目前是 `RTL` 的別名，不是 Hold、Kill 或立即停止飛機。

地面筆電不需要 ROS 2，也不需要執行 Micro XRCE-DDS Agent。地面端只需要：

1. Python 3.10 以上。
2. `pyserial`。
3. 本專案的 `tools/send_lr24_command.py`。
4. 一顆已配對的 LR24-F。

如果想用按鈕操作，可在地面端改為啟動圖形介面：

```powershell
py .\tools\send_lr24_command_ui.py
```

介面可自動掃描 COM / serial port，提供 PING、STATUS、GOTO、GOTO_AMSL、RTL 等按鈕，並在通訊紀錄中顯示送出的 checksummed frame 與 ACK／ERR。飛航相關按鈕送出前會再次確認；其封包格式、8 秒 timeout 與安全限制和本文件下方的命令列工具相同。

Windows 官方 Python 通常已包含 Tkinter；若 Ubuntu 顯示 Tkinter unavailable，先執行 `sudo apt install python3-tk`。

### 1.1 操作員指令速查

UI 會顯示所有 LR24 指令，但指令是否可用取決於空中端啟動的流程：

- **GPS GOTO 主流程**：空中端使用 `serial_gps_goto.launch.py`，主要操作是 `PING`、`STATUS`、`GOTO`、`GOTO_AMSL`、`RTL`。
- **ELRS／Offboard 範例流程**：空中端使用 `serial_elrs_offboard.launch.py`，主要操作是 `ENABLE_STREAM`、`START_MISSION`、`START_OFFBOARD`、`STOP_OFFBOARD`、`LAND`。

如果按下不屬於目前流程的按鈕，通常會收到 `ERR ... ROS service not available`。UI 不會自動判斷空中端啟動了哪一種 launch。

| 指令／UI 按鈕 | 參數 | 適用流程 | 用途與操作注意事項 |
|---|---|---|---|
| `PING` | 無 | 兩者皆可 | 測試地面端到空中端的雙向 LR24 通訊；正常回覆為 `ACK ... PONG`。每次操作都應先測試它。 |
| `HELP` | 無 | 兩者皆可 | 由空中端回傳支援的指令名稱；只代表 `lr24_command_node` 認得名稱，不代表相關 ROS service 已啟動。 |
| `STATUS` | 無 | 兩者皆可 | 查詢目前流程的狀態。GPS GOTO 流程必須讀取 `ready_for_goto`，不能只看 `ACK`。 |
| `GOTO` | 緯度、經度、相對 Home 高度（m） | GPS GOTO | 將固定翼送往 GPS 目標並進入盤旋；送出前必須確認 `ready_for_goto=true`。 |
| `GOTO_AMSL` | 緯度、經度、AMSL 海拔（m） | GPS GOTO | 與 `GOTO` 相同，但高度基準為平均海平面；空中端仍會換算並套用相對 Home 高度限制。 |
| `RTL` | 無 | GPS GOTO | 要求 PX4 進入 `AUTO_RTL`；實際返航高度、盤旋和是否降落由 PX4 參數決定。 |
| `ABORT` | 無 | GPS GOTO | 目前是 `RTL` 的別名；不是 Hold、Kill 或立即停止飛機。 |
| `ENABLE_STREAM` | 無 | ELRS／Offboard | 開始發布 Offboard setpoint，但不切模式、不 arm。 |
| `START_MISSION` | 無 | ELRS／Offboard | 開始任務軌跡，但不切模式、不 arm。 |
| `START_OFFBOARD` | 無 | ELRS／Offboard | 啟用 stream 和任務，warmup 後要求 PX4 切入 Offboard 並 arm；只應用於 SITL、拆槳或已驗證 RC 接管的受控環境。 |
| `STOP_OFFBOARD` | 無 | ELRS／Offboard | 停止發布 Offboard setpoint；飛行中直接使用可能觸發 Offboard loss 行為，應先由 RC 切回安全模式。 |
| `LAND` | 無 | ELRS／Offboard | 要求 PX4 降落並停止發布 Offboard setpoint；固定翼實飛前必須先驗證 PX4 的降落設定與航線。 |

命令列的一般格式如下；`COM7` 要換成地面端 LR24 的實際 serial port：

```powershell
py .\tools\send_lr24_command.py --port COM7 <指令> [參數]
```

常用命令可直接照以下格式輸入：

```powershell
# 通訊與狀態
py .\tools\send_lr24_command.py --port COM7 PING
py .\tools\send_lr24_command.py --port COM7 HELP
py .\tools\send_lr24_command.py --port COM7 STATUS

# GPS GOTO 主流程
py .\tools\send_lr24_command.py --port COM7 GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
py .\tools\send_lr24_command.py --port COM7 GOTO_AMSL LAT_DEG LON_DEG ALT_AMSL_M
py .\tools\send_lr24_command.py --port COM7 RTL

# ELRS／Offboard 範例流程
py .\tools\send_lr24_command.py --port COM7 ENABLE_STREAM
py .\tools\send_lr24_command.py --port COM7 START_MISSION
py .\tools\send_lr24_command.py --port COM7 START_OFFBOARD
py .\tools\send_lr24_command.py --port COM7 STOP_OFFBOARD
py .\tools\send_lr24_command.py --port COM7 LAND
```

`LAT_DEG`、`LON_DEG` 和高度欄位必須換成實際數值；目標必須位於已驗證的 geofence 內，並符合空中端設定的距離與高度限制。

### 1.2 UI 操作順序

1. 接好地面端 LR24 天線和 USB，關閉其他會占用同一個 COM port 的程式。
2. 執行 `py .\tools\send_lr24_command_ui.py`。
3. 按「重新掃描」，選擇地面端 LR24 的 serial port；Baud 必須與兩顆 LR24 和空中端程式一致，Timeout 一般保持 `8.0` 秒。
4. 按 `PING`，確認通訊紀錄出現 `< $ACK,...,PING,PONG...`。
5. 按 `STATUS`，依目前流程確認狀態；GPS GOTO 操作必須明確看到 `ready_for_goto=true`。
6. 若要送 GPS 目標，依序填入緯度、經度、高度，再根據高度基準選擇 `GOTO` 或 `GOTO_AMSL`。
7. 核對確認視窗中的 port、指令與參數後才送出；收到 `ACK` 只代表指令已接受，仍要持續監看 QGC、RC 狀態和飛機實際動作。
8. 若收到 `ERR`，先依訊息修正狀態；若 timeout，不可假設指令未執行，也不要直接重送飛航指令，應依第 9 節處理。

### 1.3 通訊紀錄判讀

| 顯示 | 意義 | 操作員處置 |
|---|---|---|
| `> $CMD,...` | 地面程式已將封包寫入 serial port | 等待相同 sequence 的回覆。 |
| `< $ACK,...` | 空中端接受指令，或對應 ROS service 回報成功 | 繼續確認回覆內容與飛機實際狀態；`STATUS ACK` 不等於 `ready_for_goto=true`。 |
| `< $ERR,...` | 空中端拒絕指令或 service 執行失敗 | 讀取最後面的原因，修正後先重新查詢 `STATUS`。 |
| `No matching...`／UI 顯示逾時 | 期限內沒有收到相同 sequence 的 ACK/ERR | 對飛航指令視為「結果未知」；不要直接重送，改用 QGC／RC 確認並依第 9 節處理。 |

QGroundControl 可以另外用來看地圖、模式與遙測，但它不是這條 LR24 指令鏈的一部分，也不能占用 LR24 的 COM / serial port。若要讓 QGC 顯示實際飛機狀態，必須另外準備一條已設定好的 MAVLink 路徑，例如另一個 Pixhawk telemetry port／遙測電台、Pixhawk USB，或網路 MAVLink。供 uXRCE-DDS 使用的 TELEM2 與這顆傳送自訂文字指令的 LR24 都不能同時拿給 QGC 使用。

## 2. 完整資料路徑

```text
地面筆電 PowerShell / Terminal
    │
    │ 執行 send_lr24_command.py
    ▼
地面端 LR24-F（USB serial）
    │
    │ 透明雙向無線鏈路
    ▼
空中端 LR24-F（USB serial）
    │
    ▼
Orin / RPi：lr24_command_node
    │ ROS 2 service
    ▼
Orin / RPi：global_goto_node
    │ /fmu/in/vehicle_command
    ▼
Micro XRCE-DDS Agent
    │ 獨立的 Pixhawk serial port
    ▼
Pixhawk / PX4
    │
    ▼
固定翼飛機
```

必須特別注意：

- 地面端 LR24 只接地面筆電，不接 Pixhawk。
- 空中端 LR24 接 Orin / RPi。
- Pixhawk TELEM2 透過另一個 USB-UART 接 Orin / RPi。
- 空中端的 LR24 與 Pixhawk 必須是兩個不同的 serial device。
- LR24 只送一次性低頻命令，不會持續傳送搖桿或 Offboard setpoint。

## 3. 兩顆 LR24-F 的前置設定

LR24 的 radio 設定不由本專案程式管理。原廠說明指出，兩顆使用出廠預設值的 LR24-F 通電後會自動 bind，連線成功時綠燈恆亮；若需要修改參數，使用原廠 [MicoAssistant](https://micoair.com/assistant/)。Windows 若未辨識 USB serial，從原廠 [Download 頁面](https://micoair.com/downloads/) 安裝 LR24-F/P 使用的 CP210x driver。完整硬體規格與原廠設定畫面可參考 [LR24-F 官方產品／操作頁](https://micoair.com/radio_telemetry_lr24f/)。

MicoAssistant 網頁版需使用 Chrome 或 Edge：先勾選／啟用 `Radio Config`，再選擇 LR24 的 COM port、baud rate 並連線。出廠 USB interface 預設通常是 `57600`；目前本專案指令範例使用 `115200`，所以兩端必須選擇同一種做法：

1. 用 MicoAssistant 將兩顆 LR24 的 USB UART baud 都設成 `115200`，儲存並重新上電；之後照本文件原指令使用。
2. 保留兩顆的 `57600`，但空中 launch 必須加 `lr24_baud_rate:=57600`，地面每個命令也必須加 `--baud 57600`。

不要只改其中一顆或只改程式一端。設定工具顯示的實際值優先於對出廠值的假設。

如果選擇保留 `57600`，兩端指令形式如下：

```bash
# 空中 Orin / RPi
ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:="$LR24_SERIAL" lr24_baud_rate:=57600
```

```powershell
# Windows 地面端；PING、STATUS、GOTO、RTL 都要保留 --baud 57600
py .\tools\send_lr24_command.py --port COM7 --baud 57600 PING
```

兩顆 LR24 必須確認：

| 項目 | 設定 |
|---|---|
| 工作模式 | Transparent Serial／透明串列傳輸 |
| USB UART baud rate | 兩端與程式一致；本文件主流程使用 `115200` bps |
| 無線傳輸速率 | 兩端一致；目前文件以約 `8 kbps` 為基準 |
| 頻道／頻率 | 兩端一致 |
| Network ID | 兩端一致 |
| 天線 | 通電前先接好 |
| 桌面測試 | 低發射功率、相距約 1～2 公尺 |

USB UART 的 `57600`／`115200 bps` 是電腦和 LR24 之間選定的速率，不等於無線鏈路的傳輸速率。

完成設定後，關閉 MicoAssistant 或任何 serial terminal。一般情況下，同一個 COM / serial port 同一時間只能由一個程式開啟。

如果尚未確認透明鏈路是否可靠，先完成 [LR24-F Link Test 使用教學](lr24_link_test_tutorial.md)。

## 4. 空中端每次開機都要執行的程序

目前專案尚未建立 systemd 自動啟動服務，因此 Orin / RPi 每次重新開機後，都要確認以下兩個程序正在執行。

### 4.1 Terminal A：Pixhawk DDS Agent

先確認 Pixhawk 使用的 serial device：

```bash
ls -l /dev/serial/by-id/
```

設定正確路徑並啟動 Agent：

```bash
PIXHAWK_SERIAL=/dev/serial/by-id/REPLACE_WITH_PIXHAWK_UART_ADAPTER
MicroXRCEAgent serial --dev "$PIXHAWK_SERIAL" -b 921600
```

這個 Terminal 必須持續開著。看到 PX4 client 建立後，再進行下一步。

### 4.2 Terminal B：GPS GOTO 與空中端 LR24 node

這裡的 `LR24_SERIAL` 必須是空中端 LR24，不是上面的 Pixhawk serial device：

```bash
NYCU_ROS_WS=/absolute/path/to/nycu_ros_ws
LR24_SERIAL=/dev/serial/by-id/REPLACE_WITH_AIRBORNE_LR24

export ROS_DOMAIN_ID=0
source /opt/ros/jazzy/setup.bash
source "$NYCU_ROS_WS/install/setup.bash"

ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
  lr24_port:="$LR24_SERIAL" \
  lr24_baud_rate:=115200
```

正確啟動時應看到類似訊息：

```text
Fixed-wing global goto node started
LR24 command node listening on ... at 115200 baud
```

若看到：

```text
Failed to open serial port
```

表示空中端 LR24 路徑錯誤、權限不足或被其他程式占用。修正後必須重新啟動 launch；目前 node 不會在背景自動重開 serial port。

### 4.3 空中端快速自我檢查

在另一個已 source ROS workspace 的 Terminal 執行：

```bash
ros2 service call /gps_goto_status std_srvs/srv/Trigger '{}'
```

即使尚未 arm 或尚未起飛，service 也應該存在。此時 `ready_for_goto=false` 是正常的，重點是能看到明確的 `readiness_reason`。

## 5. Windows 地面站操作

以下以目前專案路徑為例：

```text
C:\Users\ricky\Downloads\Project\NYCU_UAV_offboard
```

### 5.1 接上地面端 LR24

1. 先接好 LR24 天線。
2. 用 USB 將地面端 LR24 接到 Windows 筆電。
3. 關閉 LR24 設定工具、QGroundControl serial 自動連線、PuTTY、Arduino Serial Monitor 等可能占用 COM port 的程式。

### 5.2 安裝 Python 與 pyserial

開啟 PowerShell：

```powershell
py --version
py -m pip install pyserial
```

如果電腦沒有 `py` 指令，但有 `python`，可改成：

```powershell
python --version
python -m pip install pyserial
Set-Alias -Name py -Value python
```

最後一行會在目前 PowerShell 視窗把 `py` 指向 `python`，因此後續教學可以繼續照抄 `py ...`。若重新開一個 PowerShell 視窗且仍沒有 Python Launcher，要重新設定 alias，或把後續所有 `py` 改成 `python`。

### 5.3 找出地面端 LR24 的 COM port

執行：

```powershell
py -m serial.tools.list_ports -v
```

也可以從「裝置管理員 → 連接埠（COM 和 LPT）」查看。假設看到：

```text
USB Serial Port (COM7)
```

後續就使用：

```text
--port COM7
```

`COM10`、`COM11` 等大於 9 的名稱也可直接交給 pyserial，不需要自行改成其他格式。

### 5.4 切換到專案目錄

```powershell
Set-Location 'C:\Users\ricky\Downloads\Project\NYCU_UAV_offboard'
```

確認工具存在：

```powershell
Get-Item .\tools\send_lr24_command.py
```

### 5.5 第一步只送 PING

飛機保持拆槳，執行：

```powershell
py .\tools\send_lr24_command.py --port COM7 PING
```

輸出格式應類似：

```text
> $CMD,<sequence>,PING*<checksum>
< $ACK,<sequence>,PING,PONG*<checksum>
```

實際的 sequence 與 checksum 每次都不同。判讀方式是：

- 第一行 `>`：地面工具已經把命令寫入地面 LR24。
- 第二行 `< ... ACK ... PONG`：空中端確實收到命令，而且回覆已經穿過 LR24 回到地面。
- `No matching checksummed response before timeout`：目前還不能送飛行指令，先依第 10 節排除問題。

`PING` 不需要有效 GPS，也不需要 Pixhawk ready；但空中端 `lr24_command_node` 必須正在執行。

### 5.6 第二步查詢 STATUS

```powershell
py .\tools\send_lr24_command.py --port COM7 STATUS
```

起飛前常見結果會包含：

```text
state=IDLE
ready_for_goto=false
armed=false
landed=true
readiness_reason=...
```

這不是 LR24 故障，而是安全 gate 正確拒絕地面狀態的 GOTO。

`STATUS` 本身會回 `ACK`，即使 `ready_for_goto=false`。所以不能只看 `ACK`，一定要讀取回覆中的 `ready_for_goto`。

## 6. Ubuntu 地面站操作

地面端 Ubuntu 不需要安裝 ROS 2，只需要 Python 工具：

```bash
sudo apt update
sudo apt install -y python3 python3-serial
```

找出地面端 LR24：

```bash
python3 -m serial.tools.list_ports -v
ls -l /dev/serial/by-id/
```

建議使用固定的 `/dev/serial/by-id/...` 路徑：

```bash
GROUND_LR24_SERIAL=/dev/serial/by-id/REPLACE_WITH_GROUND_LR24
cd /absolute/path/to/NYCU_UAV_offboard

python3 tools/send_lr24_command.py \
  --port "$GROUND_LR24_SERIAL" PING

python3 tools/send_lr24_command.py \
  --port "$GROUND_LR24_SERIAL" STATUS
```

如果出現 `Permission denied`：

```bash
sudo usermod -aG dialout "$USER"
```

登出再登入後生效。不要長期用 `sudo python3` 規避 serial 權限。

## 7. 真正的飛行控制順序

以下流程必須由飛手和地面站操作員共同執行。

### 步驟 1：拆槳桌面測試

依序確認：

1. Agent 正在執行。
2. `serial_gps_goto.launch.py` 正在執行。
3. `PING` 收到 `PONG`。
4. `STATUS` 有回覆。
5. 未 arm、未起飛時，GOTO 必須被拒絕。

### 步驟 2：飛手保持有效 RC / ELRS 鏈路並正常起飛

GPS GOTO node 不會幫你：

- arm；
- 起飛；
- 控制手拋／跑道起飛流程；
- 自動降落。

必須由飛手在有效且獨立的 RC / ELRS manual-control link 下，按照固定翼正常流程完成 arm、起飛並爬升到安全高度。QGroundControl 可以透過前述另一條 MAVLink 路徑協助設定與監看，但不能取代此系統要求的 RC/manual-control link；若 PX4 回報 `manual_control_signal_lost`，GOTO 會被拒絕。

### 步驟 3：再次查詢 STATUS

Windows：

```powershell
py .\tools\send_lr24_command.py --port COM7 STATUS
```

Ubuntu：

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" STATUS
```

只有回覆同時包含以下條件才可進入下一步：

```text
ready_for_goto=true
armed=true
fixed_wing=true
landed=false
```

若 `ready_for_goto=false`，讀取 `readiness_reason`，修正原因後重新查詢；不要連續重送 GOTO。

### 步驟 4：準備目標座標

座標格式必須是 WGS84 十進位角度：

```text
緯度 latitude：-90 到 90
經度 longitude：-180 到 180
```

注意：

- 順序永遠是「緯度、經度、高度」。
- 不可輸入度分秒，例如 `24°47'10"N`。
- 不可加 `N`、`E` 或 `m`。
- 南緯和西經使用負號。
- 先在 QGroundControl 地圖確認目標與整個固定翼盤旋圓都位於合法空域及 geofence 內。
- 不要照抄教學中的示意座標；必須使用你已完成風險評估的實際測試點。

### 步驟 5：送出相對 Home 高度的 GOTO

指令格式：

```text
GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
```

Windows PowerShell：

```powershell
py .\tools\send_lr24_command.py --port COM7 `
  GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
```

Ubuntu：

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" \
  GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
```

執行前必須把三個大寫占位字串換成數字。例如第三個數字 `80` 代表「Home AMSL 高度加 80 公尺」，不是 AMSL 80 公尺。

預設安全限制為：

- 相對 Home 目標高度：30～120 m。
- 目標距目前位置不超過 2 km。
- 目標距 Home 不超過 2 km。

這些只是應用程式預設限制，不能取代 geofence、地形高度、轉彎半徑、風場評估與法規。

### 步驟 6：判讀 GOTO 回覆

成功時會看到類似：

```text
< $ACK,<sequence>,GOTO,GOTO accepted ...
```

這個 `ACK` 表示：

1. PX4 已接受 `DO_REPOSITION`。
2. PX4 已進入 `AUTO_LOITER`。
3. PX4 發布的目標 setpoint 與要求的 GPS 座標、高度相符。

它不代表飛機已抵達目標。固定翼會飛向該點，接近後繞點盤旋。

若看到 `ERR`，不要立刻重送。先讀完錯誤文字並修正原因。

### 步驟 7：用 STATUS 監看

每隔一段合理時間手動查詢：

```powershell
py .\tools\send_lr24_command.py --port COM7 STATUS
```

主要狀態：

| State | 意義 |
|---|---|
| `IDLE` | 尚未接受目標 |
| `COMMAND_PENDING` | 等待 PX4 ACK／模式／setpoint 確認 |
| `ENROUTE` | PX4 已接受，正在前往目標 |
| `ARRIVED` | 已進入設定的抵達範圍；固定翼仍在盤旋 |
| `ABORTED` | node 停止追蹤；必須查看 `detail` 和實際 PX4 mode |

LR24 無線速率低，不要以很高頻率輪詢 STATUS，也不要透過它傳影像或大量 ROS topics。

### 步驟 8：返航或飛手接管

Windows：

```powershell
py .\tools\send_lr24_command.py --port COM7 RTL
```

Ubuntu：

```bash
python3 tools/send_lr24_command.py --port "$GROUND_LR24_SERIAL" RTL
```

`RTL` 只是要求 PX4 進入 `AUTO_RTL`。返航高度、盤旋和最後是否降落由 PX4 Return 參數決定。

`ABORT` 目前執行相同的 RTL service：

```powershell
py .\tools\send_lr24_command.py --port COM7 ABORT
```

它不是馬達 Kill，也不是在原地 Hold。固定翼的主要獨立緊急處置仍是飛手的 RC / ELRS 模式切換。

## 8. AMSL 高度指令

只有在明確知道目標 AMSL 海拔時才使用：

```powershell
py .\tools\send_lr24_command.py --port COM7 `
  GOTO_AMSL LAT_DEG LON_DEG ALT_AMSL_M
```

`ALT_AMSL_M` 是平均海平面高度，不是相對 Home、AGL 或 GPS 橢球高。

node 仍會將它換算成相對 Home 高度並套用 30～120 m 的預設限制，所以不能用 `GOTO_AMSL` 繞過高度安全 gate。

不確定高度基準時，優先使用相對 Home 的 `GOTO`。

## 9. Timeout 與重送規則

每次地面工具會：

1. 開啟 LR24 COM / serial port。
2. 產生一個 sequence。
3. 送出 checksummed frame。
4. 等待最多 8 秒。
5. 收到 `ACK`／`ERR` 或 timeout 後關閉 port。

如果 GOTO timeout，**不要直接再執行一筆新的 GOTO**。ACK 可能只是在回程途中遺失，而 PX4 已經開始飛往目標。

先依序：

1. 由飛手透過 RC 狀態及另一條 MAVLink 路徑上的 QGroundControl 確認實際 mode 和目標；若沒有獨立 QGC 鏈路，直接以 RC 接管並停止遠端命令測試。
2. 嘗試 `STATUS`。
3. 準備用 RC 接管。
4. 確認 PX4 沒有執行舊目標後，才決定是否送新命令。

送出畫面中的 frame 會顯示 sequence，例如：

```text
> $CMD,1721370000000000000,GOTO,...
```

若確定只是在重取**完全相同指令**的既有結果，可重用原 sequence，且 command 和三個參數必須一字不差：

```powershell
py .\tools\send_lr24_command.py --port COM7 `
  --seq 1721370000000000000 GOTO LAT_DEG LON_DEG REL_HOME_ALT_M
```

空中端只保留最近 64 個 sequence 的結果。相同 sequence 搭配不同 payload 會被拒絕。一般操作不需要手動指定 `--seq`。

不要對飛行指令使用 `--simple`；工具會拒絕未加 checksum 的 GOTO、RTL 等飛航命令。

## 10. 問題排查

| 現象 | 最可能原因 | 處理方式 |
|---|---|---|
| Windows 找不到 COM port | USB 線、驅動、LR24 供電或裝置辨識失敗 | 換資料線／USB port，查看裝置管理員與 `serial.tools.list_ports` |
| `Access is denied`／無法開啟 COM | QGC、LR24 設定工具或 serial terminal 正在占用 | 關閉其他程式後再執行；同一時間只開一個程式 |
| Ubuntu `Permission denied` | 使用者不在 `dialout` | 加入 `dialout`，登出再登入 |
| 有 `>` 但沒有 `<` | 錯誤 port／baud、兩顆 LR24 未配對、非透明模式、空中 node 未啟動 | 先確認兩顆 LR24 與兩端程式使用相同 baud（57600 或 115200）、相同頻道與 Network ID，再檢查空中 launch |
| PING timeout | 問題位於地面 serial、LR24 無線鏈路或空中 `lr24_command_node` | 不要檢查 GPS；先把 PING 修通 |
| PING 成功，但 STATUS 回 `ROS service not available` | `global_goto_node` 未啟動或已崩潰 | 檢查 `serial_gps_goto.launch.py` 的 Terminal |
| STATUS 有 ACK，但 `ready_for_goto=false` | PX4/GPS/Home/RC/arming/airborne/failsafe 條件未滿足 | 讀取 `readiness_reason`，處理後重查 STATUS |
| GOTO 回 ERR | 座標、高度、距離、安全狀態、PX4 ACK 或 setpoint 確認失敗 | 完整閱讀 ERR，不要連續重送 |
| 空中端顯示 `Failed to open serial port` | `lr24_port` 錯誤、權限不足或 port 被占用 | 修正路徑／權限後重新啟動 launch |
| RTL 沒有回覆 | LR24、Orin、Agent 或 Pixhawk 鏈路可能失效 | 立即使用獨立 RC/ELRS；失聯後不能依賴軟體 RTL |

快速判斷流程：

```text
PING 失敗
  └─ 修地面 COM、兩端一致的 baud、兩顆 LR24、空中 serial node

PING 成功，但 STATUS service unavailable
  └─ 修空中 ROS launch / global_goto_node

STATUS 成功，但 ready_for_goto=false
  └─ 依 readiness_reason 修 PX4 / GPS / Home / RC / 飛行狀態

ready_for_goto=true
  └─ 經飛手確認後，才可送一筆 GOTO
```

## 11. LR24 失聯時飛機會怎樣

GOTO 是一次性 PX4 reposition command，不是需要地面持續傳送的 Offboard stream。

因此 PX4 接受 GOTO 後：

- 地面程式關閉不會自動取消目標。
- LR24 斷線不會自動取消目標。
- Orin 的 LR24 node 停止也不一定取消目標。
- 飛機通常會繼續前往已接受的 GPS 目標並盤旋。
- 如果 Orin、Agent 或 LR24 已失效，地面端再輸入 `RTL` 也送不到 Pixhawk。

所以實飛必須具備：

1. 獨立 RC / ELRS 控制鏈。
2. 已驗證的模式切換與接管程序。
3. PX4 geofence。
4. RC-loss、position-loss、battery 等 PX4 failsafe。
5. 合法空域、目視飛行與飛手即時判斷。

## 12. 第一次操作的最短檢查表

### 桌面／拆槳

- [ ] 兩顆 LR24 天線已接好。
- [ ] 兩端為透明模式、使用相同 baud、相同頻道與 Network ID。
- [ ] Orin 的 Pixhawk serial 與 LR24 serial 是不同裝置。
- [ ] Agent 正在執行。
- [ ] GPS GOTO launch 正在執行。
- [ ] 地面 `PING` 收到 `PONG`。
- [ ] 地面 `STATUS` 有回覆。
- [ ] 未起飛時 GOTO 會被拒絕。

### 實飛前

- [ ] 固定翼 SITL 已通過。
- [ ] 拆槳失聯測試已通過。
- [ ] geofence、RTL 與 failsafe 已驗證。
- [ ] 飛手可隨時用 RC / ELRS 接管。
- [ ] 目標座標、高度基準、航路與盤旋圓都已確認。

### 飛行中

- [ ] 飛手完成 arm、起飛與安全爬升。
- [ ] `STATUS` 明確顯示 `ready_for_goto=true`。
- [ ] 一次只送一個經確認的 GOTO。
- [ ] ACK 後仍持續監看 QGC、STATUS 和飛機目視狀態。
- [ ] 異常時優先 RC 接管，不依賴 LR24 一定可用。
