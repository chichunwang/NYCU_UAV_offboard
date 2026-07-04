# LR24-F Link Test 使用教學

本文件說明如何使用 `lr24_link_test.py`，在兩台 Ubuntu 電腦之間測試兩顆 LR24-F 的透明 Serial 無線鏈路。

此程式可以測量：

- 雙向通訊是否正常
- 封包遺失率
- 往返延遲 RTT
- 最小、平均、中位數、P95 與最大延遲
- 不同傳送頻率與封包大小下的鏈路表現

---

## 1. 測試架構

```text
Ubuntu A：地面端
    │
    │ USB Serial
    ▼
地面端 LR24-F
    │
    │ 無線鏈路
    ▼
空中端 LR24-F
    │
    │ USB Serial
    ▼
Ubuntu B：空中端／模擬 Jetson Orin
```

測試時：

- Ubuntu A 執行 `initiator`
- Ubuntu B 執行 `responder`

`initiator` 會傳送 `PING` 封包。

`responder` 收到後會立刻回傳對應的 `PONG`。

Ubuntu A 根據傳送與收到回覆的時間差，計算 RTT 與封包遺失率。

---

## 2. 前置需求

兩台 Ubuntu 電腦都需要：

- 一顆 LR24-F
- USB 連接線
- Python 3
- `pyserial`
- 兩顆 LR24-F 已設定為相同的無線參數

兩端 LR24-F 至少需要確認以下設定一致：

| 項目 | 建議設定 |
|---|---:|
| 工作模式 | Transparent Serial |
| UART Baud Rate | 115200 bps |
| 無線傳輸速率 | 8 kbps |
| 頻道／頻率 | 兩端相同 |
| Network ID | 兩端相同 |
| 發射功率 | 桌面測試先使用低功率 |

> LR24-F 的 UART Baud Rate 與無線傳輸速率是不同概念。  
> 電腦與 LR24-F 之間可以使用 115200 bps，但實際無線鏈路只有約 8 kbps。

測試前請先接好天線，再讓 LR24-F 通電。

---

## 3. 安裝套件

兩台 Ubuntu 都執行：

```bash
sudo apt update
sudo apt install -y python3 python3-serial
```

確認 Python 版本：

```bash
python3 --version
```

確認 `pyserial` 可以正常載入：

```bash
python3 -c "import serial; print(serial.__version__)"
```

如果出現：

```text
ModuleNotFoundError: No module named 'serial'
```

請重新安裝：

```bash
sudo apt install --reinstall python3-serial
```

---

## 4. Serial 權限設定

Ubuntu 一般使用者可能沒有操作 `/dev/ttyUSB0` 的權限。

把目前使用者加入 `dialout` 群組：

```bash
sudo usermod -aG dialout $USER
```

接著登出 Ubuntu 再重新登入。

也可以在目前終端機暫時套用：

```bash
newgrp dialout
```

確認群組：

```bash
groups
```

輸出中應該包含：

```text
dialout
```

---

## 5. 找出 LR24-F 的 Serial Port

插入 LR24-F 後執行：

```bash
ls /dev/ttyUSB*
```

常見結果：

```text
/dev/ttyUSB0
```

也可以使用：

```bash
dmesg | tail -n 20
```

或即時監看 USB 裝置：

```bash
sudo dmesg -w
```

插入 LR24-F 後，可能看到：

```text
usb 1-2: FTDI USB Serial Device converter now attached to ttyUSB0
```

### 建議使用固定裝置路徑

如果系統有多個 USB Serial 裝置，可以查看：

```bash
ls -l /dev/serial/by-id/
```

例如：

```text
/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A123456-if00-port0
```

執行程式時可以直接使用這個路徑，避免重新插拔後 `/dev/ttyUSB0` 變成 `/dev/ttyUSB1`。

---

## 6. 取得測試程式

確認檔案位於目前資料夾：

```bash
ls
```

應該看到：

```text
lr24_link_test.py
```

設定執行權限：

```bash
chmod +x lr24_link_test.py
```

查看程式說明：

```bash
python3 lr24_link_test.py --help
```

---

## 7. 啟動 Responder

在 Ubuntu B，也就是空中端／模擬 Jetson Orin 的電腦執行：

```bash
python3 lr24_link_test.py responder \
  --port /dev/ttyUSB0
```

如果使用固定裝置路徑：

```bash
python3 lr24_link_test.py responder \
  --port /dev/serial/by-id/你的裝置名稱
```

正常輸出：

```text
Responder listening on /dev/ttyUSB0 at 115200 bps. Press Ctrl+C to stop.
```

當收到封包時，會顯示：

```text
RX seq=1 bytes=59 -> ACK
RX seq=2 bytes=59 -> ACK
RX seq=3 bytes=59 -> ACK
```

Responder 會持續執行，直到按下：

```text
Ctrl+C
```

停止後會顯示成功收到的 PING 封包數量。

---

## 8. 啟動 Initiator

在 Ubuntu A，也就是地面端電腦執行：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0
```

預設參數為：

- 傳送 200 個封包
- 每 0.5 秒傳送一次
- 每個封包附帶 32 bytes payload
- 每個封包等待回覆最多 2 秒
- Serial Baud Rate 為 115200 bps

也可以明確指定：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --baud 115200 \
  --count 200 \
  --interval 0.5 \
  --payload 32 \
  --timeout 2.0
```

正常輸出：

```text
Sending 200 packets through /dev/ttyUSB0, interval=0.5s, payload=32 bytes.
seq=0001  RTT=   85.31 ms
seq=0002  RTT=   81.92 ms
seq=0003  RTT=   83.10 ms
```

如果在 timeout 時間內沒有收到回覆：

```text
seq=0004  TIMEOUT
```

---

## 9. 測試結果說明

測試結束後，程式會輸出摘要：

```text
=== LR24 Link Test Summary ===
Sent:       200
Received:   198
Lost:       2 (1.00%)
RTT min:    76.32 ms
RTT average:88.41 ms
RTT median: 84.75 ms
RTT p95:    121.08 ms
RTT max:    247.94 ms
```

各項數值意義如下。

### Sent

送出的 PING 封包總數。

### Received

成功收到對應 PONG 回覆的封包數量。

### Lost

超過 timeout 仍未收到回覆的封包數量與比例。

計算方式：

```text
封包遺失率 = Lost ÷ Sent × 100%
```

### RTT

RTT 是 Round-Trip Time，代表封包從 Ubuntu A 傳到 Ubuntu B，再由 Ubuntu B 回傳到 Ubuntu A 所需的總時間。

```text
Ubuntu A
   ↓ PING
Ubuntu B
   ↓ PONG
Ubuntu A
```

程式會統計：

| 指標 | 意義 |
|---|---|
| RTT min | 最小往返延遲 |
| RTT average | 平均往返延遲 |
| RTT median | RTT 中位數 |
| RTT p95 | 95% 封包的 RTT 不超過此值 |
| RTT max | 最大往返延遲 |

其中 P95 比單純平均值更適合觀察偶發延遲。

例如：

```text
RTT average = 90 ms
RTT p95 = 180 ms
RTT max = 900 ms
```

代表大部分封包很快，但偶爾可能出現接近 1 秒的延遲。

---

## 10. 參數說明

### Responder 參數

```bash
python3 lr24_link_test.py responder --help
```

| 參數 | 預設值 | 說明 |
|---|---:|---|
| `--port` | 無 | Serial 裝置路徑，必填 |
| `--baud` | 115200 | UART Baud Rate |
| `--timeout` | 0.2 秒 | Serial 讀取 timeout |

範例：

```bash
python3 lr24_link_test.py responder \
  --port /dev/ttyUSB0 \
  --baud 115200 \
  --timeout 0.2
```

### Initiator 參數

```bash
python3 lr24_link_test.py initiator --help
```

| 參數 | 預設值 | 說明 |
|---|---:|---|
| `--port` | 無 | Serial 裝置路徑，必填 |
| `--baud` | 115200 | UART Baud Rate |
| `--timeout` | 2.0 秒 | 等待每個 PONG 的最長時間 |
| `--count` | 200 | 傳送封包數量 |
| `--interval` | 0.5 秒 | 每次傳送的時間間隔 |
| `--payload` | 32 bytes | 每個 PING 附加的測試資料量 |

---

## 11. 建議測試流程

### 測試一：基本連線

先使用低負載：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 20 \
  --interval 1.0 \
  --payload 8
```

用途：

- 確認雙向通訊正常
- 確認 Baud Rate 正確
- 確認兩顆 LR24-F 已正確配對

### 測試二：一般指令鏈路

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 200 \
  --interval 0.5 \
  --payload 32
```

這個測試接近低頻控制指令與狀態查詢的使用情境。

### 測試三：提高傳送頻率

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 1000 \
  --interval 0.2 \
  --payload 50
```

相當於每秒最多送出 5 次 PING。

用途：

- 測試較高頻率下是否開始掉包
- 觀察 RTT 是否逐漸增加
- 檢查 LR24-F 緩衝區是否堆積

### 測試四：較大封包壓力測試

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 500 \
  --interval 0.1 \
  --payload 100
```

這屬於壓力測試，不代表正式系統應以這個速率運作。

LR24-F 的無線速率只有約 8 kbps，資料量過高時可能造成：

- 延遲持續上升
- Serial 緩衝區堆積
- PING 與 PONG 排隊
- timeout
- 封包遺失率增加

---

## 12. 建議初步合格標準

以下是專案初期測試可使用的工程判斷值，不是 LR24-F 官方保證值。

| 項目 | 建議結果 |
|---|---:|
| 低負載封包遺失率 | 0～1% |
| 一般測試封包遺失率 | 小於 2% |
| RTT 平均值 | 小於約 300 ms |
| RTT P95 | 小於約 500～1000 ms |
| 長時間測試 | RTT 不應持續上升 |
| 重連能力 | 模組重新連線後可恢復通訊 |

對以下低頻指令而言，數百毫秒延遲通常仍可接受：

- `PING`
- `STATUS`
- `ENABLE_STREAM`
- `START_MISSION`
- `LAND`

但此鏈路不適合傳送：

- 高頻 Offboard setpoint
- 即時搖桿控制
- 高頻 ROS 2 topic
- 影像
- 大量 log

---

## 13. 雙向測試

完成 Ubuntu A 傳送、Ubuntu B 回覆後，建議交換角色。

Ubuntu A：

```bash
python3 lr24_link_test.py responder \
  --port /dev/ttyUSB0
```

Ubuntu B：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 200 \
  --interval 0.5 \
  --payload 32
```

交換方向可以檢查：

- 兩端 USB Serial 是否都正常
- LR24-F 上下行是否有明顯差異
- 某一端是否有額外干擾或供電問題

---

## 14. 長時間測試

若要測試 30 分鐘，可以設定：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 3600 \
  --interval 0.5 \
  --payload 32
```

計算方式：

```text
3600 × 0.5 秒 = 1800 秒 = 30 分鐘
```

若要測試約 1 小時：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 7200 \
  --interval 0.5 \
  --payload 32
```

長時間測試主要觀察：

- 是否突然大量 timeout
- RTT 是否逐漸增加
- 模組是否過熱
- USB Serial 是否中斷
- 斷線後能否自行恢復

---

## 15. 距離測試建議

建議按順序進行：

1. 兩顆 LR24-F 相距 1～2 公尺
2. 室內不同房間
3. 室外 10 公尺
4. 室外 50 公尺
5. 室外 100 公尺以上
6. 實際機上安裝位置

每個距離都記錄：

| 項目 | 紀錄內容 |
|---|---|
| 距離 | 例如 50 公尺 |
| 發射功率 | 例如 100 mW |
| 無線速率 | 8 kbps |
| 封包數量 | 例如 1000 |
| Payload | 例如 32 bytes |
| Loss | 例如 0.4% |
| RTT average | 例如 95 ms |
| RTT p95 | 例如 180 ms |
| RTT max | 例如 420 ms |
| 測試環境 | 室內／室外／是否遮蔽 |

> 距離測試時，不要讓兩顆高功率發射模組靠得太近。  
> 桌面近距離測試應先降低發射功率。

---

## 16. 中斷與重連測試

建議在 Initiator 持續執行時：

1. 拔除 Ubuntu B 的 LR24-F
2. 等待 10 秒
3. 重新插入 LR24-F
4. 觀察通訊是否恢復

需要注意：

目前程式啟動時會開啟一次 Serial Port。

如果 USB 裝置被拔除後重新插入，Linux 可能重新建立裝置節點，程式通常需要重新啟動。

如果只是關閉 LR24-F 的無線連線，但 USB Serial 裝置仍存在，程式則有機會在鏈路恢復後繼續通訊。

---

## 17. 常見問題

### 問題一：Permission denied

錯誤：

```text
Unable to open /dev/ttyUSB0: [Errno 13] Permission denied
```

解法：

```bash
sudo usermod -aG dialout $USER
```

登出再登入，或執行：

```bash
newgrp dialout
```

不要長期使用：

```bash
sudo python3 lr24_link_test.py ...
```

使用 sudo 可能造成 Python 環境與檔案權限問題。

---

### 問題二：找不到 `/dev/ttyUSB0`

檢查：

```bash
lsusb
ls /dev/ttyUSB*
dmesg | tail -n 30
```

可能原因：

- USB 線只有充電功能
- LR24-F 沒有正常供電
- USB-UART 驅動未載入
- 裝置實際是 `/dev/ttyACM0`
- 裝置編號變成 `/dev/ttyUSB1`

---

### 問題三：Device or resource busy

錯誤：

```text
Device or resource busy
```

找出占用程式：

```bash
sudo lsof /dev/ttyUSB0
```

或：

```bash
sudo fuser -v /dev/ttyUSB0
```

常見占用來源：

- minicom
- screen
- ModemManager
- 另一個 Python 程式
- ROS 2 serial node

停止 ModemManager：

```bash
sudo systemctl stop ModemManager
```

如不需要，也可以停用開機啟動：

```bash
sudo systemctl disable ModemManager
```

---

### 問題四：全部顯示 TIMEOUT

可能原因：

- Responder 沒有啟動
- 兩端 LR24-F 頻率或 Network ID 不同
- Baud Rate 不一致
- Serial Port 選錯
- LR24-F 不是透明傳輸模式
- 天線未接好
- 模組供電不穩
- 一端 RX/TX 或 USB Serial 異常

先用低負載測試：

```bash
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --count 10 \
  --interval 1 \
  --payload 0 \
  --timeout 3
```

---

### 問題五：Responder 顯示 `RX unrecognized`

例如：

```text
RX unrecognized: 'hello'
```

代表 Responder 收到了資料，但資料不是程式預期的：

```text
PING,sequence,timestamp,payload
```

可能是：

- 另一個程式正在往同一個 Serial Port 傳資料
- LR24-F 緩衝區內還有舊資料
- Baud Rate 錯誤導致亂碼
- Serial 鏈路混入其他協定資料

重新啟動兩端程式時，程式會清除 Serial input/output buffer。

---

### 問題六：封包沒有遺失，但 RTT 越來越大

這通常表示傳送資料量超過無線鏈路可處理的速度，資料正在 LR24-F 或 Serial buffer 中排隊。

改善方式：

- 增加 `--interval`
- 降低 `--payload`
- 降低狀態回報頻率
- 不要傳送大量 log
- 確認實際無線速率設定
- 確認 UART flow control 設定

例如把：

```bash
--interval 0.1 --payload 100
```

改成：

```bash
--interval 0.5 --payload 32
```

---

### 問題七：少量封包偶爾出現很大的 RTT

可能原因：

- 無線重傳
- 系統排程延遲
- USB Serial buffer
- 電磁干擾
- LR24-F 內部封包聚合或排隊
- 傳送頻率接近頻寬上限

判斷時應一起觀察：

- RTT average
- RTT median
- RTT p95
- RTT max
- Lost percentage

不要只看最大值。

---

## 18. 封包格式

Initiator 傳送：

```text
PING,<sequence>,<timestamp_ns>,<payload>
```

例如：

```text
PING,15,1234567890123456,XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
```

Responder 回覆：

```text
PONG,<sequence>,<timestamp_ns>
```

例如：

```text
PONG,15,1234567890123456
```

Responder 會把 sequence 與原始 timestamp 原樣回傳。

Initiator 只有在以下資料都符合時，才認定為有效回覆：

- 第一欄是 `PONG`
- sequence 與目前封包相同
- timestamp 與目前封包相同

這可以避免舊封包或其他封包被誤判為目前的 ACK。

---

## 19. 目前程式限制

此程式主要用於透明 Serial 鏈路測試，並不是正式控制協定。

目前限制包括：

- 沒有 checksum
- 沒有 CRC
- 沒有封包重傳
- 沒有指令去重
- 沒有自動重新開啟被拔除的 Serial Port
- 一次只等待一個 PONG
- 不測試單向吞吐量
- RTT 包含程式、USB Serial、LR24-F 和無線鏈路的全部延遲

正式 LR24 指令協定仍應使用：

- sequence number
- checksum 或 CRC
- ACK／ERR
- timeout
- 重複指令保護
- 指令白名單
- 安全狀態判斷

---

## 20. 建議測試紀錄格式

可將每次測試結果記錄成：

```text
日期：
測試地點：
地面端電腦：
空中端電腦：
LR24-F 韌體版本：
UART Baud Rate：
無線速率：
發射功率：
距離：
是否有遮蔽物：

測試參數：
count =
interval =
payload =
timeout =

測試結果：
Sent =
Received =
Lost =
RTT min =
RTT average =
RTT median =
RTT p95 =
RTT max =

備註：
```

---

## 21. 完整範例

### Ubuntu B

```bash
sudo apt install -y python3-serial
sudo usermod -aG dialout $USER
newgrp dialout

cd NYCU_UAV_offboard
python3 lr24_link_test.py responder \
  --port /dev/ttyUSB0 \
  --baud 115200
```

### Ubuntu A

```bash
sudo apt install -y python3-serial
sudo usermod -aG dialout $USER
newgrp dialout

cd NYCU_UAV_offboard
python3 lr24_link_test.py initiator \
  --port /dev/ttyUSB0 \
  --baud 115200 \
  --count 200 \
  --interval 0.5 \
  --payload 32 \
  --timeout 2.0
```

完成後，將 Ubuntu A 顯示的 Summary 保存下來，作為 LR24-F 鏈路可行性評估依據。
