#!/usr/bin/env bash

# Get sudo password from environment variable or prompt user
# Usage: Set SUDO_PASSWORD environment variable, or script will prompt for password
if [ -z "$SUDO_PASSWORD" ]; then
    # Prompt for password if not set
    read -sp "Enter sudo password: " SUDO_PASSWORD
    echo
fi

create_symlink() {
    local target_name=$1
    ln -sf build_${target_name}/ycsbc_${target_name} ycsbc
}

# Test ORO performance with different CC configurations and message queue settings (Figure 11)
task_ycsb_ccconfig_workload_threadcnt_debug() {
    dir=./log/ycsb_ccconfig_workload_threadcnt_debug
    mkdir -p $dir
    configs=("cc_mq")
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=server_thread_scale_debug -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

# Fig.13 e-h
# Test ORO performance with different CC configurations and message queue settings (Figure 11)
task_ycsb_ccconfig_workload_threadcnt() {
    dir=./log/ycsb_ccconfig_workload_threadcnt
    mkdir -p $dir
    configs=(
        "cc"
        "cc_nocc_mq"
        "nocc_no_opt"
        "nocc"
    )
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=server_thread_scale_test -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

# Fig.14 b
# Test ORO performance with different CC configurations and message queue settings (Figure 11)
task_real_ccconfig_workload() {
    dir=./log/real_ccconfig_workload
    mkdir -p $dir
    # configs=("nocc" "nocc_no_opt")
    configs=(
        "cc_nocc_mq"
        "cc"
        "nocc_part_opt"
        "nocc_no_opt"
        "nocc"
    )
    # configs=("cc" "nocc")
    # configs=("nocc")
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=run_real_workloads -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

# Fig.15 b
task_bwtree_breakdown() {
    dir=./log/bwtree_breakdown
    mkdir -p $dir
    configs=(
        "nocc_no_opt"
        "nocc_bwtree_part1"
        "nocc_bwtree_part2"
        "nocc"
        "cc"
    )
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=bwtree_breakdown -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

# Fig.13 a-d
task_clevel_ccconfig_workload_threadcnt() {
    dir=./log/clevel_ccconfig_workload_threadcnt
    mkdir -p $dir
    configs=(
        "cc"
        "cc_nocc_mq"
        "nocc"
        "nocc_no_opt"
    )
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=clevel_server_thread_scale_test 2>&1 | tee $dir/${config}.log
    done
}

# Fig.14 a
# Test ORO performance with different CC configurations and message queue settings (Figure 11)
task_clevel_real_ccconfig_workload() {
    dir=./log/real_clevel_ccconfig_workload
    mkdir -p $dir
    # configs=("nocc" "nocc_no_opt")
    configs=(
        "nocc"
        "nocc_no_opt"
        "cc"
        "cc_nocc_mq"
    )
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=run_real_workloads -db=clevelhash 2>&1 | tee $dir/${config}.log
    done
}

# Fig.15 a
task_clevel_breakdown() {
    dir=./log/clevel_breakdown
    mkdir -p $dir
    configs=(
        "nocc_no_opt" 
        "nocc" 
        "cc"
    )
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=clevel_breakdown -db=clevelhash 2>&1 | tee $dir/${config}.log
    done
}

task_thread_scale_test() {
    dir=./log/ycsb
    mkdir -p $dir
    configs=("cc" "nocc")
    for config in "${configs[@]}"; do
        ./build.sh ${config}
        ./run_shm_ds.sh -mode=thread_scale_test -db=bwtree 2>&1 | tee $dir/${config}.log
    done
    # configs=("cc_mq" "cc_nocc_mq")
    # for config in "${configs[@]}"; do
    #     ./build.sh ${config}
    #     ./run_shm_ds.sh -mode=client_thread_scale_test -db=bwtree 2>&1 | tee $dir/${config}.log
    # done
}

task_record_scale_test() {
    dir=./log/ycsb
    mkdir -p $dir
    configs=("cc" "nocc")
    for config in "${configs[@]}"; do
        ./build.sh ${config}
        ./run_shm_ds.sh -mode=record_scale_test -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

task3() {
    dir=./log/read-heavy/max64
    mkdir -p $dir
    configs=("cc" "nocc" "nocc_dup_flag")
    for config in "${configs[@]}"; do
        create_symlink ${config}
        ./run_shm_ds.sh -mode=run_test4 -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

task_msg_queue() {
    dir=./log/imbalance
    mkdir -p $dir
    # configs=("cc_mq" )
    configs=("cc_nocc_mq")
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=run_msg_queue_test -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

task_dup_flag() {
    dir=./log/dup_flag
    mkdir -p $dir
    configs=("nocc" "nocc_dup_flag")
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        create_symlink ${config}
        ./run_shm_ds.sh -mode=test -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

task_super_large_real() {
    dir=./log/real
    mkdir -p $dir
    configs=("cc" "nocc")
    for config in "${configs[@]}"; do
        create_symlink ${config}
        for i in {1..2}; do
            ./run_shm_ds.sh -mode=real-100m -db=bwtree 2>&1 | tee $dir/${config}.log
        done
    done
}

# Check load imbalance when using multiple databases
task_imbalance_multi_db() {
    dir=./log/imbalance_multi_db
    mkdir -p $dir
    configs=("cc_mq")
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=run_imbalance_multi_db_test -db=bwtree 2>&1 | tee $dir/${config}.log
    done
}

# TODO: 1. radix_art_olc has compilation issues 2. clevelhash cc will hang
task_latency_overhead() {
    dir=./log/latency_overhead
    mkdir -p $dir
    # configs=("lat_cc" "lat_nocc_no_opt" "lat_nocc_no_opt_no_clflush")
    # configs=("lat_nocc_no_opt_no_clflush")
    # db_types=("clevelhash" "bwtree")
    # for config in "${configs[@]}"; do
    #     ./build.sh "${config}"
    #     for db_type in "${db_types[@]}"; do
    #         ./run_shm_ds.sh -mode=latency_overhead -db=${db_type} 2>&1 | tee $dir/${config}_${db_type}.log
    #     done
    # done
    # db_types2=("clht" "btree_olc" "hot" "masstree" "radix_art_olc")
    db_types2=("clht")
    # configs2=("lat_cc" "lat_nocc" "lat_nocc_no_clflush")
    configs2=("lat_nocc_no_opt_no_clflush" "lat_nocc")
    for config in "${configs2[@]}"; do
        ./build.sh "${config}"
        for db_type in "${db_types2[@]}"; do
            ./run_shm_ds.sh -mode=latency_overhead -db=${db_type} 2>&1 | tee $dir/${config}_${db_type}.log
        done
    done
}

task_various_value_size() {
    dir=./log/various_value_size
    mkdir -p $dir
    configs=("cc" "nocc" "nocc_no_opt" "cc_nocc_mq")
    for config in "${configs[@]}"; do
        ./build.sh "${config}"
        ./run_shm_ds.sh -mode=various-value-size 2>&1 | tee $dir/${config}.log
    done
}

echo "$SUDO_PASSWORD" | sudo -S ./prepare_env.sh

# task_msg_queue
# task_dup_flag
# task_super_large_real
# task_msg_queue
# task_imbalance_multi_db
# task_record_scale_test
# task_real_ccconfig_workload
# task_ycsb_ccconfig_workload_threadcnt
# task_ycsb_ccconfig_workload_threadcnt_debug
task_clevel_ccconfig_workload_threadcnt
# task_clevel_real_ccconfig_workload
# task_clevel_breakdown
# task_bwtree_breakdown
# task_latency_overhead
# task_various_value_size

echo "$SUDO_PASSWORD" | sudo -S ./reset_env.sh
