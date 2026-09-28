#!/usr/bin/env bash
# Native C commit-time gate. Run from any directory; PYTHON selects Python 3.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/c-agent-gate"
PYTHON="${PYTHON:-python3}"
PYTHON_EXECUTABLE="$("$PYTHON" -c 'import sys; assert sys.version_info.major == 3; print(sys.executable)')"

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DUSBSCPI_BUILD_TESTS=ON \
    -DUSBSCPI_BUILD_HELPERS=ON \
    -DUSBSCPI_BUILD_SOCKET_GLUE=ON \
    -DUSBSCPI_BUILD_TINYUSB_GLUE=OFF \
    -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE"
cmake --build "$BUILD_DIR" --config Debug --parallel

cd "$BUILD_DIR"
ctest -C Debug --show-only=json-v1 > test-inventory.json
"$PYTHON_EXECUTABLE" - <<'PY'
import json
import sys

with open('test-inventory.json', encoding='utf-8') as inventory:
    registered = {test['name'] for test in json.load(inventory)['tests']}
required = {'test_usbscpi', 'test_glue_tinyusb',
            'test_scpi_tcp_smoke', 'test_stream_dataplane'}
if sys.platform.startswith('linux'):
    required.update({'test_ring_concurrent', 'test_can_capture'})
missing = required - registered
if missing:
    sys.exit('Required tests not registered: ' + ', '.join(sorted(missing)))
PY

# pipefail preserves CTest failures; verbose output includes zero-exit skips.
ctest -C Debug --verbose --output-on-failure 2>&1 | tee test-output.log
printf '\nHardware-dependent skips (not successful hardware validation):\n'
if ! grep -E 'SKIP(:| )' test-output.log; then
    printf 'No script-level hardware skips reported.\n'
fi
if ! grep -q 'test_can_capture' test-inventory.json; then
    printf 'CAN capture test is unavailable on this platform.\n'
fi
printf '\nRequired host checks passed; assess hardware coverage separately above.\n'
