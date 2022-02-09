/*
 * memory access sampling for hugepage-aware tiered memory management.
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/perf_event.h>

#include "../kernel/events/internal.h"

#include <linux/htmm.h>

struct task_struct *access_sampling = NULL;
struct perf_event ***mem_event;

static bool valid_va(unsigned long addr)
{
    if (!(addr >> (PGDIR_SHIFT + 9)) && addr != 0)
	return true;
    else
	return false;
}

static __u64 get_pebs_event(enum events e)
{
    switch (e) {
	case DRAMREAD:
	    return DRAM_LLC_LOAD_MISS;
	case NVMREAD:
	    return NVM_LLC_LOAD_MISS;
	case MEMWRITE:
	    return ALL_STORES;
	default:
	    return N_HTMMEVENTS;
    }
}

static int __perf_event_open(__u64 config, __u64 config1, __u64 cpu,
	__u64 type, __u32 pid)
{
    struct perf_event_attr attr;
    struct file *file;
    int event_fd;

    memset(&attr, 0, sizeof(struct perf_event_attr));

    attr.type = PERF_TYPE_RAW;
    attr.size = sizeof(struct perf_event_attr);
    attr.config = config;
    attr.config1 = config1;
    attr.sample_period = 10007;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
    attr.disabled = 0;
    attr.exclude_kernel = 0;
    attr.precise_ip = 1;

    
    event_fd = htmm__perf_event_open(&attr, pid, cpu, -1, 0);
    //event_fd = htmm__perf_event_open(&attr, -1, cpu, -1, 0);
    if (event_fd <= 0) {
	printk("[error htmm__perf_event_open failure] event_fd: %d\n", event_fd);
	return -1;
    }

    file = fget(event_fd);
    if (!file) {
	printk("invalid file\n");
	return -1;
    }
    mem_event[cpu][type] = fget(event_fd)->private_data;
    return 0;
}

static int pebs_init(pid_t pid, int node)
{
    int cpu, event;

    mem_event = kzalloc(sizeof(struct perf_event **) * CPUS_PER_SOCKET, GFP_KERNEL);
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	mem_event[cpu] = kzalloc(sizeof(struct perf_event *) * N_HTMMEVENTS, GFP_KERNEL);
    }
    
    printk("pebs_init\n");   
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    if (get_pebs_event(event) == N_HTMMEVENTS) {
		mem_event[cpu][event] = NULL;
		continue;
	    }

	    if (__perf_event_open(DRAM_LLC_LOAD_MISS, 0, cpu, event, pid))
		return -1;
	    if (htmm__perf_event_init(mem_event[cpu][event], BUFFER_SIZE))
		return -1;
	}
    }

    return 0;
}

static void pebs_disable(void)
{
    int cpu, event;

    printk("pebs disable\n");
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    if (mem_event[cpu][event])
		perf_event_disable(mem_event[cpu][event]);
	}
    }
}

static int ksamplingd(void *data)
{
    unsigned long long nr_sampled = 0;
    unsigned long long nr_throttled = 0;
    unsigned long long nr_unknown = 0;

    while (!kthread_should_stop()) {
	int cpu, event;
	
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	    for (event = 0; event < N_HTMMEVENTS; event++) {
		struct perf_buffer *rb;
		struct perf_event_mmap_page *up;
		struct perf_event_header *ph;
		struct htmm_event *he;
		unsigned long pg_index, offset;
		int page_shift;
		
		if (!mem_event[cpu][event])
		    continue;

		__sync_synchronize();

		rb = mem_event[cpu][event]->rb;
		if (!rb) {
		    printk("event->rb is NULL\n");
		    return -1;
		}
		/* perf_buffer is ring buffer */
		up = READ_ONCE(rb->user_page);
		if (READ_ONCE(up->data_head) == up->data_tail) {
		    continue;
		}
		/* read barrier */
		smp_rmb();
	
		page_shift = PAGE_SHIFT + page_order(rb);
		/* get address of a tail sample */
		offset = READ_ONCE(up->data_tail);
		pg_index = (offset >> page_shift) & (rb->nr_pages - 1);
		offset &= (1 << page_shift) - 1;
	
		ph = (void*)(rb->data_pages[pg_index] + offset);
		switch (ph->type) {
		    case PERF_RECORD_SAMPLE:
			he = (struct htmm_event *)ph;
			if (!valid_va(he->addr)) {
			    printk("invalid va: %llx\n", he->addr);
			    break;
			}

			/* TODO: update page info */
			nr_sampled++;
			break;
		    case PERF_RECORD_THROTTLE:
		    case PERF_RECORD_UNTHROTTLE:

			nr_throttled++;
		
			break;
		    default:
			nr_unknown++;
			break;
		}

		/* read, write barrier */
		smp_mb();
		WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
	    }
	}
    }

    printk("nr_sampled: %llu, nr_throttled: %llu, nr_unknown: %llu\n", nr_sampled, nr_throttled, nr_unknown);

    return 0;
}

static int ksamplingd_run(void)
{
    int err = 0;
    
    if (!access_sampling) {
	access_sampling = kthread_run(ksamplingd, NULL, "ksamplingd");
	if (IS_ERR(access_sampling)) {
	    err = PTR_ERR(access_sampling);
	    access_sampling = NULL;
	}
    }
    return err;
}

int ksamplingd_init(pid_t pid, int node)
{
    int ret;

    if (access_sampling)
	return 0;

    ret = pebs_init(pid, node);
    if (ret) {
	printk("htmm__perf_event_init failure... ERROR:%d\n", ret);
	return 0;
    }

    return ksamplingd_run();
}

void ksamplingd_exit(void)
{
    if (access_sampling) {
	kthread_stop(access_sampling);
	access_sampling = NULL;
    }
    pebs_disable();
}
