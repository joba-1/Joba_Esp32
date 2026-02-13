#!/bin/bash

# Script to test MIN_INTER_TX_SILENCE_MS values from 10 to 100ms in 10ms steps
# For each value: edit code, build, flash, wait 5min, query stats, record results
# Finally, set to the value with highest registers/sec

DEVICE_IP="joba-esp32-1"
USERNAME="admin"
PASSWORD="joba-1"

RESULTS_FILE="test_results.txt"
echo "Value(ms),RegistersPerSec,CollisionPercent" > "$RESULTS_FILE"

BEST_VALUE=10
BEST_REG_SEC=0

for VALUE in $(seq 10 10 100); do
    echo "Testing MIN_INTER_TX_SILENCE_MS = ${VALUE}ms"

    # Edit the source file
    sed -i "s/static constexpr uint32_t MIN_INTER_TX_SILENCE_MS = [0-9]\+;/static constexpr uint32_t MIN_INTER_TX_SILENCE_MS = ${VALUE};/" src/ModbusRTUFeature.cpp

    # Flash
    if ! /home/joachim/.platformio/penv/bin/pio run -t upload --environment serial; then
        echo "Flash failed for ${VALUE}ms"
        continue
    fi

    # Wait 5 minutes
    echo "Waiting 5 minutes for stats to accumulate..."
    sleep 300

    # Query stats
    STATS=$(curl -s -u "${USERNAME}:${PASSWORD}" "http://${DEVICE_IP}/api/modbus/status")
    if [ -z "$STATS" ]; then
        echo "Failed to get stats for ${VALUE}ms"
        continue
    fi

    # Parse JSON - assuming the fields exist
    # Note: Adjust parsing based on actual JSON structure
    OWN_SUCCESS=$(echo "$STATS" | jq -r '.ownRequestsSuccess // 0')
    OWN_FAILED=$(echo "$STATS" | jq -r '.ownRequestsFailed // 0')
    TOTAL_REQUESTS=$((OWN_SUCCESS + OWN_FAILED))
    if [ "$TOTAL_REQUESTS" -eq 0 ]; then
        COLLISION_PCT=0.00
    else
        COLLISION_PCT=$(awk "BEGIN { printf \"%.2f\", ($OWN_FAILED / $TOTAL_REQUESTS) * 100 }")
    fi

    # Registers/sec: assuming average 2 registers per request (adjust if needed)
    AVG_REGISTERS_PER_REQ=2
    REG_SEC=$(awk "BEGIN { printf \"%.2f\", ($OWN_SUCCESS * $AVG_REGISTERS_PER_REQ) / 300 }")

    echo "${VALUE},${REG_SEC},${COLLISION_PCT}" >> "$RESULTS_FILE"

    # Check if best
    if awk "BEGIN { exit !($REG_SEC > $BEST_REG_SEC) }"; then
        BEST_REG_SEC=$REG_SEC
        BEST_VALUE=$VALUE
    fi

    echo "Value: ${VALUE}ms, Reg/sec: ${REG_SEC}, Collision%: ${COLLISION_PCT}"
done

echo "Best value: ${BEST_VALUE}ms with ${BEST_REG_SEC} registers/sec"

# Set to best value
sed -i "s/static constexpr uint32_t MIN_INTER_TX_SILENCE_MS = [0-9]\+;/static constexpr uint32_t MIN_INTER_TX_SILENCE_MS = ${BEST_VALUE};/" src/ModbusRTUFeature.cpp

# Flash
if ! /home/joachim/.platformio/penv/bin/pio run -t upload --environment serial; then
    echo "Flash failed for ${BEST_VALUE}ms"
    continue
fi


echo "Set MIN_INTER_TX_SILENCE_MS to ${BEST_VALUE}ms"
echo "Results saved in $RESULTS_FILE"