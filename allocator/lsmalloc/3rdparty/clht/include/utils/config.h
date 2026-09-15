#ifndef SHM_DS_UTILS_H
#define SHM_DS_UTILS_H

/* Use CXL memory */

// #define USE_DISTRI_CXL

/* Assume No CC support */
// #define NO_CC

// #define OPT_NO_CC

#ifdef NO_CC
    #define BYPASS_CACHE_DEFAULT (true)
    #ifndef QUEUE_SIM_CAS
        #define QUEUE_SIM_CAS
    #endif
    #define USE_CXL
    #else
    #define BYPASS_CACHE_DEFAULT (false)
    #ifdef USE_MSG_QUEUE
        #define QUEUE_SIM_CAS
        #define USE_CXL
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

/* TIMING CONFIG */
// #define BWTREE_TIMING_ROOT_LOAD
// #define BWTREE_TIMING
// #define TIMING_BTREEOLC
// #define COUNTING
// #define TIMING_LOAD_BALANCE

// #define SNIPER

#endif
