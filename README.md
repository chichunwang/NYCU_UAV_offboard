# NYCU UAV Offboard Control

本專案用於開發固定翼無人機的板外控制（Offboard Control）系統。

主要目標是讓板外電腦取得飛行器與外部目標的 GPS 座標，透過飛控控制無人機執行繞點飛行與目標追蹤。

---

## 專案目標

專案分為兩個主要階段：

1. GPS 座標取得與繞點飛行
2. 根據外部雲台相機提供的目標座標進行追蹤

另外需要建立板外電腦、飛控與地面站之間的通訊方式。

---

# 系統架構

預計系統架構如下：

```text
雲台相機系統
     │
     │ 目標 GPS 座標
     ▼
板外電腦（Ubuntu）
     │
     │ MAVLink / Offboard Command
     ▼
Pixhawk 飛控
     │
     ▼
固定翼無人機
```

地面端架構：

```text
地面站電腦
     │
     │ LR24-F / Serial / Network
     ▼
空中端板外電腦
     │
     ▼
Pixhawk 飛控
```

---

# 開發里程碑

## Milestone 1：取得 GPS 座標並完成繞點飛行

第一階段目標是讓板外電腦取得飛行器目前的 GPS 資訊，並控制無人機依序飛向指定座標。

### 開發項目

- [ ] 板外電腦成功連接 Pixhawk
- [ ] 接收飛行器 GPS 座標
- [ ] 顯示經度、緯度、高度與時間戳記
- [ ] 檢查 GPS Fix 狀態與衛星數量
- [ ] 建立目標航點格式
- [ ] 發送單一 GPS 航點
- [ ] 判斷飛行器是否抵達航點
- [ ] 支援多個航點依序飛行
- [ ] 完成繞點或封閉路徑飛行
- [ ] 加入返航與失效保護

### 建議控制流程

```text
INIT
  ↓
等待飛控連線
  ↓
等待 GPS 有效
  ↓
解鎖與起飛
  ↓
飛向航點 1
  ↓
抵達判斷
  ↓
飛向下一個航點
  ↓
完成所有航點
  ↓
返航或盤旋
```

### 航點資料格式

建議統一使用以下格式：

```cpp
struct Waypoint {
    double latitude_deg;
    double longitude_deg;
    float altitude_m;
};
```

### 第一階段驗收條件

- 可以穩定取得飛行器 GPS 座標
- 可以載入至少三個航點
- 飛行器能依序飛向各航點
- 能判斷是否抵達航點
- GPS 或通訊中斷時不會持續送出危險指令
- 能執行返航、盤旋或其他安全模式

---

## Milestone 2：根據雲台相機座標追蹤目標

第二階段由另一組的雲台相機系統提供目標座標，板外電腦接收座標後，控制飛行器朝向目標位置飛行。

### 外部輸入資料

雲台相機組至少需要提供：

```text
target_id
latitude
longitude
altitude
timestamp
valid
```

建議資料格式：

```cpp
struct TargetPosition {
    int target_id;
    double latitude_deg;
    double longitude_deg;
    float altitude_m;
    uint64_t timestamp_ms;
    bool valid;
};
```

### 開發項目

- [ ] 定義雲台相機組與飛控組之間的資料格式
- [ ] 接收目標 GPS 座標
- [ ] 檢查座標是否合法
- [ ] 檢查資料是否逾時
- [ ] 對目標座標進行濾波
- [ ] 將目標座標轉換為飛行控制目標
- [ ] 控制飛行器朝向目標飛行
- [ ] 限制最大速度、轉彎率與高度範圍
- [ ] 目標移動時持續更新追蹤點
- [ ] 目標消失時進入安全模式

### 建議追蹤狀態機

```text
WAIT_TARGET
     ↓
收到有效目標
     ↓
TRACK_TARGET
     ↓
持續更新目標位置
     ↓
資料逾時或目標遺失
     ↓
HOLD / LOITER
     ↓
重新取得目標或返航
```

### 第二階段驗收條件

- 可以穩定接收另一組傳送的目標座標
- 能辨識過期或無效的目標資料
- 目標移動時可以持續更新飛行方向
- 目標資料中斷時會停止追蹤
- 不會因單次錯誤座標產生劇烈控制指令
- 追蹤功能可隨時由地面站中止

---

# 地面站通訊

目前的小項目是建立空中端與地面站之間的通訊。

可能使用的通訊方式：

- USB Serial
- LR24-F 透明串列通訊
- MAVLink
- UDP 或 TCP
- PPP 網路連線
- SSH 遠端操作

初期建議先使用純 Serial 驗證資料傳輸，再逐步加入封包格式、錯誤檢查與網路協定。

## 通訊開發順序

```text
純 Serial 雙向傳輸
        ↓
定義封包格式
        ↓
加入封包起始碼與長度
        ↓
加入 CRC 錯誤檢查
        ↓
加入序號與時間戳記
        ↓
傳送 GPS、狀態與控制命令
        ↓
視需求導入 MAVLink 或網路連線
```

## 建議訊息類型

```text
HEARTBEAT
GPS_POSITION
TARGET_POSITION
AIRCRAFT_STATUS
MISSION_COMMAND
MISSION_STATUS
ABORT_COMMAND
```

通訊封包至少應包含：

```text
message_type
sequence_number
payload_length
timestamp
payload
checksum
```

---

# 建議程式架構

```text
NYCU_UAV_offboard/
├── README.md
├── .gitignore
├── CMakeLists.txt
├── package.xml
├── include/
│   └── my_offboard_cpp/
│       ├── communication.hpp
│       ├── navigation.hpp
│       ├── waypoint_manager.hpp
│       ├── target_tracker.hpp
│       └── safety_manager.hpp
├── src/
│   ├── main.cpp
│   ├── communication.cpp
│   ├── navigation.cpp
│   ├── waypoint_manager.cpp
│   ├── target_tracker.cpp
│   └── safety_manager.cpp
├── config/
│   ├── vehicle.yaml
│   └── communication.yaml
├── launch/
├── test/
└── docs/
```

各模組負責內容：

```text
communication
負責 Pixhawk、地面站與雲台相機資料收發

navigation
負責將目標位置轉換成飛行控制命令

waypoint_manager
負責航點清單、航點切換與抵達判斷

target_tracker
負責接收、檢查與濾波目標座標

safety_manager
負責通訊中斷、GPS 無效、目標遺失與返航邏輯
```

---

# 開發流程

每個功能都應建立獨立 Branch，不要直接在 `master` 上開發。

## 建立功能分支

```bash
git switch master
git pull origin master
git switch -c feature/功能名稱
```

例如：

```bash
git switch -c feature/gps-reader
git switch -c feature/waypoint-navigation
git switch -c feature/lr24-communication
git switch -c feature/target-tracking
```

## 提交修改

```bash
git add .
git commit -m "Add GPS position reader"
git push -u origin feature/gps-reader
```

推送完成後，在 GitHub 建立 Pull Request，由其他成員檢查後再合併進 `master`。

---

# Branch 命名規則

```text
feature/gps-reader
feature/waypoint-navigation
feature/ground-communication
feature/target-tracking

fix/gps-timeout
fix/serial-packet-error

test/sitl-waypoint
docs/update-readme
```

---

# Commit 訊息規則

建議使用簡短且明確的英文描述：

```text
Add GPS position reader
Add waypoint arrival detection
Implement serial packet parser
Fix target coordinate timeout
Update ground communication document
```

避免使用：

```text
update
test
修改
123
```

---

# 測試規範

所有控制功能都必須依照以下順序測試：

1. 單元測試
2. 假資料測試
3. PX4 SITL 模擬測試
4. 地面靜態測試
5. 無螺旋槳測試
6. 低風險飛行測試
7. 完整任務測試

未通過模擬與地面測試前，不得直接進行實際飛行。

---

# 安全要求

所有飛行功能必須具備：

- GPS 無效檢查
- 通訊逾時檢查
- 目標資料逾時檢查
- 最大高度限制
- 最大速度限制
- 最大目標距離限制
- 地理圍欄
- 手動接管功能
- 立即中止功能
- 返航或盤旋失效保護

任何收到的外部座標都不能直接使用，必須先檢查範圍、時間戳記與合理性。

---

# 目前進度

## GPS 航點飛行

- [ ] 取得 Pixhawk GPS
- [ ] 顯示 GPS 資訊
- [ ] 傳送單一航點
- [ ] 多航點任務
- [ ] 繞點飛行
- [ ] SITL 測試
- [ ] 實機測試

## 目標追蹤

- [ ] 定義目標資料格式
- [ ] 接收相機組座標
- [ ] 座標有效性檢查
- [ ] 目標座標濾波
- [ ] 追蹤控制
- [ ] 目標遺失處理
- [ ] SITL 測試
- [ ] 實機測試

## 地面站通訊

- [ ] 純 Serial 傳輸
- [ ] LR24-F 雙向通訊
- [ ] 自訂封包格式
- [ ] CRC 錯誤檢查
- [ ] Heartbeat
- [ ] 飛行狀態回傳
- [ ] 地面控制命令
- [ ] SSH 或網路連線測試

---

# 開發環境

請在此補上團隊統一使用的版本：

```text
作業系統：Ubuntu 24.04 LTS（Noble Numbat）
ROS 2 版本：ROS 2 Jazzy Jalisco
PX4 版本：PX4 v1.17.0 Stable
MAVSDK 版本：MAVSDK v3.17.1（若使用 MAVSDK）
MAVROS 版本：MAVROS 2.14.0（若使用 MAVROS）
CMake 版本：TBD 執行 cmake --version 確認
編譯器版本：TBD 執行 g++ --version 確認
飛控型號：Holybro Pixhawk 6C Mini
板外電腦：NVIDIA Jetson Orin Nano
通訊模組：MicoAir LR24-F
```

---

# 團隊協作原則

- 不直接修改 `master`
- 一個 Branch 只處理一個功能
- 合併前必須完成基本測試
- 通訊格式修改需要同步通知所有組別
- 所有座標必須標明單位與座標系
- 所有訊息必須附帶時間戳記
- 參數不得直接寫死在程式中
- 測試資料與正式飛行資料必須分開
- 發生錯誤時，系統應優先進入安全狀態