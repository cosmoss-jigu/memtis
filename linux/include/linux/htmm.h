


#define BUFFER_SIZE	512 /* 1MB */
#define CPUS_PER_SOCKET 20


/* pebs events */
#define DRAM_LLC_LOAD_MISS  0x1d3
#define NVM_LLC_LOAD_MISS   0x80d1
#define ALL_STORES	    0x82d0
#define ALL_LOADS	    0x81d0
#define STLB_MISS_STORES    0x12d0
#define STLB_MISS_LOADS	    0x11d0

struct htmm_event {
    struct perf_event_header header;
    __u64 ip;
    __u32 pid, tid;
    __u64 addr;
};


enum events {
    DRAMREAD = 0,
    NVMREAD = 1,
    MEMWRITE = 2,
    TLB_MISS_LOADS = 3,
    TLB_MISS_STORES = 4,
    N_HTMMEVENTS
};




extern int ksamplingd_init(pid_t pid, int node);
extern void ksamplingd_exit(void);
