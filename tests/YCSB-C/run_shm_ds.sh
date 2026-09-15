#!/bin/bash

set -e

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/usr/local/lib"

db_types=("clht" "clevelhash" "hot" "btree_olc" "bwtree" "masstree" "radix_art_olc")
DB_TYPE="bwtree"
MODE="test"
# DBG_LEVEL=1
TEST_TYPE="fixed_db"
# USE_MSG_QUEUE=1s
CONFIG_TYPE=""
# ENABLE_CAT=""
ENABLE_SNIPER=0
PERF=0
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-}"

# threads actually do the processing
SERVER_THREADS_N=144
# threads which dispatch requests
CLIENT_THREADS_N=48
DB_NUM=1
MACHINE_NR=1
FOLLOWER_LIST="localhost"
CONFIG_PATH="${CONFIG_PATH:-config.ini}"
TRACE_PATH="${TRACE_PATH:-}"
TRACE_NAME="${TRACE_NAME:-2020Mar}"
DISABLE_ASLR="${DISABLE_ASLR:-0}"
VALUE_SIZE=8

output_file="output.txt"
throughput_file="throughput.txt"
metric_file="metric.txt"
repeat_count=3

workloads=("workloada_zipfian.spec" "workloadb_zipfian.spec" "workloadc_zipfian.spec" "workloadh_zipfian.spec")
real_workloads=(
"cluster001" "cluster002" "cluster003" "cluster004" "cluster005" "cluster006" "cluster007" "cluster008" "cluster009" "cluster010" "cluster011" "cluster012" "cluster013" "cluster014" "cluster015" "cluster016" "cluster017" "cluster018" "cluster019" "cluster020" "cluster021" "cluster022" "cluster023" "cluster024" "cluster025" "cluster026" "cluster027" "cluster028" "cluster029" "cluster030" "cluster031" "cluster032" "cluster033" "cluster034" "cluster035" "cluster036" "cluster037" "cluster038" "cluster039" "cluster040" "cluster041" "cluster042" "cluster043" "cluster044" "cluster045" "cluster046" "cluster047"
)

db_nums=(1)
thread_nums=(1 2 4 8 16 32 64 128)
client_thread_nums=(2 4 8 16 32 64 128)
value_size_nums=(8 64 512 1024 4096)

# args: 1. -db=DB_TYPE, 2. -debug=DBG_LEVEL, 3. -test_type=TEST_TYPE, 4. -use-msg=USE_MSG_QUEUE
for i in "$@"; do
	case $i in
	-db=*)
		DB_TYPE="${i#*=}"
		shift
		;;
	-mode=*)
		MODE="${i#*=}"
		shift
		;;
	-test_type=*)
		TEST_TYPE="${i#*=}"
		shift
		;;
	-use-msg=*)
		USE_MSG_QUEUE="${i#*=}"
		shift
		;;
	-config-type=*)
		CONFIG_TYPE="${i#*=}"
		shift
		;;
	-config=*)
		CONFIG_PATH="${i#*=}"
		shift
		;;
	-trace-path=*)
		TRACE_PATH="${i#*=}"
		shift
		;;
	-trace-name=*)
		TRACE_NAME="${i#*=}"
		shift
		;;
	*) ;;
	esac
done

get_ini_value() {
    local file="$1"
    local section="$2"
    local key="$3"
    
    # State variable
    local in_section=false
    
    # Read file line by line
    while IFS= read -r line; do
        # Clean line content: remove leading/trailing spaces and comments
        line_cleaned=$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/[;#].*$//')
        
        # Skip empty lines
        [[ -z "$line_cleaned" ]] && continue
        
        # Section processing
        if [[ "$line_cleaned" =~ ^\[([^]]+)\]$ ]]; then
            # Close current section state
            $in_section && in_section=false
            
            # Match target section
            [[ "${BASH_REMATCH[1]}" == "$section" ]] && in_section=true
            
        # Key-value processing (only in target section)
        elif $in_section; then
            if [[ "$line_cleaned" =~ ^([^=]+)=(.*)$ ]]; then
                # Extract and clean key-value
                current_key=$(echo "${BASH_REMATCH[1]}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
                current_value=$(echo "${BASH_REMATCH[2]}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
                
                # Match target key
                [[ "$current_key" == "$key" ]] && echo "$current_value" && return 0
            fi
        fi
    done < "$file"
    
    # Not found, return error
    return 1
}

enable_cat() {
	sudo pqos -I -e "llc:2=0x000f;"
	sudo pqos -I -a "pid:2=${1};"
}

disable_cat() {
	sudo pqos -I -R
}

run_cmd() {
    local cmd=$1
    local debug=${2:-false}
    if [ "$debug" = true ]; then
        cmd="gdb --args $cmd"
    fi
	if [ "$ENABLE_SNIPER" = 1 ]; then
		cmd="run-sniper -c cascade_lake.cfg -n 48 -v --roi -- $cmd"
	fi
	if [ "$PERF" = 1 ]; then
		if [ -z "$FLAMEGRAPH_DIR" ]; then
			echo "Set FLAMEGRAPH_DIR when PERF=1." >&2
			return 2
		fi
		cmd="perf record -F 99 -g -- $cmd"
	fi

	local original_aslr=""
	if [ "$DISABLE_ASLR" = 1 ]; then
		original_aslr=$(cat /proc/sys/kernel/randomize_va_space)
		echo 0 | sudo tee /proc/sys/kernel/randomize_va_space >/dev/null
	fi

	local shm_path=$(get_ini_value "$CONFIG_PATH" "shm/cacheable" "device_path" )
	local shm_size_mb=$(get_ini_value "$CONFIG_PATH" "shm/cacheable" "mem_size" )
	if [ -n "$shm_path" ] && [ -n "$shm_size_mb" ]; then
		if [ ! -e "$shm_path" ] || [ -f "$shm_path" ]; then
			truncate -s "${shm_size_mb}M" "$shm_path"
			sleep 1
		fi
	fi
	echo $cmd

	run_with_cat() {
        if [ "$ENABLE_CAT" = true ]; then
            disable_cat
            eval $1 &
            local pid=$!
            enable_cat $pid
            wait $pid
            disable_cat
        else
            eval $1
        fi
    }
    
	local run_status=0
	set +e
	if [ "$debug" != true ] && [ "$ENABLE_SNIPER" != 1 ]; then
        while true; do
            run_with_cat "bash -c \"$cmd\""
			run_status=$?
			[ $run_status -eq 124 ] && echo "Command timed out, restarting..." || break
        done
    else
        run_with_cat "$cmd"
		run_status=$?
    fi

	if [ "$PERF" = 1 ]; then
		sudo perf script > out.perf
		"$FLAMEGRAPH_DIR/stackcollapse-perf.pl" out.perf > out.folded
		"$FLAMEGRAPH_DIR/flamegraph.pl" out.folded > flamegraph.svg
		rm -f out.perf out.folded perf.data*
	fi

	if [ -n "$original_aslr" ]; then
		echo "$original_aslr" | sudo tee /proc/sys/kernel/randomize_va_space >/dev/null
	fi
	set -e
	return "$run_status"
}

run_ycsbc() {
    local debug=${2:-false}
	if [ ! -f "workloads/$1" ]; then
		echo "Missing workload: workloads/$1" >&2
		return 2
	fi
    local cmd="./ycsbc -db \"$DB_TYPE\" -machinenum \"$MACHINE_NR\" -follower_list \"$FOLLOWER_LIST\" -client_threads \"$CLIENT_THREADS_N\" -server_threads \"$SERVER_THREADS_N\" -dbnum \"$DB_NUM\" -valuesize $VALUE_SIZE -P \"workloads/$1\" -C $CONFIG_PATH"
    run_cmd "$cmd" $debug
}

require_trace_dataset() {
	if [ -z "$TRACE_PATH" ]; then
		echo "Twitter trace data is external. Set TRACE_PATH or pass -trace-path=/path/to/samples." >&2
		return 2
	fi
	if [ ! -d "$TRACE_PATH/$TRACE_NAME" ]; then
		echo "Trace directory not found: $TRACE_PATH/$TRACE_NAME" >&2
		return 2
	fi
}

run_real() {
    local debug=${2:-false}
	require_trace_dataset
    local cmd="./ycsbc -db \"$DB_TYPE\" -machinenum \"$MACHINE_NR\" -follower_list \"$FOLLOWER_LIST\"  -client_threads \"$CLIENT_THREADS_N\" -server_threads \"$SERVER_THREADS_N\" -dbnum \"$DB_NUM\" -valuesize $VALUE_SIZE -tracepath \"$TRACE_PATH\" -tracename \"$TRACE_NAME\" -workloadname \"$1\" -C \"$CONFIG_PATH\""
    run_cmd "$cmd" $debug
}

do_ycsb_task() {
  for workload in "${workloads[@]}"; do
    for thread_num in "${thread_nums[@]}"; do
      SERVER_THREADS_N=$thread_num
      run_ycsbc $workload
      sleep 1
    done
  done
}

do_real_task() {
  for workload in "${workloads[@]}"; do
    for thread_num in "${thread_nums[@]}"; do
      SERVER_THREADS_N=$thread_num
      run_real $workload
      sleep 1
    done
  done
}

case $MODE in 
"smoke")
	SERVER_THREADS_N=2
	run_ycsbc "workloada_small.spec"
	;;
"debug")
	SERVER_THREADS_N=2
	run_ycsbc "workloada_zipfian.spec" true
	;;
"test-small")
	SERVER_THREADS_N=32
	run_ycsbc "workloada_zipfian.spec"
	;;
"ycsb-a")
	CLIENT_THREADS_N=48
	thread_nums=(144)
	for thread_num in "${thread_nums[@]}"; do
		SERVER_THREADS_N=$thread_num
		run_ycsbc "workloada_zipfian.spec"
		sleep 1
	done
	# run_ycsbc "workloada_zipfian.spec"
	;;
"ycsb-b")
	CLIENT_THREADS_N=48
	thread_nums=(144)
	for thread_num in "${thread_nums[@]}"; do
		SERVER_THREADS_N=$thread_num
		run_ycsbc "workloadb_zipfian.spec"
		sleep 1
	done
	# run_ycsbc "workloada_zipfian.spec"
	;;
"ycsb-b-10m")
	CLIENT_THREADS_N=48
	thread_nums=(144)
	for thread_num in "${thread_nums[@]}"; do
		SERVER_THREADS_N=$thread_num
		run_ycsbc "workloadb_zipfian_10m.spec"
		sleep 1
	done
	;;
"test")
	CLIENT_THREADS_N=48
	thread_nums=(48)
	for thread_num in "${thread_nums[@]}"; do
		SERVER_THREADS_N=$thread_num
		run_ycsbc "workloade.spec"
		sleep 1
	done
	# run_ycsbc "workloada_zipfian.spec"
	;;
"run")
	for workload in "${workloads[@]}"; do
		run_ycsbc $workload 2>&1 | tee -a ./log/$DB_TYPE_$workload.log
		# sleep 5
	done
	;;
"ycsb_workload_threadcnt")
	local_workloads=(
		"workloada_zipfian.spec" 
		"workloadb_zipfian.spec" "workloadc_zipfian.spec" "workloadh_zipfian.spec" 
	)
	local_threads=(
		1 2 4 8 16 32 64 128
	)
	for workload in "${local_workloads[@]}"; do
		for thread_num in "${local_threads[@]}"; do
			SERVER_THREADS_N=$thread_num
			run_ycsbc $workload
			sleep 1
		done
	done
	;;
"clevel_breakdown")
	workloads=(
		"workloada_zipfian.spec" 
		"workloadb_zipfian.spec" 
		"workloadc_zipfian.spec" 
		"workloadh_zipfian.spec"
	)
	SERVER_THREADS_N=144
  DB_TYPE="clevelhash"
	for workload in "${workloads[@]}"; do
		run_ycsbc $workload
		sleep 1
	done
	;;
"bwtree_breakdown")
	workloads=(
		"workloada_zipfian.spec" 
		"workloadb_zipfian.spec" 
		"workloadc_zipfian.spec" 
		"workloadh_zipfian.spec"
	)
	SERVER_THREADS_N=144
	for workload in "${workloads[@]}"; do
		run_ycsbc $workload
		sleep 1
	done
	;;
"ycsb_bwtree_retry_count")
	SERVER_THREADS_N=48
	MACHINE_NR=1
	workloads=(
	"cluster001" "cluster002" "cluster003" "cluster004" "cluster005" "cluster006" "cluster007" "cluster008" "cluster009" "cluster010" "cluster011" "cluster012" "cluster013" "cluster014" "cluster015" "cluster016" "cluster017" "cluster018" "cluster019" "cluster020" "cluster021" "cluster022" "cluster023" "cluster024" "cluster025" "cluster026" "cluster027" "cluster028" "cluster029" "cluster030" "cluster031" "cluster032" "cluster033" "cluster034" "cluster035" "cluster036" "cluster037" "cluster038" "cluster039" "cluster040" "cluster041" "cluster042" "cluster043" "cluster044" "cluster045" "cluster046" "cluster047"
	)
	for workload in "${workloads[@]}"; do
		run_real $workload true
		sleep 1
	done
	;;
"read_latency_test")
	SERVER_THREADS_N=1
	run_real "workloadc_zipfian.spec"
	;;
"write_latency_test")
	SERVER_THREADS_N=1
	run_real "workloadh_zipfian.spec"
	;;
"latency_overhead")
	SERVER_THREADS_N=1
	run_ycsbc "workloada_zipfian_10m.spec"
	;;
"server_thread_scale_test")
  type=$CONFIG_TYPE
  workloads=(
	"workloada_zipfian_100m.spec"
	"workloadb_zipfian_100m.spec"
	"workloadc_zipfian_100m.spec"
	# "workloadh_zipfian.spec"
	)
	thread_nums=(24 48 72 96 120 144)
  for workload in "${workloads[@]}"; do
    for thread_num in "${thread_nums[@]}"; do
      SERVER_THREADS_N=$thread_num
      run_ycsbc $workload
      sleep 1
    done
  done
  ;;
"clevel_server_thread_scale_debug")
  type=$CONFIG_TYPE
  thread_nums=(144)
  DB_TYPE="clevelhash"
  workload="workloada_zipfian.spec"
  for thread_num in "${thread_nums[@]}"; do
	SERVER_THREADS_N=$thread_num
	run_ycsbc $workload
	sleep 1
  done
  ;;
"clevel_server_thread_scale_test")
  type=$CONFIG_TYPE
  DB_TYPE="clevelhash"
    workloads=(
	"workloada_zipfian_100m.spec" 
	"workloadb_zipfian_100m.spec" 
	"workloadc_zipfian_100m.spec" 
	# "workloadh_zipfian.spec"
	)
	thread_nums=(24 48 72 96 120 144)
  for workload in "${workloads[@]}"; do
    for thread_num in "${thread_nums[@]}"; do
      SERVER_THREADS_N=$thread_num
      run_ycsbc $workload
      sleep 1
    done
  done
  ;;
"client_thread_scale_test")
  type=$CONFIG_TYPE
  for workload in "${workloads[@]}"; do
    for thread_num in "${client_thread_nums[@]}"; do
      CLIENT_THREADS_N=$thread_num
      run_ycsbc $workload
      sleep 1
    done
  done
  ;;
"record_scale_test")
  type=$CONFIG_TYPE
  bworkloads=("workloadb_zipfian_50m.spec" "workloadb_uniform_50m.spec")
#   bworkloads=("workloadb_zipfian_20m.spec" "workloadb_zipfian_50m.spec" "workloadb_uniform_10m.spec" "workloadb_uniform_20m.spec" "workloadb_uniform_50m.spec")
  bthreads=(64 128)
  for workload in "${bworkloads[@]}"; do
	for thread_num in "${bthreads[@]}"; do
		SERVER_THREADS_N=$thread_num
		run_ycsbc $workload
		sleep 1
	done
  done
  ;;
"run_test3")
	require_trace_dataset
  type=$CONFIG_TYPE
  for workload in "${real_workloads[@]}"; do
		PRE_SERVER_THREADS_N=$SERVER_THREADS_N
		for thread_num in "${thread_nums[@]}"; do
			SERVER_THREADS_N=$thread_num
			./ycsbc -db "$DB_TYPE" -client_threads "$CLIENT_THREADS_N" -server_threads "$SERVER_THREADS_N" -dbnum "$DB_NUM" -tracepath "$TRACE_PATH" -tracename "$TRACE_NAME" -workloadname "$workload" 2>&1 | tee ./log/real/"$workload"_"$type"_"$SERVER_THREADS_N"_"$DB_NUM".log
			sleep 1
		done
		SERVER_THREADS_N=$PRE_SERVER_THREADS_N
		PRE_DB_NUM=$DB_NUM
		for db_num in "${db_nums[@]}"; do
			DB_NUM=$db_num
			./ycsbc -db "$DB_TYPE" -client_threads "$CLIENT_THREADS_N" -server_threads "$SERVER_THREADS_N" -dbnum "$DB_NUM" -tracepath "$TRACE_PATH" -tracename "$TRACE_NAME" -workloadname "$workload" 2>&1 | tee ./log/real/"$workload"_"$type"_"$SERVER_THREADS_N"_"$DB_NUM".log
			sleep 1
		done
		DB_NUM=$PRE_DB_NUM
  done
  ;;
"run_real_workloads_debug")
	SERVER_THREADS_N=144
	DB_TYPE="clevelhash"
	real_workloads=(
"cluster001" 
)
	for workload in "${real_workloads[@]}"; do
		run_real $workload true
		sleep 1
	done
	;;
"clevel_run_real_workloads")
	SERVER_THREADS_N=144
	DB_TYPE="clevelhash"
	for workload in "${real_workloads[@]}"; do
		run_real $workload
		sleep 1
	done
	;;
"clevel_run_real_workloads_comple")
	SERVER_THREADS_N=144
	DB_TYPE="clevelhash"
	real_workloads=(
	"cluster008"
	)
	for workload in "${real_workloads[@]}"; do
		run_real $workload
		sleep 1
	done
	;;
"run_real_workloads")
	SERVER_THREADS_N=144
	for workload in "${real_workloads[@]}"; do
		run_real $workload
		sleep 1
	done
	;;
"run_imbalance_multi_db_test")
	for db_num in 1 2 4 8 16; do
		DB_NUM=$db_num
		run_ycsbc "workloadb_zipfian.spec"
	done
	;;
"real")
	run_real "cluster002"
	;;
"real-10m")
	run_real "cluster020-10m" true
	;;
"real-100m")
	run_real "cluster017-100m"
	;;
"gdb-real")
	run_real "cluster012" true
	;;
"real-benchmark")
	require_trace_dataset
	for db_type in "${db_types[@]}"; do
	./ycsbc -db "$db_type" -client_threads "$CLIENT_THREADS_N" -server_threads "$SERVER_THREADS_N" -dbnum "$DB_NUM" -tracepath "$TRACE_PATH" -tracename "$TRACE_NAME" -workloadname "cluster001"
	sleep 5
	done
	;;
"ycsbc-benchmark")
	for db_type in "${db_types[@]}"; do
	./ycsbc -db "$db_type" -client_threads "$CLIENT_THREADS_N" -server_threads "$SERVER_THREADS_N" -dbnum "$DB_NUM" -P "workloads/workloada.spec"
	sleep 5
	done
	;;
"various-value-size")
	db_types=("bwtree" "clevelhash")
	# workloads=("workloada_zipfian.spec" "workloadb_zipfian.spec" "workloadc_zipfian.spec")
	workloads=("workloada_zipfian_100m.spec" "workloadb_zipfian_100m.spec" "workloadc_zipfian_100m.spec")
	value_size_nums=(8 64 512 1024 4096)
	for db_type in "${db_types[@]}"; do
		DB_TYPE=$db_type
		for workload in "${workloads[@]}"; do
			for value_size in "${value_size_nums[@]}"; do
				VALUE_SIZE=$value_size
				run_ycsbc $workload
				sleep 1
			done
		done
	done
	;;
*)
	echo "Invalid MODE: $MODE"
	;;
esac
