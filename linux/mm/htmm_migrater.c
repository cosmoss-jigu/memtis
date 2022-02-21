/*
 *
 */
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/mmzone.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/rmap.h>
#include <linux/delay.h>
#include <linux/node.h>
#include <linux/htmm.h>

#include "internal.h"

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)                   \
	do {                                                            \
		if ((_page)->lru.prev != _base) {                       \
			struct page *prev;				\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}                                                       \
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

void add_memcg_to_kmigraterd(struct mem_cgroup *memcg, int nid)
{
    struct mem_cgroup_per_node *mz, *pn = memcg->nodeinfo[nid];
    pg_data_t *pgdat = NODE_DATA(nid);

    if (!pgdat)
	return;
    
    if (pn->memcg != memcg)
	printk("memcg mismatch!\n");

    spin_lock(&pgdat->kmigraterd_lock);
    list_for_each_entry(mz, &pgdat->kmigraterd_head, kmigraterd_list) {
	if (mz == pn)
	    goto add_unlock;
    }
    list_add_tail(&pn->kmigraterd_list, &pgdat->kmigraterd_head);
add_unlock:
    spin_unlock(&pgdat->kmigraterd_lock);
}

void del_memcg_from_kmigraterd(struct mem_cgroup *memcg, int nid)
{
    struct mem_cgroup_per_node *mz, *pn = memcg->nodeinfo[nid];
    pg_data_t *pgdat = NODE_DATA(nid);
    
    if (!pgdat)
	return;

    spin_lock(&pgdat->kmigraterd_lock);
    list_for_each_entry(mz, &pgdat->kmigraterd_head, kmigraterd_list) {
	if (mz == pn) {
	    list_del(&pn->kmigraterd_list);
	    break;
	}
    }
    spin_unlock(&pgdat->kmigraterd_lock);
}

unsigned long get_nr_lru_pages_node(struct mem_cgroup *memcg, pg_data_t *pgdat)
{
    struct lruvec *lruvec;
    unsigned long nr_pages = 0;
    enum lru_list lru;

    lruvec = mem_cgroup_lruvec(memcg, pgdat);

    for_each_lru(lru)
	nr_pages += lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    
    return nr_pages;
}

static bool need_lowertier_promotion(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec;
    unsigned long lruvec_size;

    VM_BUG_ON(node_is_toptier(pgdat->node_id));

    lruvec = mem_cgroup_lruvec(memcg, pgdat);
    lruvec_size = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);

    return lruvec_size ? true : false;
}

static bool need_toptier_demotion(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    unsigned long nr_lru_pages, max_nr_pages;

    max_nr_pages = memcg->nodeinfo[pgdat->node_id]->max_nr_base_pages;
    nr_lru_pages = get_nr_lru_pages_node(memcg, pgdat);

    if (nr_lru_pages > max_nr_pages - HTMM_MIN_FREE_PAGES) {
	int target_nid = next_demotion_node(pgdat->node_id);
	pg_data_t *target_pgdat;
	
	if (target_nid == NUMA_NO_NODE)
	    return false;

	target_pgdat = NODE_DATA(target_nid);
	if (need_lowertier_promotion(target_pgdat, memcg))
	    return true;
    }

    return false;
}

static unsigned long node_free_pages(pg_data_t *pgdat)
{
    int z;
    long free_pages;
    long total = 0;

    for (z = pgdat->nr_zones - 1; z >= 0; z--) {
	struct zone *zone = pgdat->node_zones + z;
	long nr_high_wmark_pages;

	if (!populated_zone(zone))
	    continue;

	free_pages = zone_page_state(zone, NR_FREE_PAGES);
	free_pages -= zone->nr_reserved_highatomic;
	free_pages -= zone->lowmem_reserve[ZONE_MOVABLE];

	nr_high_wmark_pages = high_wmark_pages(zone);
	if (free_pages >= nr_high_wmark_pages)
	    total += (free_pages - nr_high_wmark_pages);
    }
    return (unsigned long)total;
}

static bool promotion_available(int target_nid, struct mem_cgroup *memcg,
	unsigned long *nr_to_promote)
{
    pg_data_t *pgdat;
    unsigned long max_nr_pages, cur_nr_pages;

    if (target_nid == NUMA_NO_NODE)
	return false;
    
    pgdat = NODE_DATA(target_nid);

    cur_nr_pages = get_nr_lru_pages_node(memcg, pgdat);
    max_nr_pages = memcg->nodeinfo[target_nid]->max_nr_base_pages;
    
    if (max_nr_pages == ULONG_MAX) {
	*nr_to_promote = node_free_pages(pgdat);
	return true;
    }
    else if (cur_nr_pages < max_nr_pages) {
	*nr_to_promote = max_nr_pages - cur_nr_pages;
	return true;
    }
    return false;
}

static bool need_lru_cooling(struct mem_cgroup_per_node *pn)
{
    return READ_ONCE(pn->need_cooling);
}

static __always_inline void update_lru_sizes(struct lruvec *lruvec,
	enum lru_list lru, unsigned long *nr_zone_taken)
{
    int zid;

    for (zid = 0; zid < MAX_NR_ZONES; zid++) {
	if (!nr_zone_taken[zid])
	    continue;

	update_lru_size(lruvec, lru, zid, -nr_zone_taken[zid]);
    }
}

static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru, struct list_head *dst,
	isolate_mode_t mode)
{
    struct list_head *src = &lruvec->lists[lru];
    unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
    unsigned long scan = 0, nr_taken = 0;
    LIST_HEAD(busy_list);

    while (scan < nr_to_scan && !list_empty(src)) {
	struct page *page;
	unsigned long nr_pages;

	page = lru_to_page(src);
	prefetchw_prev_lru_page(page, src, flags);
	VM_BUG_ON_PAGE(!PageLRU(page), page);
	 
	nr_pages = compound_nr(page);
	scan += nr_pages;

	if (!__isolate_lru_page_prepare(page, 0)) {
	    list_move(&page->lru, src);
	    continue;
	}
	if (unlikely(!get_page_unless_zero(page))) {
	    list_move(&page->lru, src);
	    continue;
	}
	if (!TestClearPageLRU(page)) {
	    put_page(page);
	    list_move(&page->lru, src);
	    continue;
	}

	nr_taken += nr_pages;
	nr_zone_taken[page_zonenum(page)] += nr_pages;
	list_move(&page->lru, dst);
    }

    update_lru_sizes(lruvec, lru, nr_zone_taken);
    return nr_taken;
}

static struct page *alloc_migrate_page(struct page *page, unsigned long node)
{
    int nid = (int) node;
    int zidx;
    struct page *newpage = NULL;
    gfp_t mask = (GFP_HIGHUSER_MOVABLE |
		  __GFP_THISNODE | __GFP_NOMEMALLOC |
		  __GFP_NORETRY | __GFP_NOWARN) &
		  ~__GFP_RECLAIM;

    if (PageHuge(page))
	return NULL;

    zidx = zone_idx(page_zone(page));
    if (is_highmem_idx(zidx) || zidx == ZONE_MOVABLE)
	mask |= __GFP_HIGHMEM;

    if (thp_migration_supported() && PageTransHuge(page)) {
	mask |= GFP_TRANSHUGE_LIGHT;
	newpage = __alloc_pages_node(nid, mask, HPAGE_PMD_ORDER);

	if (!newpage)
	    return NULL;

	prep_transhuge_page(page);
	__prep_transhuge_page_for_htmm(page);
    } else
	newpage = __alloc_pages_node(nid, mask, 0);

    return newpage;
}

static unsigned long migrate_page_list(struct list_head *migrate_list,
	pg_data_t *pgdat, bool promotion)
{
    int target_nid;
    unsigned int nr_succeeded = 0;

    if (promotion)
	target_nid = next_promotion_node(pgdat->node_id);
    else
	target_nid = next_demotion_node(pgdat->node_id);

    if (list_empty(migrate_list))
	return 0;

    if (target_nid == NUMA_NO_NODE)
	return 0;

    migrate_pages(migrate_list, alloc_migrate_page, NULL,
	    target_nid, MIGRATE_ASYNC, MR_NUMA_MISPLACED, &nr_succeeded);

    if (promotion)
	count_vm_events(HTMM_NR_PROMOTED, nr_succeeded);
    else
	count_vm_events(HTMM_NR_DEMOTED, nr_succeeded);

    return nr_succeeded;
}

static unsigned long shrink_page_list(struct list_head *page_list,
	pg_data_t* pgdat)
{
    LIST_HEAD(demote_pages);
    LIST_HEAD(ret_pages);
    unsigned long nr_reclaimed = 0;

    cond_resched();

    while (!list_empty(page_list)) {
	struct page *page;
	
	page = lru_to_page(page_list);
	list_del(&page->lru);

	if (!trylock_page(page))
	    goto keep;
	if (PageActive(page))
	    goto keep_locked;
	if (unlikely(!page_evictable(page)))
	    goto keep_locked;
	if (PageWriteback(page))
	    goto keep_locked;
	if (PageTransHuge(page) && !thp_migration_supported())
	    goto keep_locked;
	
	list_add(&page->lru, &demote_pages);
	unlock_page(page);
	continue;

keep_locked:
	unlock_page(page);
keep:
	list_add(&page->lru, &ret_pages);
    }

    nr_reclaimed = migrate_page_list(&demote_pages, pgdat, false);
    if (!list_empty(&demote_pages))
	list_splice(&demote_pages, page_list);

    list_splice(&ret_pages, page_list);
    return nr_reclaimed;
}

static unsigned long promote_page_list(struct list_head *page_list,
	pg_data_t *pgdat)
{
    LIST_HEAD(promote_pages);
    LIST_HEAD(ret_pages);
    unsigned long nr_promoted = 0;

    cond_resched();

    while (!list_empty(page_list)) {
	struct page *page;

	page = lru_to_page(page_list);
	list_del(&page->lru);

	if (!trylock_page(page))
	    goto __keep;
	if (!PageActive(page))
	    goto __keep_locked;
	if (unlikely(!page_evictable(page)))
	    goto __keep_locked;
	if (PageWriteback(page))
	    goto __keep_locked;
	if (PageTransHuge(page) && !thp_migration_supported())
	    goto __keep_locked;

	list_add(&page->lru, &promote_pages);
	unlock_page(page);
	continue;
__keep_locked:
	unlock_page(page);
__keep:
	list_add(&page->lru, &ret_pages);
    }

    nr_promoted = migrate_page_list(&promote_pages, pgdat, true);
    if (!list_empty(&promote_pages))
	list_splice(&promote_pages, page_list);

    list_splice(&ret_pages, page_list);
    return nr_promoted;
}

static unsigned long demote_inactive_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru)
{
    LIST_HEAD(page_list);
    pg_data_t *pgdat = lruvec_pgdat(lruvec);
    unsigned long nr_reclaimed = 0, nr_taken;
    int file = is_file_lru(lru);

    lru_add_drain();

    spin_lock_irq(&lruvec->lru_lock);
    nr_taken = isolate_lru_pages(nr_to_scan, lruvec, lru, &page_list, 0);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    if (nr_taken == 0)
	return 0;
    
    nr_reclaimed = shrink_page_list(&page_list, pgdat);

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &page_list);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&page_list);
    free_unref_page_list(&page_list);

    return nr_reclaimed;
}

static unsigned long promote_active_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru)
{
    LIST_HEAD(page_list);
    pg_data_t *pgdat = lruvec_pgdat(lruvec);
    unsigned long nr_taken, nr_promoted;
    
    lru_add_drain();

    spin_lock_irq(&lruvec->lru_lock);
    nr_taken = isolate_lru_pages(nr_to_scan, lruvec, lru, &page_list, 0);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON, nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    if (nr_taken == 0)
	return 0;

    nr_promoted = promote_page_list(&page_list, pgdat);

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &page_list);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON, -nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&page_list);
    free_unref_page_list(&page_list);

    return nr_promoted;
}

static unsigned long demote_lruvec(unsigned long nr_to_reclaim, short priority,
	pg_data_t *pgdat, struct lruvec *lruvec)
{
    enum lru_list lru;
    unsigned long nr_reclaimed = 0, nr_to_scan;

    for_each_evictable_lru(lru) {
	if (is_active_lru(lru))
	    continue;	
	
	if (is_file_lru(lru))
	    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
	else
	    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES) >> priority;

	nr_reclaimed += demote_inactive_list(nr_to_scan, lruvec, lru);
    }

    return nr_reclaimed;
}

static unsigned long promote_lruvec(unsigned long nr_to_promote, short priority,
	pg_data_t *pgdat, struct lruvec *lruvec)
{
    enum lru_list lru;
    unsigned long nr_promoted = 0, nr;
    
    nr = nr_to_promote >> priority;
    if (nr) {
	nr_promoted += promote_active_list(nr, lruvec, LRU_ACTIVE_ANON);
    }

    return nr_promoted;
}

static unsigned long demote_node(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    short priority = DEF_PRIORITY;
    unsigned long nr_to_reclaim = 0, nr_evictable_pages = 0, nr_reclaimed = 0;
    enum lru_list lru;

    nr_to_reclaim = MAX_MIGRATION_RATE_IN_MBPS * 256;
    nr_to_reclaim *= htmm_demotion_period_in_ms;
    nr_to_reclaim /= 1000; // max num. of demotable pages in this period.
    
    for_each_evictable_lru(lru) {
	if (is_active_lru(lru))
	    continue;
	nr_evictable_pages += lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    }
    nr_to_reclaim = min(nr_to_reclaim, nr_evictable_pages);
    
    do {
	nr_reclaimed += demote_lruvec(nr_to_reclaim, priority, pgdat, lruvec);
	if (nr_reclaimed >= nr_to_reclaim)
	    break;
	priority--;
    } while (priority);

    return nr_reclaimed;
}

static unsigned long promote_node(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    unsigned long nr_to_promote, nr_promoted = 0;
    short priority = DEF_PRIORITY;

    if (!promotion_available(next_promotion_node(pgdat->node_id), memcg, &nr_to_promote))
	return 0;

    nr_to_promote = min(nr_to_promote,
		    lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES));

    do {
	nr_promoted += promote_lruvec(nr_to_promote, priority, pgdat, lruvec);
	if (nr_promoted >= nr_to_promote)
	    break;
	priority--;
    } while (priority);
    
    return nr_promoted;
}

static void cooling_active_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru)
{
    unsigned long nr_taken;
    pg_data_t *pgdat = lruvec_pgdat(lruvec);
    LIST_HEAD(l_hold);
    LIST_HEAD(l_active);
    LIST_HEAD(l_inactive);
    int file = is_file_lru(lru);

    lru_add_drain();

    spin_lock_irq(&lruvec->lru_lock);
    nr_taken = isolate_lru_pages(nr_to_scan, lruvec, lru, &l_hold, 0);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    cond_resched();
    while (!list_empty(&l_hold)) {
	struct page *page;

	page = lru_to_page(&l_hold);
	list_del(&page->lru);

	if (unlikely(!page_evictable(page))) {
	    putback_lru_page(page);
	    continue;
	}

	if (!file) {
	    bool still_hot;

	    still_hot = cooling_page(page, lruvec_memcg(lruvec));
	    if (still_hot) { /* hot pages after cooling */
		list_add(&page->lru, &l_active);
		continue;
	    }
	}

	/* cold or file page */
	ClearPageActive(page);
	SetPageWorkingset(page);
	list_add(&page->lru, &l_inactive);
    }

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &l_active);
    move_pages_to_lru(lruvec, &l_inactive);
    list_splice(&l_inactive, &l_active);

    __mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&l_active);
    free_unref_page_list(&l_active);
}

static void cooling_node(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    unsigned long nr_to_scan, nr_scanned = 0;
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

    nr_to_scan = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);
    do {
	unsigned long scan = nr_to_scan >> 3; /* 12.5% */

	if (!scan)
	    scan = nr_to_scan;
	/* limits the num. of scanned pages to reduce the lock holding time */
	cooling_active_list(scan, lruvec, LRU_ACTIVE_ANON);
	nr_scanned += scan;
    } while (nr_scanned < nr_to_scan);

    /* active file list */
    cooling_active_list(lruvec_lru_size(lruvec, LRU_ACTIVE_FILE, MAX_NR_ZONES),
					lruvec, LRU_ACTIVE_FILE);
}

static struct mem_cgroup_per_node *next_memcg_cand(pg_data_t *pgdat)
{
    struct mem_cgroup_per_node *pn;

    spin_lock(&pgdat->kmigraterd_lock);
    if (!list_empty(&pgdat->kmigraterd_head)) {
	pn = list_first_entry(&pgdat->kmigraterd_head, typeof(*pn), kmigraterd_list);
	list_move_tail(&pn->kmigraterd_list, &pgdat->kmigraterd_head);
    }
    else
	pn = NULL;
    spin_unlock(&pgdat->kmigraterd_lock);

    return pn;
}

static int kmigraterd_demotion(pg_data_t *pgdat)
{
    //do_set_cpus_allowed(pgdat->kmigraterd, cpumask_of_node(pgdat->node_id));

    for ( ; ; ) {
	struct mem_cgroup_per_node *pn;
	struct mem_cgroup *memcg;

	if (kthread_should_stop())
	    break;

	pn = next_memcg_cand(pgdat);
	if (!pn) {
	    msleep_interruptible(1000);
	    continue;
	}

	memcg = pn->memcg;
	if (!memcg || !memcg->htmm_enabled) {
	    spin_lock(&pgdat->kmigraterd_lock);
	    if (!list_empty_careful(&pn->kmigraterd_list))
		list_del(&pn->kmigraterd_list);
	    spin_unlock(&pgdat->kmigraterd_lock);
	    continue;
	}

	/* demotes inactive lru pages */
	if (need_toptier_demotion(pgdat, memcg)) {
	    demote_node(pgdat, memcg); 
	}

	/* performs cooling */
	if (need_lru_cooling(pn)) {
	    //cooling_node(pgdat, memcg);
	}

	/* default: wait 100 ms */
	msleep_interruptible(htmm_demotion_period_in_ms);
    }
    return 0;
}

static int kmigraterd_promotion(pg_data_t *pgdat)
{
    int target_nid;
    const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

    //if (!cpumask_empty(cpumask))
//	do_set_cpus_allowed(pgdat->kmigraterd, cpumask);

    for ( ; ; ) {
	struct mem_cgroup_per_node *pn;
	struct mem_cgroup *memcg;

	if (kthread_should_stop())
	    break;
	
	pn = next_memcg_cand(pgdat);
	if (!pn) {
	    msleep_interruptible(1000);
	    continue;
	}

	memcg = pn->memcg;
	if (!memcg || !memcg->htmm_enabled) {
	    spin_lock(&pgdat->kmigraterd_lock);
	    if (!list_empty_careful(&pn->kmigraterd_list))
		list_del(&pn->kmigraterd_list);
	    spin_unlock(&pgdat->kmigraterd_lock);
	}

	/* promotes hot pages to fast memory node */
	if (need_lowertier_promotion(pgdat, memcg)) {
	    promote_node(pgdat, memcg);
	}
	
	if (need_lru_cooling(pn)) {
	    //cooling_node(pgdat, memcg);
	}
	
	msleep_interruptible(htmm_promotion_period_in_ms);
    }

    return 0;
}

static int kmigraterd(void *p)
{
    pg_data_t *pgdat = (pg_data_t *)p;
    int nid = pgdat->node_id;

    if (node_is_toptier(nid))
	return kmigraterd_demotion(pgdat);
    else
	return kmigraterd_promotion(pgdat);
}

/*void wakeup_kmigraterd(int nid)
{
    pg_data_t *pgdat = NODE_DATA(nid);
    
}*/

static void kmigraterd_run(int nid)
{
    pg_data_t *pgdat = NODE_DATA(nid);
    if (!pgdat || pgdat->kmigraterd)
	return;

    pgdat->kmigraterd = kthread_run(kmigraterd, pgdat, "kmigraterd%d", nid);
    if (IS_ERR(pgdat->kmigraterd)) {
	pr_err("Fails to start kmigraterd on node %d\n", nid);
	pgdat->kmigraterd = NULL;
    }
}

static void kmigraterd_stop(int nid)
{
    struct task_struct *km = NODE_DATA(nid)->kmigraterd;
    
    if (km) {
	kthread_stop(km);
	NODE_DATA(nid)->kmigraterd = NULL;
    }
}

int kmigraterd_init(void)
{
    int nid;

    for_each_node_state(nid, N_MEMORY)
	kmigraterd_run(nid);
    return 0;
}
