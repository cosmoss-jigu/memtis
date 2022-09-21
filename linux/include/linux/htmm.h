#include <uapi/linux/perf_event.h>

#define DEFERRED_SPLIT_ISOLATED 1

#define BUFFER_SIZE	512 /* 1MB */
#define CPUS_PER_SOCKET 20
#define MAX_MIGRATION_RATE_IN_MBPS  1024 /* 1024MB per sec */


/* pebs events */
#define DRAM_LLC_LOAD_MISS  0x1d3
#define NVM_LLC_LOAD_MISS   0x80d1
#define ALL_STORES	    0x82d0
#define ALL_LOADS	    0x81d0
#define STLB_MISS_STORES    0x12d0
#define STLB_MISS_LOADS	    0x11d0

/* tmm option */
#define HTMM_NO_MIG	    0x0
#define	HTMM_BASELINE	    0x1
#define HTMM_HUGEPAGE_OPT   0x2
#define HTMM_HUGEPAGE_OPT_V2	0x3

/**/
#define DRAM_ACCESS_CYCLES  150
#define NVM_ACCESS_CYCLES   500
#define DELTA_CYCLES	(NVM_ACCESS_CYCLES - DRAM_ACCESS_CYCLES)

#define MULTIPLIER  4

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

typedef struct huge_region {
    struct vm_area_struct *vma;
    struct list_head hr_entry;
    spinlock_t lock;
    unsigned long haddr;
    unsigned int hot_utils;
    unsigned int total_accesses;
    unsigned int cooling_clock;
} huge_region_t;

/* htmm_core.c */
extern void htmm_mm_init(struct mm_struct *mm);
extern void htmm_mm_exit(struct mm_struct *mm);
extern void __prep_transhuge_page_for_htmm(struct page *page);
extern void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
					 struct page *page);
extern void clear_transhuge_pginfo(struct page *page);
extern void copy_transhuge_pginfo(struct page *page,
				  struct page *newpage);
extern pginfo_t *get_compound_pginfo(struct page *page, unsigned long address);

extern void check_transhuge_cooling(void *arg, struct page *page, bool locked);
extern void check_base_cooling(pginfo_t *pginfo, struct page *page, bool locked);
extern void set_page_coolstatus(struct page *page, pte_t *pte, struct mm_struct *mm);

extern void set_lru_adjusting(struct mem_cgroup *memcg, bool inc_thres);

extern void update_pginfo(pid_t pid, unsigned long address, enum events e);
extern bool region_for_toptier(huge_region_t *region);

extern bool deferred_split_huge_page_for_htmm(struct page *page);
extern unsigned long deferred_split_scan_for_htmm(struct mem_cgroup_per_node *pn,
						  struct list_head *split_list);
extern void putback_split_pages(struct list_head *split_list, struct lruvec *lruvec);

extern bool check_split_huge_page(struct mem_cgroup *memcg, struct page *meta, bool hot);
extern bool move_page_to_deferred_split_queue(struct mem_cgroup *memcg, struct page *page);

extern void move_page_to_active_lru(struct page *page);
extern void move_page_to_inactive_lru(struct page *page);


extern struct page *get_meta_page(struct page *page);
extern unsigned int get_accesses_from_idx(unsigned int idx);
extern unsigned int get_idx(unsigned long num);
extern int get_base_idx(unsigned int num);
extern int get_skew_idx(unsigned int num);
extern unsigned int get_weight(uint8_t history);
extern void uncharge_htmm_pte(pte_t *pte, struct mem_cgroup *memcg);
extern void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg);
extern void charge_htmm_page(struct page *page, struct mem_cgroup *memcg);

extern long cal_huge_hotness(struct mem_cgroup *memcg, void *meta, bool huge);
extern bool is_hot_huge_page(struct page *meta);
extern bool is_hot_huge_page_v2(struct page *meta);
extern enum region_list hugepage_type(struct page *page);

extern void set_lru_split_pid(pid_t pid);
extern void adjust_active_threshold(pid_t pid);
extern void set_lru_cooling_pid(pid_t pid);

extern struct kmem_cache *huge_region_cachep;

extern huge_region_t *huge_region_alloc(void);
extern void huge_region_free(huge_region_t *node);
extern void *huge_region_lookup(struct mm_struct *mm, unsigned long addr);
extern void *huge_region_delete(struct mm_struct *mm, unsigned long addr);
extern void *huge_region_insert(struct mm_struct *mm, unsigned long addr,
				huge_region_t *node);

/* htmm_sampler.c */
extern int ksamplingd_init(pid_t pid, int node);
extern void ksamplingd_exit(void);

/* htmm_migrater.c */
#define HTMM_MIN_FREE_PAGES 256 * 10 // 10MB
extern unsigned long get_nr_lru_pages_node(struct mem_cgroup *memcg, pg_data_t *pgdat);
extern void add_memcg_to_kmigraterd(struct mem_cgroup *memcg, int nid);
extern void del_memcg_from_kmigraterd(struct mem_cgroup *memcg, int nid);
extern int kmigraterd_init(void);

/* htmm_cooler.c */
extern void register_memcg_for_cooling(struct mem_cgroup *memcg);
extern int kcoolingd_init(void);
