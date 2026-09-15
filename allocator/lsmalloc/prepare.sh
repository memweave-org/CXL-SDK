#!/bin/bash

data_path="/dev/shm/logcxl_data"
data_size=4
hashmap_path="/dev/shm/logcxl_hashmap"
hashmap_size=4

numa_node=0

numactl --membind=$numa_node dd if=/dev/zero of=$data_path bs=1G count=$data_size
chmod 666 $data_path

numactl --membind=$numa_node dd if=/dev/zero of=$hashmap_path bs=1G count=$hashmap_size
chmod 666 $hashmap_path