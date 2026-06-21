# ESP32-S3 BLE 连接 / 配对 / 安全信息(USB SCPI)

通过 USB SCPI 让主机控制 ESP32-S3:扫描 → 连接 BLE 设备 → 配对(支持
passkey / 数字比较 / Just Works)→ 读回安全等级、配对结果、MAC、加密/认证/
绑定/密钥长度。

> 状态:已在硬件上验证。对真实树莓派 4(`D8:3A:DD:E4:7A:98`)完成 LE 安全
> 等级 4(LE Secure Connections + 认证 + 128bit)的配对。

---

## 1. 涉及文件

| 文件 | 作用 |
|---|---|
| `examples/esp32s3/main/ble_conn.c` / `.h` | 连接/配对状态机、GAP 事件回调、SCPI 后端接口 |
| `examples/esp32s3/main/ble_scan.c` / `.h` | 扫描;`ble_scan_addr()` 把地址+类型给连接复用 |
| `examples/esp32s3/main/app_main.c` | SCPI 命令注册、`ble_conn_init()`、`scan_init` 任务栈 12K |
| `examples/esp32s3/main/CMakeLists.txt` | 加入 `ble_conn.c` |
| `examples/esp32s3/sdkconfig.defaults` | NimBLE SMP + `CONFIG_BT_NIMBLE_NVS_PERSIST`(绑定持久化) |
| `examples/esp32s3/host/scan_test.py` | 主机测试脚本(`--connect <i>`) |
| `examples/esp32s3/README.md` | 用法 + 命令表 |

### SMP 配置(`ble_conn_init()`)

```c
ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;  // 主机 PC 充当键盘/显示
ble_hs_cfg.sm_bonding = 1;
ble_hs_cfg.sm_mitm    = 1;   // 要求认证 -> 触发 passkey / 数字比较
ble_hs_cfg.sm_sc      = 1;   // LE Secure Connections
ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
ble_store_config_init();     // 关键!见第 5 节
```

`KeyboardDisplay` 是逻辑声明,不要求物理键盘:passkey 由主机经
`BLE:PAIR:PASSKey` 注入,要显示的数字经 `BLE:PAIR:NUMCmp?` / `BLE:PAIR:PASSKey?`
读出。

---

## 2. SCPI 命令

| 命令 | 返回 | 说明 |
|---|---|---|
| `BLE:SCAN <秒>` | — | 启动被动扫描(默认 5s) |
| `BLE:SCAN:DONE?` | `0/1` | 扫描完成 |
| `BLE:SCAN:COUNt?` | uint | 设备数(≤20) |
| `BLE:SCAN? <i>` | CSV | `<addr>,<rssi>,"<name>",<adv_type>` |
| `BLE:CONNect <i>` | — | 连接扫描结果第 i 个(复用其地址+类型) |
| `BLE:CONNect:STATe?` | int | `0`空闲 `1`连接中 `2`已连接 `3`失败 |
| `BLE:CONNect:STATus?` | int | 最后的 NimBLE 状态/原因码(排错,0=正常) |
| `BLE:DISConnect` | — | 断开 |
| `BLE:PAIR` | — | 在当前连接上发起配对/加密 |
| `BLE:PAIR:STATe?` | int | `0`空闲 `1`进行中 `2`需passkey `3`需数字比较 `4`完成 `5`失败 `6`本机显示passkey |
| `BLE:PAIR:PASSKey <n>` | — | 注入 6 位 passkey(状态 2) |
| `BLE:PAIR:PASSKey?` | uint | 读本机生成的 passkey(状态 6,在对端输入) |
| `BLE:PAIR:NUMCmp?` | uint | 读待比较的 6 位数(状态 3) |
| `BLE:PAIR:CONFirm <0\|1>` | — | 数字比较确认/拒绝 |
| `BLE:SEC?` | CSV | `<mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>` |

`<level>` = LE 安全等级:`1` 无 / `2` 加密未认证(Just Works) /
`3` 加密+认证 / `4` 再加 128bit LE SC 密钥。
(注:`sec_state` 无显式 SC 标志,L4 用 `key_size==16` 近似;详见 ble_conn.c 注释。)

---

## 3. 烧录(重要:单 USB 板子)

板子单 USB 走 ESP32-S3 内部 PHY:**app 一运行就从 USB-Serial-JTAG
(`303a:1001`)切到 OTG/USBTMC(`1209:0001` → `/dev/usbtmc*`),JTAG 口消失**,
无法自动重烧。

重烧步骤:
```bash
# 1) 手动进下载模式:按住 BOOT/IO0 → 点一下 RESET/EN → 松开 BOOT
#    之后 303a:1001 会作为 /dev/ttyACMx 重新出现
# 2) 编译 + 烧录
. /home/tkxb/HDD/Projects/esp-idf/export.sh
cd /home/tkxb/Projects/esp32s3_demo/iotsploit-usb/examples/esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash     # 端口号以实际 ID_MODEL_ID=1001 的那个为准
```

> 控制台日志走 **UART0**(`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`),JTAG 串口读不到
> NimBLE 日志 —— 排错请用 `BLE:CONNect:STATus?` 之类 SCPI 诊断。

确认烧录口:
```bash
for p in /dev/ttyACM*; do echo "$p -> $(udevadm info -q property -n $p | grep ID_MODEL_ID)"; done
# ID_MODEL_ID=1001 即 USB-Serial-JTAG(下载口)
```

---

## 4. 测试

### 4.1 树莓派端准备(被测 BLE 外设)

ESP32-S3 **只能扫 LE 广播**;手机蓝牙设置页看到 Pi 是经典 BR/EDR,ESP 看不到。
必须在 Pi 上开 LE 广播 + 配对 agent。

```bash
ssh tkxb@192.168.50.80          # 密码 547236

# LE 可连接广播(否则 ESP 扫不到)
sudo btmgmt -i hci0 connectable on
sudo btmgmt -i hci0 bondable on
sudo btmgmt -i hci0 advertising on
sudo btmgmt -i hci0 info | grep "current settings"   # 应含 advertising

# 自动接受配对的 agent(数字比较自动确认),后台常驻
sudo setsid bash -c "python3 /tmp/agent.py > /tmp/agent.log 2>&1" </dev/null >/dev/null 2>&1 &
cat /tmp/agent.log              # "agent registered KeyboardDisplay"
```

> 没有 agent 的话,BlueZ 会在配对中途断链(HCI reason 0x13)。
> `/tmp/agent.py` 内容见第 8 节(KeyboardDisplay,`RequestConfirmation` 自动 yes);
> 它在 `/tmp`,Pi 重启会丢,需重新创建。

### 4.2 一键测试(仓库脚本)

```bash
cd /home/tkxb/Projects/esp32s3_demo/iotsploit-usb/examples/esp32s3
sudo python3 host/scan_test.py --ble-secs 6                 # 先看设备列表/索引
sudo python3 host/scan_test.py --ble-secs 6 --connect <i>   # 连接+配对第 i 个
```

### 4.3 手动逐条(理解协议/排错)

设备节点 `/dev/usbtmc1`(需 root)。

```bash
sudo python3 - <<'EOF'
import os,time
d=os.open("/dev/usbtmc1",os.O_RDWR)
def cmd(t,read=True):
    os.write(d,(t+"\n").encode())
    return os.read(d,4096).decode(errors="replace").strip() if read else None

cmd("BLE:SCAN 6",read=False)
while cmd("BLE:SCAN:DONE?")!="1": time.sleep(0.3)
idx=None
for i in range(int(cmd("BLE:SCAN:COUNt?"))):
    row=cmd(f"BLE:SCAN? {i}"); print(i,row)
    if "D8:3A:DD:E4:7A:98" in row.upper(): idx=i
print("target idx =",idx)

cmd(f"BLE:CONNect {idx}",read=False)
while cmd("BLE:CONNect:STATe?") not in ("2","3"): time.sleep(0.2)
print("connect:",cmd("BLE:CONNect:STATe?"),"status:",cmd("BLE:CONNect:STATus?"))

cmd("BLE:PAIR",read=False)
while True:
    ps=cmd("BLE:PAIR:STATe?")
    if   ps=="3": print("numcmp =",cmd("BLE:PAIR:NUMCmp?")); cmd("BLE:PAIR:CONFirm 1",read=False)
    elif ps=="2": cmd(f"BLE:PAIR:PASSKey {input('PIN: ')}",read=False)
    elif ps in ("4","5"): break
    time.sleep(0.3)
print("pair state:",ps)                 # 4=成功 5=失败
print("BLE:SEC? ->",cmd("BLE:SEC?"))     # <mac>,<level>,<enc>,<auth>,<bonded>,<keysize>
os.close(d)
EOF
```

预期:`BLE:SEC? -> D8:3A:DD:E4:7A:98,4,1,1,1,16`

### 4.4 抓包(可选,排查 SMP)

在 Pi 上:
```bash
sudo btmon -w /tmp/cap.btsnoop      # 逐包刷盘;触发连接/配对后 Ctrl-C
sudo btmon -r /tmp/cap.btsnoop      # 解码
```
> 别用 `btmon > file`(块缓冲,看着是空的)。

---

## 5. 已踩过的坑(供后续优化参考)

1. **`BLE:PAIR` 返回 ENOTSUP(8) 且不发 SMP 包** —— 根因不是 SM 没编进去
   (`NIMBLE_BLE_SM`=1),而是**漏注册绑定存储**:配对会走
   `ble_sm_chk_store_overflow → ble_store_read`,当 `store_read_cb` 为 NULL 时
   返回 ENOTSUP。修复:`ble_conn_init()` 里调用 `ble_store_config_init()`
   (手动声明 `void ble_store_config_init(void);`,无公开头文件)。

2. **反复连接把 NimBLE GAP 状态卡死** —— 多次失败尝试后 `ble_gap_connect`
   会同步立即失败(t+0.00s、状态从不进入"连接中"),复位即恢复。

3. **手机能看到 Pi ≠ ESP 能看到** —— 经典 BR/EDR vs LE,见 4.1。

4. **连上后 Pi 停止广播** —— 已连接的外设不再广播,重连前先 `BLE:DISConnect`。

---

## 6. 后续优化方向(TODO)

- [ ] **L3/L4 严格区分**:当前用 `key_size==16` 近似 SC;改为在
      `BLE_GAP_EVENT_ENC_CHANGE` 记录是否走了 LE SC,据此判定。
- [ ] **裸 MAC 连接**:补 `BLE:CONNect:ADDR <mac>,<type>`,不依赖扫描索引。
- [ ] **多连接**:当前只维护一个 `conn_handle`,可扩展为句柄数组。
- [ ] **主动扫描**:`ble_scan.c` 当前 `passive=1`,拿不到 scan response 名字;
      可加 `BLE:SCAN:ACTive` 开关。
- [ ] **GATT 操作**:连接+配对后做服务发现/读写特征(`BLE:GATT:*`)。
- [ ] **配对错误细分**:把 SMP 失败原因(reason code)透出到 SCPI。
- [ ] **解绑/清绑定**:`BLE:BOND:CLEAr`(`ble_store_util_delete_peer` / 清 NVS)。
- [ ] **断连原因**:`BLE:CONNect:STATus?` 已有,可在文档里给出常见码对照表。
- [ ] **并发安全**:跨任务共享状态目前用 volatile + 标志,若加复杂逻辑考虑加锁。

---

## 7. 相关板子/外设信息

- 板子:ESP32-S3,单 USB 内部 PHY(JTAG `303a:1001` ↔ OTG/USBTMC `1209:0001`)。
- ESP-IDF:`v5.2.2`,`. /home/tkxb/HDD/Projects/esp-idf/export.sh`。
- 测试外设:树莓派 4,`ssh tkxb@192.168.50.80`(pw 547236,免密 sudo),
  BD_ADDR `D8:3A:DD:E4:7A:98`,BlueZ,`/tmp/agent.py` 自动接受配对。

---

## 8. 附录:Pi 端自动接受配对 agent(`/tmp/agent.py`)

KeyboardDisplay 能力,数字比较/passkey 自动接受。Pi 上需 `python3-dbus` + `gi`
(树莓派系统默认有)。用 `sudo` 运行并注册为默认 agent。

```python
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
AGENT_PATH="/test/agent"; CAP="KeyboardDisplay"
class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self): pass
    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self,d,u): return
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self,d): print("RequestPinCode->0000",flush=True); return "0000"
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self,d): print("RequestPasskey->0",flush=True); return dbus.UInt32(0)
    @dbus.service.method("org.bluez.Agent1", in_signature="ouq", out_signature="")
    def DisplayPasskey(self,d,p,e): print("DisplayPasskey",p,flush=True)
    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def DisplayPinCode(self,d,c): print("DisplayPinCode",c,flush=True)
    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self,d,p): print("RequestConfirmation",p,"-> auto-yes",flush=True); return
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self,d): return
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self): pass
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus=dbus.SystemBus()
Agent(bus,AGENT_PATH)
mgr=dbus.Interface(bus.get_object("org.bluez","/org/bluez"),"org.bluez.AgentManager1")
mgr.RegisterAgent(AGENT_PATH,CAP)
mgr.RequestDefaultAgent(AGENT_PATH)
print("agent registered",CAP,flush=True)
GLib.MainLoop().run()
```

> 想测真实 **PIN 输入**(而非自动接受):把 Pi 这边换成 `DisplayOnly`/
> `KeyboardOnly` 能力或显示固定 passkey 的 agent,ESP 侧就会进入
> `BLE:PAIR:STATe? == 2`,再用 `BLE:PAIR:PASSKey <n>` 注入。
