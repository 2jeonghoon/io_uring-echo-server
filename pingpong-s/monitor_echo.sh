LOGFILE=cpu_usage.log
LATENCY_FILE="latency_dump.txt"

# Ctrl+C 처리: 종료 시 pidstat도 같이 종료되도록
cleanup() {
		echo ""		
		echo "🛑 서버와 모니터링을 종료합니다."
		kill $PIDSTAT_PID 2>/dev/null
		kill -INT $SERVER_PID 2>/dev/null
		wait $SERVER_PID 2>/dev/null

		# Ensure file I/O is completed
		if [ -f "$LATENCY_FILE" ]; then
				echo "✅ Latency data saved successfully to $LATENCY_FILE"
		fi
		
		echo "✅ CPU 로그가 저장되었습니다: $LOGFILE"
		exit 0
}
trap cleanup SIGINT

# 서버 실행 (백그라운드)
./napi-busy-poll-server -l -a "192.168.1.101" -b -s 1 -p 8050 -u &
SERVER_PID=$!

# pidstat로 CPU 사용량 로그 기록 (1초마다)
pidstat -p $SERVER_PID -t 1 > $LOGFILE &
PIDSTAT_PID=$!

echo "🚀 서버가 실행 중입니다. Ctrl+C를 눌러 종료하세요..."
wait $SERVER_PID
