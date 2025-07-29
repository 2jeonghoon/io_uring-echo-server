BASE_PORT=8050
NUM_SERVERS=16
LOG_DIR="cpu_logs"
LATENCY_DIR="latency_dumps"
SERVER_BIN="./io_uring_echo_server"

mkdir -p "$LOG_DIR" "$LATENCY_DIR"

# 배열로 PID 저장
declare -a SERVER_PIDS
declare -a PIDSTAT_PIDS

cleanup() {
		echo -e "\n🛑 모든 서버와 모니터링을 종료합니다."

		for pid in "${PIDSTAT_PIDS[@]}"; do
				kill "$pid" 2>/dev/null
		done

		for pid in "${SERVER_PIDS[@]}"; do
				kill "$pid" 2>/dev/null
				wait "$pid" 2>/dev/null
		done

		echo "✅ 모든 로그가 저장되었습니다. ($LOG_DIR, $LATENCY_DIR)"
		exit 0
}

trap cleanup SIGINT

for ((i=0; i<NUM_SERVERS; i++)); do
		PORT=$((BASE_PORT + i))
		LATENCY_FILE="$LATENCY_DIR/result_${PORT}.txt"
		LOGFILE="$LOG_DIR/cpu_usage_${PORT}.log"
		echo "🚀 서버 시작: 포트 $PORT"
		$SERVER_BIN "$PORT" "$i" > "$LATENCY_FILE" 2>&1 &
		SERVER_PID=$!
		SERVER_PIDS+=("$SERVER_PID")
		pidstat -p "$SERVER_PID" -t 1 > "$LOGFILE" &
		PIDSTAT_PIDS+=("$!")

		sleep 60  # 다음 서버 실행까지 1분 대기
done

# 모든 서버가 종료될 때까지 대기
wait
