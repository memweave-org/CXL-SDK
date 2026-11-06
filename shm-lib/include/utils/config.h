#ifndef SHM_DS_UTILS_H
#define SHM_DS_UTILS_H

/* Use CXL memory */
// #define USE_CXL

// #define USE_DISTRI_CXL

/* Assume No CC support */
// #define NO_CC

// #define OPT_NO_CC

#if defined(SHM_LIB_WITH_UBSM) || defined(SHM_LIB_WITH_UB)
    #define SHM_NT_UB
#endif

#ifdef NO_CC
    #define BYPASS_CACHE_DEFAULT (true)
    #if !defined(SHM_NT_UB) && !defined(QUEUE_SIM_CAS)
        #define QUEUE_SIM_CAS
    #endif
    #ifndef USE_CXL
        #define USE_CXL
    #endif
    #else
    #define BYPASS_CACHE_DEFAULT (false)
    #ifdef USE_MSG_QUEUE
        #ifndef SHM_NT_UB
            #define QUEUE_SIM_CAS
        #endif
        #ifndef USE_CXL
            #define USE_CXL
        #endif
    #endif
#endif

/* Memory allocation */

#define ALLOC_REMOTE_MEM
#ifdef ALLOC_REMOTE_MEM
    #ifdef NO_CC
        #define SHM_NODE 4
    #else
        #define SHM_NODE 0
    #endif
#endif


#ifdef NO_CC
/*
 * NO_CC has several modes
 * 1. NO_CC_CAS_W_FLUSH: CAS + FLUSH
 * 2. NO_CC_USE_NT: now with bug
 * 3. NO_CC_USE_CLFLUSH: use clflush instead of clflushopt
 * 4. NO_CC_MEM_FENCE: memory_fence after clflush
 * 5. NO_CC_WO_FLUSH_NODE: do not flush node
 */
#define NO_CC_CAS_W_FLUSH
// #define NO_CC_MEM_FENCE          // Useless option
// #define NO_CC_USE_NT
// #define NO_CC_WO_FLUSH_NODE
#define USE_CLWB
// #define STAT_NT_POINTER

/* Assume No CAS support */
// #define NO_CAS
// #define USE_DISAGR_CAS
// #define ENABLE_UNCACHE_MEM

/* Opt */
// #define OPT_GC
// #define OPT_ROOT_READ
// #define OPT_IN_USE_FLAG
// #define NT_SIM

#ifdef OPT_IN_USE_FLAG
// #define OPT_DUP_IN_USE_FLAG 4
#define OPT_PASS_CHAIN_HEAD_WITH_FLAG

// #define QUEUE_SIM_CAS
#endif

#endif

// The coordinated CXL run must retain real node publication and the original
// queued atomics. A split address layout alone does not select the UB backend.
#ifdef YCSB_CXL_TWO_PROCESS
#if defined(SHM_NT_UB) || defined(NT_SIM) || !defined(QUEUE_SIM_CAS)
#error "CXL A/B requires QUEUE_SIM_CAS without UB or NT_SIM"
#endif
#if defined(NO_CLFLUSH) || defined(NO_CC_WO_FLUSH_NODE) || (!defined(NO_CC) && !defined(USE_MSG_QUEUE))
#error "CXL A/B requires NO_CC with native cacheline maintenance enabled"
#endif
#endif

/* TIMING CONFIG */
// #define BWTREE_TIMING_ROOT_LOAD
// #define BWTREE_TIMING
// #define TIMING_BTREEOLC
// #define COUNTING
// #define TIMING_LOAD_BALANCE

// #define SNIPER

#endif
