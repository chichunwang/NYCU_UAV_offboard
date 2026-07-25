# 換網路

## 登錄網路
修改網路設定檔

```
sudo nano /etc/netplan/50-cloud-init.yaml
```

格式會像下面狀況

```
network:
  version: 2
  wifis:
    wlan0:
      dhcp4: true
      access-points:
        "UAV331":
          auth:
            key-management: "psk"
            password: "..."
```

照格式加入自己的網路，ex: 名字: bb， 密碼: 123

```
network:
  version: 2
  wifis:
    wlan0:
      dhcp4: true
      access-points:
        "UAV331":
          auth:
            key-management: "psk"
            password: "..."
        "bb":
          auth:
            key-management: "psk"
            password: "123"
```

ctrl+O儲存，ctrl+x退出

使用下列指令確認格式沒有寫錯

```
sudo netplan generate
```

無誤就用以下指令更新設定檔
```
sudo netplan apply
```


## 手動切換網路
以下指令可以看到目前檔案內存有幾個網路設定，如果需要手動切會網路也需要先看要切換到哪個ID。


```
sudo wpa_cli -i wlan0 list_networks
```

假設要切到ID1

```

sudo wpa_cli -i wlan0 select_network 1
sudo wpa_cli -i wlan0 reassociate
```


使用指令確認切換
```
iwgetid -r
```

看IP，方便知道ssh要連什麼
```
hostname -I
```
