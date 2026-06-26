# LR24-F PPP / SSH Offboard 操作架構

本文件描述目前建議的地面站到 Jetson Orin 控制方式：

```text
地面筆電 Ubuntu 24.04
    │
    │ USB Serial: /dev/ttyUSBx, 115200 bps
    ▼
地面端 LR24-F
    │
    │ 無線鏈路: 8 kbps
    ▼
空中端 LR24-F
    │
    │ USB Serial: /dev/ttyUSBx, 115200 bps
    ▼
Jetson Orin
    │
    │ PPP network: 10.24.0.1 <-> 10.24.0.2
    ▼
SSH / ROS 2 service
    │
    ▼
my_offboard_node
    │
    ▼
PX4 /fmu/in/*
```

LR24-F 只負責承載低頻寬 IP 連線。Offboard setpoint 必須由 Orin 本機持續送給 PX4，不建議從地面站透過 LR24-F 即時傳 setpoint。

## IP 規劃

| 端點 | PPP IP | 用途 |
|---|---:|---|
| 地面筆電 | `10.24.0.1` | SSH client / 指令端 |
| Jetson Orin | `10.24.0.2` | SSH server / ROS 2 offboard 控制端 |

## PPP 啟動

兩端如果尚未安裝 PPP 工具，先安裝：

```bash
sudo apt update
sudo apt install ppp
```

先確認兩邊 LR24-F 出現的 serial port：

```bash
ls /dev/ttyUSB*
```

也可以插拔 LR24-F 後看 kernel log：

```bash
dmesg -w
```

### Jetson Orin 端

先在 Orin 端啟動 PPP，讓它等待地面站撥入：

```bash
sudo pppd /dev/ttyUSB0 115200 10.24.0.2:10.24.0.1 \
  noauth local nocrtscts lock noipdefault passive persist nodetach \
  mtu 296 mru 296 lcp-echo-interval 10 lcp-echo-failure 3
```

### 地面筆電端

再在地面站啟動 PPP：

```bash
sudo pppd /dev/ttyUSB0 115200 10.24.0.1:10.24.0.2 \
  noauth local nocrtscts lock noipdefault persist nodetach \
  mtu 296 mru 296 lcp-echo-interval 10 lcp-echo-failure 3
```

PPP 成功後，兩邊會出現 `ppp0`：

```bash
ip addr show ppp0
ping -c 3 10.24.0.2
```

> LR24-F 空中速率只有 8 kbps，`ping` 延遲高或 SSH 反應慢是正常的。避免用這條鏈路傳影像、大量 ROS topic 或大量 log。

## SSH 連線

Orin 端需先開 SSH server：

```bash
sudo apt update
sudo apt install openssh-server
sudo systemctl enable --now ssh
```

地面站連線：

```bash
ssh orin@10.24.0.2
```

建議使用 SSH key，避免低頻寬鏈路上反覆輸入密碼：

```bash
ssh-copy-id orin@10.24.0.2
```

## Orin 端啟動 Offboard 節點

在 Orin 上先啟動 ROS 2 offboard 節點。此節點現在預設是 standby，不會在啟動後立刻 arm：

```bash
source /opt/ros/jazzy/setup.bash
source ~/NYCU_UAV_offboard/install/setup.bash
ros2 run my_offboard_cpp my_offboard_node
```

如果 Orin 使用 ROS 2 Humble，請把 `/opt/ros/jazzy/setup.bash` 改成 `/opt/ros/humble/setup.bash`。

也可以用 SSH 從地面站遠端啟動：

```bash
ssh orin@10.24.0.2 'bash -lc "source /opt/ros/jazzy/setup.bash && source ~/NYCU_UAV_offboard/install/setup.bash && ros2 run my_offboard_cpp my_offboard_node"'
```

## 從地面站呼叫 Orin 進入 Offboard

節點啟動後，從地面站用 SSH 呼叫 Orin 本機的 ROS 2 service：

```bash
ssh orin@10.24.0.2 'bash -lc "source /opt/ros/jazzy/setup.bash && source ~/NYCU_UAV_offboard/install/setup.bash && ros2 service call /start_offboard std_srvs/srv/Trigger {}"'
```

`my_offboard_node` 收到 `/start_offboard` 後會：

1. 開始發布 `/fmu/in/offboard_control_mode`
2. 開始發布 `/fmu/in/trajectory_setpoint`
3. 等待約 1 秒，讓 PX4 先收到穩定 setpoint
4. 發送切換 Offboard mode 指令
5. 發送 arm 指令

## 遠端停止或降落

停止發布 Offboard setpoint：

```bash
ssh orin@10.24.0.2 'bash -lc "source /opt/ros/jazzy/setup.bash && source ~/NYCU_UAV_offboard/install/setup.bash && ros2 service call /stop_offboard std_srvs/srv/Trigger {}"'
```

送 PX4 land 指令：

```bash
ssh orin@10.24.0.2 'bash -lc "source /opt/ros/jazzy/setup.bash && source ~/NYCU_UAV_offboard/install/setup.bash && ros2 service call /land std_srvs/srv/Trigger {}"'
```

實機測試時，仍需保留 ELRS 或其他獨立 RC 鏈路作為手動接管與緊急保護。

## 建議測試順序

1. 只接兩台電腦和兩顆 LR24-F，先確認 PPP 可以建立。
2. 用 `ping` 確認 `10.24.0.1` 和 `10.24.0.2` 可通。
3. 用 SSH key 確認地面站能登入 Orin。
4. Orin 不接飛機，先確認 `ros2 service list` 看得到 `/start_offboard`。
5. SITL 或拆槳狀態下測試 `/start_offboard`。
6. 實機上電前確認 RC 接管、kill switch、PX4 failsafe 參數與測試場域。
