//
//  ycsbc.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/19/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include <cstdint>
#include <numa.h>
#include <numaif.h>

#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "connection/establish.h"
#include "core/core_workload.h"
#include "core/db.h"
#include "core/delegator/load_manager.h"
#include "core/delegator/master_trx_manager.h"
#include "core/delegator/trx_thread_controller.h"
#include "core/perf.h"
#include "core/properties.h"
#include "core/real_workload.h"
#include "core/utils/inicpp.h"
#include "db/db_factory.h"
#include "follower/global_variable.h"
#include "shm/mm.h"
#include "utils/helper.h"
#include "utils/sim_id.h"

#ifdef LIMIT_ATOMIC
#include "atomic.hpp"
#endif

void PrepareShmEnv(inicpp::IniManager &ini) {
  inicpp::section cacheable_sec = ini["shm/cacheable"];
  std::string mem_type = cacheable_sec["mem_type"];
  std::string shm_path = cacheable_sec["device_path"];
  std::string base_string = cacheable_sec["mmap_base_addr"];
  void *base = base_string.empty()
                   ? nullptr
                   : (void *)std::stoull(base_string, nullptr, 0);
  size_t size = cacheable_sec["mem_size"]; // Unit is MB
  size *= 1024 * 1024;

  std::string alloc_backend = cacheable_sec.toString("allocator_backend");
  for (auto &ch : alloc_backend) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (alloc_backend.empty()) {
    alloc_backend = "memkind";
  }

  CacheableInitParams params{};
  params.device_path = shm_path.c_str();
  params.mmap_base = base;
  params.size_bytes = size;
  params.worker_machine_count = SimThreadInfo::worker_machine_count;
  params.worker_machine_id = SimThreadInfo::worker_machine_id;
  params.mem_type_cxl = (mem_type == "cxl");
  params.cxlalloc_heap_numa = -1;
  {
    int n = cacheable_sec.toInt("cxlalloc_heap_numa");
    if (cacheable_sec.getLine("cxlalloc_heap_numa") != -1) {
      params.cxlalloc_heap_numa = static_cast<int8_t>(n);
    }
  }
  params.thread_count = 256;
  {
    int tc = cacheable_sec.toInt("cxlalloc_thread_count");
    if (cacheable_sec.getLine("cxlalloc_thread_count") != -1 && tc > 0) {
      params.thread_count = static_cast<uint16_t>(tc);
    }
  }

  if (alloc_backend == "cxlalloc") {
    params.backend = CacheableAllocatorBackend::Cxlalloc;
  } else if (alloc_backend == "memkind") {
    params.backend = CacheableAllocatorBackend::Memkind;
  } else {
    std::cerr << "Unknown allocator_backend: " << alloc_backend << std::endl;
    exit(EXIT_FAILURE);
  }

  if (mem_type != "local" && mem_type != "cxl") {
    std::cerr << "Unknown cacheable memory type: " << mem_type << std::endl;
    exit(EXIT_FAILURE);
  }

  init_cacheable_allocator_unified(params);
#ifdef ENABLE_UNCACHE_MEM
  if (ini["shm"]["enable_uncacheable"]) {
    auto uncacheable_ini = ini["shm/uncacheable"];
    std::string mem_type = uncacheable_ini["mem_type"];
    std::string shm_path = uncacheable_ini["device_path"];
    uintptr_t base = uncacheable_ini["mmap_base_addr"];
    size_t size = uncacheable_ini["memory_size"];
    if (mem_type == "local") {
      init_uncacheable_allocator(shm_path.c_str(), (void *)base, size);
    }
    // else if(mem_type == "cxl") {
    //   init_cxl_cacheable_allocator(shm_path.c_str(), (void *)base, size);
    // }
    else {
      std::cerr << "Unknown uncacheable memory type: " << mem_type << std::endl;
      exit(EXIT_FAILURE);
    }
  }
#endif
}

auto build_dbs(utils::Properties &props) {
  auto dbs = ALLOC_AND_CONSTRUCT(cxl_vector<ycsbc::DB *>, cacheable.malloc);
  dbs->reserve(SimThreadInfo::worker_db_count);
  for (uint32_t i = 0; i < SimThreadInfo::worker_db_count; i++) {
    ycsbc::DB *db = ycsbc::DBFactory::CreateDB(props);
    if (!db) {
      std::cout << "Unknown database name " << props["dbname"] << std::endl;
      exit(0);
    }
    dbs->push_back(db);
  }
  return dbs;
}

auto build_workload(utils::Properties &props) {
  ycsbc::CoreWorkload *wl;
  bool use_real_trace = (props.GetProperty("tracename", "") != "");
  // No trace name means using synthetic workload
  if (use_real_trace) {
    wl = new ycsbc::RealWorkload();
  } else {
    wl = new ycsbc::CoreWorkload();
  }
  wl->Init(props);
  return wl;
}

void master_process(utils::Properties &props) {
  auto dbs = build_dbs(props);

  auto wl = build_workload(props);
  int total_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);

  auto begin = std::chrono::steady_clock::now();
  ycsbc::LoadManager load_manager(dbs, wl);
  load_manager.LoadData(SimThreadInfo::dispatcher_thread_count,
                        SimThreadInfo::worker_machine_count, total_ops);
  auto end = std::chrono::steady_clock::now();
  // std::cerr << "LoadData time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "ms" << std::endl;
  const double load_ms =
      std::chrono::duration<double, std::milli>(end - begin).count();
  const double load_kops_per_sec =
      load_ms > 0 ? total_ops / load_ms : 0.0;
  std::cerr << "Load throughput: " << load_kops_per_sec
            << " Kops/s" << std::endl;
  
#ifdef TRX_TYPE_STAT
  std::cerr << "\n=== Statistics after LoadData phase ===" << std::endl;
  for (uint32_t i = 0; i < SimThreadInfo::worker_db_count; i++) {
    std::cerr << "DB " << i << ": ";
    (*dbs)[i]->GetStats();
  }
  std::cerr << "========================================\n" << std::endl;
#endif
  
  auto machine_list =stoi(props.GetProperty("machinenum", "1")) == 1 ? std::vector<std::string>() :
      FollowerManager::split_machines(props.GetProperty("follower_list"));
  assert(machine_list.size() == SimThreadInfo::worker_machine_count - 1);

  double duration;
  uint64_t sum;

  size_t total_trx_ops =
      stoi(props.GetProperty(ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY));
  auto master_trx_manager =
      ALLOC_AND_CONSTRUCT(ycsbc::MasterTransactionManager, cacheable.malloc,
                          machine_list, dbs, wl, total_trx_ops);
  master_trx_manager->Execute(props, duration, sum);

  std::cerr << "dbname\t" << props["dbname"] << '\n'
            << "filename\t"
            << ((props.GetProperty("tracename", "") != "")
                    ? props["workloadname"]
                    : props["workload_file"]) << '\n'
            << "value size\t" << int(VALUE_ADDR_SIZE) << '\n';
  // print_define();
  std::cerr << "dispatcher\t" << SimThreadInfo::dispatcher_thread_count << '\n'
            << "worker\t" << SimThreadInfo::worker_thread_count << '\n'
            << "dbnum\t" << SimThreadInfo::worker_db_count << '\n'
            << "throughput\t" << (sum / duration / 1000000) << " Mops/s\n"
            << "duration(s)\t" << duration << '\n'
            << "total operations\t" << sum << '\n';

  PrintStatistics();

#ifdef TRX_TYPE_STAT
  for (uint32_t i = 0; i < SimThreadInfo::worker_db_count; i++) {
    auto &db = (*dbs)[i];
    std::cerr << "DB " << i << ": ";
    db->GetStats();
  }
#endif

  for (uint32_t i = 0; i < SimThreadInfo::worker_db_count; i++) {
    ycsbc::DB *db = (*dbs)[i];
#ifdef USE_CXL
    db->~DB();
    // cacheable.free(db);
#else
    delete db;
#endif
  }
}

void follower_process(utils::Properties &props) {
  std::string hexAddrStr = props.GetProperty("share_var");
  void *addr = nullptr;

  std::stringstream ss;
  ss << std::hex << hexAddrStr;
  ss >> addr;
  g_var_struct = (GlobalVariables *)addr;

  auto trx_thread_controller = g_var_struct->trx_thread_controller;

  trx_thread_controller->StartThreads();
  trx_thread_controller->WaitForLocalThreads();
  std::cerr << "Follower machine " << SimThreadInfo::worker_machine_id
            << " finished." << std::endl;
}

int main(const int argc, const char *argv[]) {
#ifdef LIMIT_ATOMIC
  cxl_std::init_atomic_library(0, getpid());
#endif

  InitStatistics();
  utils::Properties props;
  std::string file_name = utils::ParseCommandLine(argc, argv, props);
  inicpp::IniManager ini_manager(props.GetProperty("config_file", ""));

  int machineno = stoi(props.GetProperty("machineno", "0"));
  SimThreadInfo::setup_machine_ids(machineno);

  int machine_nr = stoi(props.GetProperty("machinenum", "1"));
  SimThreadInfo::setup_worker_machine_count(machine_nr);

#ifdef USE_MSG_QUEUE
  int dispatcher_nr = stoi(props.GetProperty("client_thread_count", "1"));
  int worker_nr = stoi(props.GetProperty("server_thread_count", "1"));
#else
  int dispatcher_nr = stoi(props.GetProperty("server_thread_count", "1"));
  int worker_nr = stoi(props.GetProperty("server_thread_count", "1"));
#endif

  SimThreadInfo::setup_worker_nr(worker_nr);
  SimThreadInfo::setup_dispatcher_nr(dispatcher_nr);

  int num_db = stoi(props.GetProperty("dbnum", "1"));
  SimThreadInfo::setup_worker_db_count(num_db);

  set_value_size(std::stoi(props.GetProperty("valuesize", "0")));

#ifdef CLEAR_CACHE
  uint64_t *clear_cache = new uint64_t[1024 * 1024 * 1024 / sizeof(uint64_t)];
  memset(clear_cache, 0, 1024 * 1024 * 1024);
#endif

  if (ini_manager["debug"]["follower"] && SimThreadInfo::is_follower()) {
    std::cout << "Follower debug mode enabled. Press Enter to continue...";
    std::cin.get();
  }

  PrepareShmEnv(ini_manager);
  if (SimThreadInfo::is_master()) {
    master_process(props);
  } else {
    follower_process(props);
  }
#ifdef LIMIT_ATOMIC
  cxl_std::exit_atomic_library();
#endif
}
