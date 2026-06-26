# feature/lr24-communication

此分支用於開發 **地面站（Ground Control Station）與 Jetson Orin 之間的通訊模組**。

目前使用 **MicoAir LR24-F** 作為無線通訊設備，並先以 Serial／USB 串列通訊方式進行資料傳輸與測試。

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
| 通訊方式 | USB Serial |
| Serial Baud Rate | 115200 bps |
| USB Baud Rate | 115200 bps |
| 無線傳輸速率 | 8 kbps |
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