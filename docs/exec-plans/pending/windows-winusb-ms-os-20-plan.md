# Windows Single-Step / Zero-Zadig WinUSB via MS OS 2.0 (WCID) Descriptors

> **Status:** Implemented; Windows and physical-board validation pending.
> **Scope:** Firmware-only. **No changes required** to Rust (`host/rust`), Dart/Flutter, or Python host code.

## Problem

Composite `iotsploit-usb` firmware (currently `examples/esp32s3/main/app_main.c`) exposes two USB interfaces under one configuration (`bNumInterfaces = 2`, `bDeviceClass = 0x00`):

- **Interface 0 (`ITF_NUM_USBTMC`)**: USBTMC / USB488 (`0xFE / 0x03 / 0x01`, endpoints `0x01` OUT, `0x81` IN)
- **Interface 1 (`ITF_NUM_VENDOR`)**: Vendor-specific log tunnel (`0xFF / 0x00 / 0x00`, string `"IoTSploit Log"`, endpoints `0x02` OUT, `0x82` IN)

Keeping USBTMC and the log stream on separate interfaces is intentional (see `docs/add-log-endpoint.md`): it preserves compatibility with the Linux kernel `usbtmc` driver (`/dev/usbtmcN`) and TinyUSB's `usbtmc` class driver.

However, on Windows:

1. `usbccgp.sys` (USB Common Class Generic Parent) exposes each interface as a separate child device (`MI_00` and `MI_01`).
2. Neither interface has a built-in Windows class driver usable by `nusb`, and the firmware currently does not advertise Microsoft OS 2.0 (WCID) descriptors.
3. As a result, users must run Zadig **twice** per board — once for `MI_00` (USBTMC) and once for `MI_01` (IoTSploit Log).

With MS OS 2.0 descriptors, Windows initiates discovery through BOS and requests the descriptor set. The firmware reports `WINUSB` as the compatible ID for each interface, allowing Windows to bind its built-in WinUSB driver automatically. This replaces manual Zadig setup on a clean supported Windows installation; existing driver bindings must be checked separately.

## Why No Host / Dart / Python Changes Are Needed

- **Rust (`host/rust/src/usbtmc_raw.rs`)**: `nusb` on Windows already opens WinUSB-bound interfaces via `detach_and_claim_interface(iface_num)` for both `UsbtmcRaw` (Interface 0) and `UsbtmcLogReader` (Interface 1). It only requires `WinUSB` to be the active Windows driver on each interface.
- **Dart / Flutter & Python**: Higher layers sit above `iotsploit-host` / `nusb` (or `/dev/usbtmcN` on Linux) and do not inspect Windows driver bindings or BOS/WCID descriptors.
- **Linux & macOS**: Linux ignores BOS MS OS 2.0 descriptors (kernel `usbtmc` still claims Interface 0 and `nusb` still claims Interface 1), and macOS IOKit is unaffected.

## Proposed Firmware Changes

### 1. Device Descriptor (`bcdUSB = 0x0210`)

Windows 8.1+ only requests the BOS descriptor if `bcdUSB >= 0x0201`. Update `desc_device` in `examples/esp32s3/main/app_main.c`:

- Change `.bcdUSB = 0x0200` to `.bcdUSB = 0x0210`.
- If testing on a Windows machine that already enumerated the previous firmware, bump `.bcdDevice` (e.g., `0x0100` -> `0x0101`) so Windows does not reuse cached `usbflags` registry entries.

Changing the revision or clearing `usbflags` does not remove installed driver packages or prove a clean driver binding. Do not use either as a substitute for the clean Windows test below.

### 2. BOS Descriptor (`tud_descriptor_bos_cb`)

TinyUSB (bundled in `examples/esp32s3/managed_components/espressif__tinyusb/src/device/usbd.h`) already provides:

- `TUD_BOS_DESCRIPTOR(_total_len, _num_caps)`
- `TUD_BOS_MICROSOFT_OS_DESC_LEN` (`28`)
- `TUD_BOS_MS_OS_20_DESCRIPTOR(_desc_set_len, _vendor_code)`

Add `tud_descriptor_bos_cb(void)` returning a 1-capability BOS descriptor pointing to a vendor request code (e.g., `VENDOR_REQUEST_MICROSOFT = 1`) and the exact byte length of the MS OS 2.0 descriptor set.

### 3. MS OS 2.0 Descriptor Set (`tud_vendor_control_xfer_cb`)

Implement `tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)`:

- On `CONTROL_STAGE_SETUP`:
  - Require `request->bmRequestType == 0xC0` (device-recipient vendor IN), `request->bRequest == VENDOR_REQUEST_MICROSOFT`, `request->wValue == 0`, and `request->wIndex == 7` (`MS_OS_20_DESCRIPTOR_INDEX`).
  - Only for a matching request, return `tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, sizeof(desc_ms_os_20))`.
  - Return `false` for unsupported SETUP requests so TinyUSB stalls them. In particular, reject OUT requests: TinyUSB would copy received data into the supplied constant descriptor buffer.
- Return `true` for subsequent DATA/ACK stages without starting another transfer.
- The `desc_ms_os_20` descriptor set should contain:
  1. **Set Header** (`MS_OS_20_SET_HEADER_DESCRIPTOR`, Windows version `0x06030000` for Windows 8.1+)
  2. **Configuration Subset Header** (`MS_OS_20_SUBSET_HEADER_CONFIGURATION`, `bConfigurationValue = 0`)
  3. **Function Subset for Interface 0** (`MS_OS_20_SUBSET_HEADER_FUNCTION`, `bFirstInterface = ITF_NUM_USBTMC`):
     - `MS_OS_20_FEATURE_COMPATBLE_ID`: `"WINUSB"`
     - `MS_OS_20_FEATURE_REG_PROPERTY`: `DeviceInterfaceGUIDs` (`REG_MULTI_SZ`, e.g., a dedicated GUID for the USBTMC interface)
  4. **Function Subset for Interface 1** (`MS_OS_20_SUBSET_HEADER_FUNCTION`, `bFirstInterface = ITF_NUM_VENDOR`):
     - `MS_OS_20_FEATURE_COMPATBLE_ID`: `"WINUSB"`
     - `MS_OS_20_FEATURE_REG_PROPERTY`: `DeviceInterfaceGUIDs` (`REG_MULTI_SZ`, a distinct GUID for the log interface)

> **Note for other examples:** `examples/esp32s3/main/app_main.c` does not currently define `tud_vendor_control_xfer_cb` or `tud_descriptor_bos_cb`. If this is later adopted in `examples/stm32f4disco-socketcan/main.c`, extend its existing `tud_vendor_control_xfer_cb` rather than adding a duplicate definition.

### 4. Optional Documentation Update

Once validated, update `host/rust/README.md` (Windows section) to describe automatic WinUSB binding for the tested Windows versions and firmware. Keep Zadig instructions for non-WCID firmware and explain that existing third-party driver bindings may need manual migration. Descriptor support starts at Windows 8.1; do not claim that version was tested unless it was exercised.

## Verification Plan

1. **Build and descriptor validation:**
   - Run `tools/testing/test-c-full.sh` from the `iotsploit-usb` repository and build `examples/esp32s3` with `idf.py build` in the ESP-IDF environment. The host gate does not build this firmware.
   - Check BOS, set, configuration-subset, and function-subset lengths against the emitted bytes; verify UTF-16LE property encoding and the double-NUL terminator for each `REG_MULTI_SZ` value.
   - On hardware, verify the valid descriptor request succeeds and wrong-direction, wrong-recipient, wrong-value, and unknown requests stall without resetting the device.
2. **Windows (clean installation, no Zadig):**
   - Use a clean Windows 10/11 machine or VM with no matching third-party driver package in its driver store. Flash the updated ESP32-S3 firmware and connect it without installing a driver manually.
   - Confirm the composite parent remains bound to `usbccgp.sys`, and both `MI_00` (USBTMC) and `MI_01` (IoTSploit Log) bind through Microsoft's inbox `winusb.inf` to `WinUSB`. Record the selected INF and matching compatible ID for both children; the service name alone is insufficient evidence.
   - Run `iotsploit-host devices` and `iotsploit-host info`, then verify log streaming through the Flutter Device Log tab while issuing SCPI commands through the same application session.
   - Disconnect and reconnect the board and confirm both interfaces remain usable without manual setup.
3. **Windows (existing installation):**
   - Separately exercise a machine that previously enumerated the old firmware, including one with a Zadig-installed package. Record descriptor-cache behavior and the selected driver for each interface; document any manual migration needed rather than assuming a revision bump replaces an existing driver.
4. **Linux and macOS regression checks:**
   - On Linux, confirm `/dev/usbtmcN` enumerates on Interface 0 and can complete a SCPI query. Separately verify raw USB SCPI and `UsbtmcLogReader` on Interface 1 (`0x82`); raw USB claiming can detach the kernel USBTMC driver.
   - On macOS, verify raw USB discovery, SCPI queries, and log streaming.

Report gate results, the ESP32-S3 build result, board and Windows versions exercised, and any unavailable hardware or platform checks. Automatic binding is accepted only after the clean Windows test passes; a firmware build or an existing Zadig binding does not prove it.

## Implementation and Validation Record (2026-11-13)

- Added BOS and a 338-byte MS OS 2.0 descriptor set in the existing ESP32-S3 descriptor owner. Both interface subsets advertise `WINUSB` with distinct, stable `DeviceInterfaceGUIDs`; the USB interface and endpoint configuration is unchanged.
- Updated `bcdUSB` to `0x0210` and `bcdDevice` to `0x0101`; the vendor callback accepts only the specified device-recipient IN request and rejects unsupported SETUP requests.
- ESP-IDF v5.2.2 `idf.py build`: passed; image fits the app partition with 34% free. The build reports the pre-existing unused `cmd_ble_conn_state` warning.
- C gate: 0 failures; 6 tests exercised successfully, plus 1 script-level skip (`test_can_capture`, no `vcan0`). The initial sandbox run denied localhost sockets; rerunning with socket access passed.
- Focused validation parsed the descriptors from the built ELF and checked nested lengths, both compatible IDs, UTF-16LE properties, GUIDs, BOS capability, and device revision. A temporary harness exercised the actual callback body across all 256 request types, invalid request fields, DATA/ACK stages, and transfer-failure propagation; all checks passed.
- No board was flashed or exercised. Clean Windows binding, existing-installation migration, and physical Linux/macOS regression checks remain pending; automatic WinUSB binding is not yet hardware-verified.
