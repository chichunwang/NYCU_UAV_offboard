# 電子圍籬返航（Geofence RTL）

## 功能說明
為 offboard 飛行新增「電子圍籬返航」安全機制：
設定一個安全飛行範圍，當無人機飛出此範圍時，自動觸發返航（Return to Launch）。

## 運作流程
1. 記錄起飛點（home position）作為返航目標
2. 持續訂閱無人機的全球位置（vehicle_global_position）
3. 即時判斷無人機是否超出設定的安全範圍
4. 一旦超出邊界，自動飛回起飛點降落

## 使用的主要 ROS 2 Topic
- `/fmu/out/vehicle_global_position`：讀取無人機目前經緯度
- `/fmu/out/home_position`：取得起飛點座標
- `/fmu/in/trajectory_setpoint`：送出返航目標位置
- `/fmu/in/offboard_control_mode`：維持 offboard 模式

## 開發狀態
🚧 開發中

## 未來規劃
- 空中避障：透過圖傳影像辨識障礙物（如氣球），偵測到後自動閃避
