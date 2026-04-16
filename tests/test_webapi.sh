#!/bin/bash
#
# ucvm - Web API integration test
#
# Tests the ESP32 webserver API via curl.
# Usage: ./test_webapi.sh [host] [hex_file]
#   host:     ESP32 IP (default: 192.168.0.2)
#   hex_file: firmware to upload (default: examples/avr/uart_hello/uart_hello.hex)
#
set -e

HOST="${1:-192.168.0.2}"
HEX="${2:-examples/avr/uart_hello/uart_hello.hex}"
BASE="http://${HOST}"

GREEN='\033[92m'
RED='\033[91m'
CYAN='\033[96m'
RESET='\033[0m'
PASS=0; FAIL=0

check() {
    if [ $1 -eq 0 ]; then
        echo -e "  ${GREEN}PASS${RESET} $2"
        PASS=$((PASS+1))
    else
        echo -e "  ${RED}FAIL${RESET} $2"
        FAIL=$((FAIL+1))
    fi
}

echo -e "${CYAN}=== ucvm Web API Test ===${RESET}"
echo "Host: ${HOST}"

# 1. Status
echo -e "\n${CYAN}[1] GET /api/status${RESET}"
STATUS=$(curl -s "${BASE}/api/status")
echo "  $STATUS"
echo "$STATUS" | grep -q '"arch"'
check $? "Status returns arch"
echo "$STATUS" | grep -q '"state"'
check $? "Status returns state"

# 2. List bridge entries
echo -e "\n${CYAN}[2] GET /api/bridge${RESET}"
BRIDGE=$(curl -s "${BASE}/api/bridge")
echo "  $BRIDGE"
echo "$BRIDGE" | grep -q '"entries"'
check $? "Bridge returns entries array"

# 3. Add entry: MCU UART 0 -> Host UART 0 @ 9600 baud
echo -e "\n${CYAN}[3] POST /api/bridge (add UART mapping)${RESET}"
ADD=$(curl -s -X POST "${BASE}/api/bridge" \
  -H "Content-Type: application/json" \
  -d '{"action":"add","mp":1,"mi":0,"pin":0,"ht":1,"hi":0,"param":96,"flags":0}')
echo "  $ADD"
echo "$ADD" | grep -q '"ok":true'
check $? "Add entry succeeds"

# 4. Verify entry was added
BRIDGE2=$(curl -s "${BASE}/api/bridge")
echo "  $BRIDGE2"
echo "$BRIDGE2" | grep -q '"uart"'
check $? "Entry appears in listing"

# 5. Delete entry
echo -e "\n${CYAN}[4] POST /api/bridge (delete)${RESET}"
DEL=$(curl -s -X POST "${BASE}/api/bridge" \
  -H "Content-Type: application/json" \
  -d '{"action":"del","index":0}')
echo "  $DEL"
echo "$DEL" | grep -q '"ok":true'
check $? "Delete entry succeeds"

# 6. Save to NVS
echo -e "\n${CYAN}[5] POST /api/bridge (save)${RESET}"
SAVE=$(curl -s -X POST "${BASE}/api/bridge" \
  -H "Content-Type: application/json" \
  -d '{"action":"save"}')
echo "  $SAVE"
echo "$SAVE" | grep -q '"ok":true'
check $? "Save to NVS succeeds"

# 7. Upload firmware
echo -e "\n${CYAN}[6] POST /api/firmware${RESET}"
if [ -f "$HEX" ]; then
    FW=$(curl -s -X POST "${BASE}/api/firmware" \
      -H "Content-Type: text/plain" \
      --data-binary @"$HEX")
    echo "  $FW"
    echo "$FW" | grep -q '"ok":true'
    check $? "Firmware upload succeeds"
else
    echo "  Skipped (${HEX} not found)"
fi

# 8. Reset CPU
echo -e "\n${CYAN}[7] POST /api/reset${RESET}"
RST=$(curl -s -X POST "${BASE}/api/reset" \
  -H "Content-Type: application/json" \
  -d '{}')
echo "  $RST"
echo "$RST" | grep -q '"ok":true'
check $? "CPU reset succeeds"

# 9. Halt CPU
echo -e "\n${CYAN}[8] POST /api/reset (halt)${RESET}"
HALT=$(curl -s -X POST "${BASE}/api/reset" \
  -H "Content-Type: application/json" \
  -d '{"action":"halt"}')
echo "  $HALT"
echo "$HALT" | grep -q '"ok":true'
check $? "CPU halt succeeds"

# Summary
TOTAL=$((PASS+FAIL))
echo -e "\n\033[1mResults: ${PASS}/${TOTAL} passed\033[0m"
[ $FAIL -eq 0 ] && exit 0 || exit 1
