#!/usr/bin/env bash
#
# ble_scan_test.sh — host test for the nRF52840 iotsploit-usb BLE scanner.
#
# Drives the SCPI command set over USBTMC (/dev/usbtmc0) and prints the BLE
# devices the firmware discovers. Pure bash + coreutils, no pyvisa needed.
#
# Usage:
#   sudo ./ble_scan_test.sh [seconds] [device]
#     seconds : how long to scan        (default 12)
#     device  : usbtmc character device (default /dev/usbtmc0)
#
# Exit codes: 0 ok, 1 setup/enumeration error, 2 wrong/absent firmware.

set -u

DUR="${1:-12}"
DEV="${2:-/dev/usbtmc0}"

# /dev/usbtmc0 is root-owned; re-exec under sudo if we can't open it.
if [[ ! -r "$DEV" || ! -w "$DEV" ]] && [[ $EUID -ne 0 ]]; then
    echo "Need root to access $DEV — re-running with sudo..."
    exec sudo "$0" "$DUR" "$DEV"
fi

if [[ ! -e "$DEV" ]]; then
    echo "ERROR: $DEV not found." >&2
    echo "  The board may not be flashed or enumerated. Check:" >&2
    echo "    lsusb | grep 1209        # expect 1209:0001 IoTSploit" >&2
    echo "    dmesg | tail" >&2
    echo "  If absent after a flash, the SoftDevice is likely missing —" >&2
    echo "  reflash with:  JLinkExe -CommanderScript flash_all.jlink" >&2
    exit 1
fi

# --- USBTMC helpers -------------------------------------------------------
# scpi_write CMD : send a command, expect no reply
# scpi_query CMD : send a query, echo the first non-empty reply line
scpi_write() {
    exec 3<>"$DEV"
    printf '%s\n' "$1" >&3
    exec 3<&-
}
scpi_query() {
    local reply
    exec 3<>"$DEV"
    printf '%s\n' "$1" >&3
    reply="$(timeout 2 head -c 256 <&3 2>/dev/null)"
    exec 3<&-
    # device pads the bulk-IN transfer with blank lines; keep the real answer
    printf '%s' "$reply" | tr -d '\r' | sed '/^[[:space:]]*$/d' | head -n1
}

# --- 1. identity ----------------------------------------------------------
echo "== Device on $DEV =="
IDN="$(scpi_query '*IDN?')"
echo "*IDN? -> ${IDN:-<no response>}"
if [[ "$IDN" != *nRF52840* ]]; then
    echo "ERROR: unexpected/empty *IDN? — wrong firmware or USBTMC stall." >&2
    exit 2
fi

# --- 2. fresh scan --------------------------------------------------------
echo
echo "== Scanning for ${DUR}s =="
scpi_write 'BLE:SCAN:CLEar'
scpi_write 'BLE:SCAN:START'

elapsed=0
step=2
while (( elapsed < DUR )); do
    sleep "$step"
    elapsed=$(( elapsed + step ))
    printf '  t=%2ss  state=%s  count=%s\n' \
        "$elapsed" "$(scpi_query 'BLE:SCAN:STATe?')" "$(scpi_query 'BLE:SCAN:COUNt?')"
done

scpi_write 'BLE:SCAN:STOP'

# --- 3. dump results ------------------------------------------------------
COUNT="$(scpi_query 'BLE:SCAN:COUNt?')"
COUNT="${COUNT//[!0-9]/}"   # keep digits only
COUNT="${COUNT:-0}"
echo
echo "== Found $COUNT device(s) =="
if (( COUNT == 0 )); then
    echo "  No advertisers seen. Sanity-check the RF environment, e.g.:"
    echo "    bluetoothctl scan on     # or a phone BLE scanner app"
    exit 0
fi

printf '  %-3s %-17s %-5s %s\n' "#" "MAC" "RSSI" "NAME"
for (( i = 0; i < COUNT; i++ )); do
    line="$(scpi_query "BLE:SCAN:RESult? $i")"   # MAC,RSSI,NAME
    mac="${line%%,*}"
    rest="${line#*,}"
    rssi="${rest%%,*}"
    name="${rest#*,}"
    printf '  %-3s %-17s %-5s %s\n' "$i" "$mac" "$rssi" "$name"
done

echo
echo "Done. (re-run to scan again; results are cleared on each START)"
