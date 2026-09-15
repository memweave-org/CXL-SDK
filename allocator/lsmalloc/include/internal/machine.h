#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FLUSH(addr) asm volatile("clwb (%0)" ::"r"(addr))
#define FENCE asm volatile("sfence" ::: "memory")

#ifdef FAULT_INJECTION
#include <random>
extern std::random_device rd;
extern std::mt19937 mt_rand;
extern std::uniform_int_distribution<int> dis;
#define POTENTIAL_FAULT      \
    if (dis(mt_rand) == 1) { \
        FENCE;               \
        _exit(0);            \
        FENCE;               \
    }
#define POTENTIAL_FAULT_REF            \
    if (shm->dis(shm->mt_rand) == 1) { \
        FENCE;                         \
        _exit(0);                      \
        FENCE;                         \
    }
#else
#define POTENTIAL_FAULT \
    {}
#define POTENTIAL_FAULT_REF \
    {}
#endif

static inline void* get_cxl_mm(const char* cxl_dev_path, size_t mmap_size) {
    int dev_fd = open(cxl_dev_path, O_RDWR);
    if (dev_fd <= 0) {
        fprintf(stdout, "open file %s error\n", cxl_dev_path);
        exit(-1);
    }

    void* buf = NULL;
    buf = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        printf("ERROR: %d\n", errno);
        exit(-1);
    }
    fprintf(stdout, "CXL dev %s is open\n", cxl_dev_path);
    return buf;
}

static inline uint64_t get_mac() {
    struct ifreq ifreq;
    int sock;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }
    strcpy(ifreq.ifr_name, "eth0");  // Currently, only get eth0
    /*
    if (ioctl (sock, SIOCGIFHWADDR, &ifreq) < 0)
    {
        perror ("ioctl");
        return -1;
    }
    */
    char mac[18];
    snprintf(mac, sizeof(mac), "%X:%X:%X:%X:%X:%X", (unsigned char)ifreq.ifr_hwaddr.sa_data[0],
             (unsigned char)ifreq.ifr_hwaddr.sa_data[1], (unsigned char)ifreq.ifr_hwaddr.sa_data[2],
             (unsigned char)ifreq.ifr_hwaddr.sa_data[3], (unsigned char)ifreq.ifr_hwaddr.sa_data[4],
             (unsigned char)ifreq.ifr_hwaddr.sa_data[5]);

    unsigned u[6];
    sscanf(mac, "%x:%x:%x:%x:%x:%x", u, u + 1, u + 2, u + 3, u + 4, u + 5);
    uint64_t result = 0;
    for (int i = 0; i < 6; i++) {
        result = (result << 8) + u[i];
    }
    return result;
}
