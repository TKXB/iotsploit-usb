# ESP32-S3 BLE SPP Security over USBTMC/SCPI Plan

## Goal

Create a new `iotsploit-usb` ESP32-S3 example that adapts Espressif's open-source
NimBLE `ble_spp/spp_server` example, exposes it over `iotsploit-usb`'s
USBTMC/SCPI control path, and prints the negotiated BLE security level.

The example should let a host connect over USBTMC, pair with a BLE central, and
query the current BLE connection security state with SCPI.

## Assumptions

- Target MCU: ESP32-S3.
- USB transport: TinyUSB USBTMC through existing `glue/usbscpi_tinyusb.c`.
- BLE stack: ESP-IDF NimBLE.
- Upstream source reference: ESP-IDF
  `examples/bluetooth/nimble/ble_spp/spp_server`.
- Host/UI changes are not required for the first version; the firmware should
  advertise command metadata through `SYSTem:HELP:DESCription?`.
- The SPP data path is secondary. The core purpose is BLE connection, pairing,
  and security-level reporting.

## New Example Location

Create:

```text
third_party/iotsploit-usb/examples/esp32s3-spp-security/
```

Proposed files:

```text
examples/esp32s3-spp-security/
  CMakeLists.txt
  README.md
  sdkconfig.defaults
  main/
    CMakeLists.txt
    app_main.c
    ble_spp_security.c
    ble_spp_security.h
    tusb_config.h
    idf_component.yml
  components/
    iotsploit-usb/
      CMakeLists.txt
```

## Source Reuse Strategy

Reuse from existing `iotsploit-usb` ESP32-S3 example:

- ESP-IDF project layout.
- TinyUSB USBTMC configuration.
- USB PHY setup.
- `usbscpi_tinyusb.c` glue integration.
- SCPI initialization and static buffers.
- USB descriptor pattern.
- `SYSTem:HELP:DESCription?` descriptor metadata pattern.

Adapt from ESP-IDF `ble_spp/spp_server`:

- NimBLE host initialization.
- SPP GATT service registration.
- BLE advertising setup.
- GAP event handling.
- Connection descriptor printing pattern using `ble_gap_conn_find()`.
- `struct ble_gap_conn_desc.sec_state` fields:
  - `encrypted`
  - `authenticated`
  - `bonded`
  - `key_size`

Do not copy the UART bridge unless it is needed for basic SPP behavior. The first
version can keep SPP minimal and focus on security reporting.

## BLE Security Design

The BLE module should track one active connection:

```c
static volatile uint16_t s_conn_handle;
static volatile int s_connected;
```

On connect, disconnect, connection update, and encryption change, refresh cached
security data from:

```c
struct ble_gap_conn_desc desc;
ble_gap_conn_find(s_conn_handle, &desc);
```

Configure NimBLE security:

```c
ble_hs_cfg.sm_bonding = 1;
ble_hs_cfg.sm_mitm = 1;
ble_hs_cfg.sm_sc = 1;
ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
```

Register the bond store:

```c
ble_store_config_init();
```

## Security Level Mapping

`BLE:SEC?` returns:

```text
<mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>
```

Level mapping:

| Level | Meaning |
|---|---|
| `1` | Not encrypted |
| `2` | Encrypted, unauthenticated, typically Just Works |
| `3` | Encrypted and authenticated |
| `4` | Encrypted, authenticated, and 16-byte key as an LE Secure Connections proxy |

Implementation sketch:

```c
int level = 1;
if (enc && !auth) {
    level = 2;
} else if (enc && auth) {
    level = (key_size >= 16) ? 4 : 3;
}
```

Caveat: NimBLE's public `ble_gap_sec_state` does not expose a perfect
"Secure Connections was used" flag in this path. `key_size == 16` is a practical
proxy, but legacy pairing can also negotiate a 16-byte key. If strict L4
classification becomes required, record pairing method details during the
security/encryption event path instead of relying only on key size.

## SCPI Interface

Proposed commands:

| Command | Type | Description |
|---|---|---|
| `BLE:ADV:STARt` | command | Start BLE SPP advertising |
| `BLE:ADV:STOP` | command | Stop advertising |
| `BLE:CONNect:STATe?` | query | `0` idle, `1` connected |
| `BLE:PAIR` | command | Initiate pairing/security on the active BLE connection |
| `BLE:SEC?` | query | Return `<mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>` |

Optional diagnostics:

| Command | Type | Description |
|---|---|---|
| `BLE:CONNect:STATus?` | query | Last GAP status/reason |
| `BLE:PAIR:STATe?` | query | Pairing state, if interactive pairing is added |
| `BLE:PAIR:PASSKey` | command | Submit passkey, if passkey input is needed |
| `BLE:PAIR:NUMCmp?` | query | Numeric comparison value, if needed |
| `BLE:PAIR:CONFirm` | command | Confirm numeric comparison, if needed |

For the first implementation, keep the command set minimal unless interactive
pairing is needed by the chosen security configuration.

## Descriptor Metadata

Advertise the commands through `SYSTem:HELP:DESCription?`.

If a workflow is useful, add:

```text
name=ble-security
type=trigger_poll_fetch or trigger_poll_interactive
```

For a minimal first version, simple command descriptors may be enough:

- Start advertising from UI.
- Pair from UI.
- Query `BLE:SEC?` from UI.

## Host Usage Example

Example pyvisa flow:

```python
import time
import pyvisa

d = pyvisa.ResourceManager().open_resource("USB0::0x1209::0x0001::0001::INSTR")

print(d.query("*IDN?"))
print(d.query("SYSTem:HELP:DESCription?"))

d.write("BLE:ADV:STARt")

# Connect from phone/central, then:
while int(d.query("BLE:CONNect:STATe?")) == 0:
    time.sleep(0.5)

d.write("BLE:PAIR")
time.sleep(2)

print(d.query("BLE:SEC?"))
```

Expected `BLE:SEC?` shape:

```text
D8:3A:DD:E4:7A:98,4,1,1,1,16
```

Meaning:

```text
<mac>,level=4,encrypted=1,authenticated=1,bonded=1,key_size=16
```

## Verification Plan

Build:

```sh
. /home/tkxb/HDD/Projects/esp-idf/export.sh
cd third_party/iotsploit-usb/examples/esp32s3-spp-security
idf.py set-target esp32s3
idf.py build
```

Flash:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

Host checks:

1. `*IDN?` returns the example ID string.
2. `SYSTem:HELP:DESCription?` includes the BLE security commands.
3. `BLE:ADV:STARt` starts advertising.
4. A phone or another BLE central can connect.
5. `BLE:PAIR` initiates pairing/security.
6. `BLE:SEC?` returns a valid CSV row with a nonzero security state after pairing.

## Risks

- Interactive pairing may require additional SCPI prompts for passkey or numeric
  comparison, depending on the central and IO capability negotiation.
- `key_size == 16` is only a proxy for level 4; strict LE Secure Connections
  reporting may require extra state captured during pairing.
- ESP32-S3 uses one 2.4 GHz radio shared by BLE and Wi-Fi, so this example should
  avoid running Wi-Fi workflows at the same time.
- Copying too much of `spp_server` would make the example noisy. Keep UART/SPP
  transport code out unless needed for the security demonstration.

## Success Criteria

- A new ESP32-S3 example builds cleanly.
- The example enumerates as USBTMC.
- SCPI can start BLE advertising and query connection state.
- After central pairing, `BLE:SEC?` prints:

```text
<mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>
```

- No host or UI code changes are required for the first version.
