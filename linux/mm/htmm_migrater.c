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

static unsigned long need_lowertier_promotion(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec;
    unsigned long lruvec_size;

    lruvec = mem_cgroup_lruvec(memcg, pgdat);
    lruvec_size = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);
    
    if (htmm_mode == HTMM_NO_MIG)
	return 1;

    return lruvec_size;
}

static bool need_toptier_demotion(pg_data_t *pgdat, struct mem_cgroup *memcg, unsigned long *nr_exceeded)
{
    unsigned long nr_lru_pages, max_nr_pages;
    unsigned long nr_need_promoted;
    int target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);
    pg_data_t *target_pgdat;
  
    if (target_nid == NUMA_NO_NODE)
	return false;

    target_pgdat = NODE_DATA(target_nid);

    max_nr_pages = memcg->nodeinfo[pgdat->node_id]->max_nr_base_pages;
    nr_lru_pages = get_nr_lru_pages_node(memcg, pgdat);

    if (READ_ONCE(memcg->nodeinfo[pgdat->node_id]->need_demotion))
	return true;

    if (nr_lru_pages + HTMM_MIN_FREE_PAGES <= max_nr_pages)
	return false;

    nr_need_promoted = need_lowertier_promotion(target_pgdat, memcg);
    if (nr_lru_pages + nr_need_promoted <= max_nr_pages)
	return false;

    if (nr_lru_pages > max_nr_pages) 
	*nr_exceeded = (nr_lru_pages - max_nr_pages);

    return true;
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
    unsigned long nr_isolated;

    if (target_nid == NUMA_NO_NODE)
	return false;
    
    pgdat = NODE_DATA(target_nid);

    cur_nr_pages = get_nr_lru_pages_node(memcg, pgdat);
    max_nr_pages = memcg->nodeinfo[target_nid]->max_nr_base_pages;
    nr_isolated = node_page_state(pgdat, NR_ISOLATED_ANON) +
		  node_page_state(pgdat, NR_ISOLATED_FILE);

    if (max_nr_pages == ULONG_MAX) {
	*nr_to_promote = node_free_pages(pgdat);
	return true;
    }
    else if (cur_nr_pages + nr_isolated < max_nr_pages) {
	/* almost isolated pages would be demoted to the capacity-tier,
	 * so we add some value (HTMM_MIN_FREE_PAGES) in the above 'if' statement. */
	*nr_to_promote = max_nr_pages - cur_nr_pages - nr_isolated;
	return true;
    }
    return false;
}

static bool need_lru_cooling(struct mem_cgroup_per_node *pn)
{
    return READ_ONCE(pn->need_cooling);
}

static bool need_lru_adjusting(struct mem_cgroup_per_node *pn)
{
    return READ_ONCE(pn->need_adjusting);
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

    //if (list_empty(src) && lru == LRU_ACTIVE_ANON)
//	printk("isolate_lru_pages -> lru is empty\n");

    while (scan < nr_to_scan && !list_empty(src)) {
	struct page *page;
	unsigned long nr_pages;

	page = lru_to_page(src);
	prefetchw_prev_lru_page(page, src, flags);
	//VM_BUG_ON_PAGE(!PageLRU(page), page);
	VM_WARN_ON(!PageLRU(page));

	nr_pages = compound_nr(page);
	scan += nr_pages;

/*	if (PageNeedSplit(page)) {
	    list_move(&page->lru, src);
	    continue;
	}*/

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

	prep_transhuge_page(newpage);
	__prep_transhuge_page_for_htmm(newpage);
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
	target_nid = htmm_cxl_mode ? 0 : next_promotion_node(pgdat->node_id);
    else
	target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);

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
	pg_data_t* pgdat, struct mem_cgroup *memcg, bool shrink_active,
	unsigned long nr_to_reclaim)
{
    LIST_HEAD(demote_pages);
    LIST_HEAD(ret_pages);
    unsigned long nr_reclaimed = 0;
    unsigned long nr_demotion_cand = 0;

    cond_resched();

    while (!list_empty(page_list)) {
	struct page *page;
	
	page = lru_to_page(page_list);
	list_del(&page->lru);

	if (!trylock_page(page))
	    goto keep;
	if (!shrink_active && PageActive(page))
	    goto keep_locked;
	if (unlikely(!page_evictable(page)))
	    goto keep_locked;
	if (PageWriteback(page))
	    goto keep_locked;
	if (PageTransHuge(page) && !thp_migration_supported())
	    goto keep_locked;
	if (nr_demotion_cand > nr_to_reclaim + HTMM_MIN_FREE_PAGES)
	    goto keep_locked;

	if (htmm_static_thres == 0 && PageAnon(page)) {
	    if (PageTransHuge(page)) {
		struct page *meta = get_meta_page(page);

		if (meta->idx >= memcg->warm_threshold)
		    goto keep_locked;
	    } else {
		unsigned int idx = get_pginfo_idx(page);

		if (idx >= memcg->warm_threshold)
		    goto keep_locked;
	    }
	}

	list_add(&page->lru, &demote_pages);
	nr_demotion_cand += compound_nr(page);
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
	if (!PageActive(page) && htmm_mode != HTMM_NO_MIG)
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
	unsigned long nr_to_reclaim, struct lruvec *lruvec,
	enum lru_list lru, bool shrink_active)
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

    if (nr_taken == 0) {
	return 0;
    }

    nr_reclaimed = shrink_page_list(&page_list, pgdat, lruvec_memcg(lruvec),
	    shrink_active, nr_to_reclaim);

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
	pg_data_t *pgdat, struct lruvec *lruvec, bool shrink_active)
{
    enum lru_list lru;
    unsigned long nr_reclaimed = 0, nr_to_scan;

    for_each_evictable_lru(lru) {
	if (!shrink_active && is_active_lru(lru))
	    continue;	
	
	if (is_file_lru(lru)) {
	    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
	    nr_to_reclaim <<= 1;
	    if (nr_to_scan > nr_to_reclaim)
		nr_to_scan = nr_to_reclaim;
	} else if (htmm_mode != HTMM_NO_MIG) {
	    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES) >> priority;

	    if (nr_to_scan < nr_to_reclaim)
		nr_to_scan = nr_to_reclaim * 11 / 10; // because warm pages are not demoted
	    //else if (nr_to_scan > nr_to_reclaim << 1)
	    //	nr_to_reclaim <<= 1;
	   // else if (nr_to_scan >= 262144) // max nr_isolated is 1GB 
	///	nr_to_scan = 262144;
	}

	if (!nr_to_scan)
	    continue;

	while (nr_to_scan > 0) {
	    unsigned long scan = min(nr_to_scan, SWAP_CLUSTER_MAX);
	    nr_reclaimed += demote_inactive_list(scan, scan,
					     lruvec, lru, shrink_active);
	    nr_to_scan -= scan;
	    if (nr_reclaimed >= nr_to_reclaim)
		break;
	}

	if (nr_reclaimed >= nr_to_reclaim)
	    break;
    }

    return nr_reclaimed;
}

static unsigned long promote_lruvec(unsigned long nr_to_promote, short priority,
	pg_data_t *pgdat, struct lruvec *lruvec, enum lru_list lru)
{
    unsigned long nr_promoted = 0, nr;
    
    nr = nr_to_promote >> priority;
    if (nr)
	nr_promoted += promote_active_list(nr, lruvec, lru);

    return nr_promoted;
}

static unsigned long demote_node(pg_data_t *pgdat, struct mem_cgroup *memcg,
	unsigned long nr_exceeded)
{
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    short priority = DEF_PRIORITY;
    unsigned long nr_to_reclaim = 0, nr_evictable_pages = 0, nr_reclaimed = 0;
    enum lru_list lru;
    bool shrink_active = false;
    int target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);

    nr_to_reclaim = htmm_max_cooling_interval * 256; // mb to pages
    nr_to_reclaim *= htmm_demotion_period_in_ms;
    nr_to_reclaim /= 1000; // max num. of demotable pages in this period.

    for_each_evictable_lru(lru) {
	if (is_active_lru(lru))
	    continue;

	if (htmm_mode == HTMM_NO_MIG && !is_file_lru(lru))
	    continue;

	nr_evictable_pages += lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    }

    nr_to_reclaim = min(nr_to_reclaim, nr_evictable_pages);
    nr_to_reclaim = min(nr_to_reclaim, need_lowertier_promotion(NODE_DATA(target_nid), memcg));
    if (nr_to_reclaim < HTMM_MIN_FREE_PAGES << 1)
        nr_to_reclaim = HTMM_MIN_FREE_PAGES << 1;
    nr_to_reclaim = max(nr_to_reclaim, nr_exceeded);
    if (nr_exceeded > nr_evictable_pages)
	shrink_active = true;

    do {
	nr_reclaimed += demote_lruvec(nr_to_reclaim, priority, pgdat, lruvec, shrink_active);
	if (nr_reclaimed >= nr_to_reclaim)
	    break;
	priority--;
    } while (priority);

    if (htmm_static_thres == 0) {
	int target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);
	unsigned long nr_lowertier_active =
	    target_nid == NUMA_NO_NODE ? 0: need_lowertier_promotion(NODE_DATA(target_nid), memcg);
	
	nr_lowertier_active = nr_lowertier_active < nr_to_reclaim ?
			nr_lowertier_active : nr_to_reclaim;
	if (nr_lowertier_active && nr_reclaimed < nr_lowertier_active)
	    memcg->warm_threshold = memcg->active_threshold;
    }

    WRITE_ONCE(memcg->nodeinfo[pgdat->node_id]->need_demotion, false);
    return nr_reclaimed;
}

static unsigned long promote_node(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    unsigned long nr_to_promote, nr_promoted = 0, tmp;
    enum lru_list lru = LRU_ACTIVE_ANON;
    short priority = DEF_PRIORITY;
    int target_nid = htmm_cxl_mode ? 0 : next_promotion_node(pgdat->node_id);

    if (!promotion_available(target_nid, memcg, &nr_to_promote))
	return 0;

    nr_to_promote = min(nr_to_promote,
		    lruvec_lru_size(lruvec, lru, MAX_NR_ZONES));
    
    if (nr_to_promote == 0 && htmm_mode == HTMM_NO_MIG) {
	lru = LRU_INACTIVE_ANON;
	nr_to_promote = min(tmp, lruvec_lru_size(lruvec, lru, MAX_NR_ZONES));
    }
    do {
	nr_promoted += promote_lruvec(nr_to_promote, priority, pgdat, lruvec, lru);
	if (nr_promoted >= nr_to_promote)
	    break;
	priority--;
    } while (priority);
    
    return nr_promoted;
}

static unsigned long cooling_active_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru)
{
    unsigned long nr_taken;
    struct mem_cgroup *memcg = lruvec_memcg(lruvec);
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
	    int still_hot;

	    if (PageTransHuge(compound_head(page))) {
		struct page *meta = get_meta_page(page);
		bool need_split = false;

#ifdef DEFERRED_SPLIT_ISOLATED
		if (check_split_huge_page(memcg, get_meta_page(page), false)) {

		    spin_lock_irq(&lruvec->lru_lock);
		    if (deferred_split_huge_page_for_htmm(compound_head(page))) {
			need_split = true;
		    }
		    spin_unlock_irq(&lruvec->lru_lock);
		}
#endif
		check_transhuge_cooling((void *)memcg, page, false);
		if (need_split)
		    continue;

		if (meta->idx >= memcg->active_threshold)
		    still_hot = 2;
		else
		    still_hot = 1;
	    }
	    else {
		still_hot = cooling_page(page, lruvec_memcg(lruvec));
	    }

	    if (still_hot == 2) {
		/* page is still hot after cooling */
		if (!PageActive(page))
		    SetPageActive(page);
		list_add(&page->lru, &l_active);
		continue;
	    } 
	    else if (still_hot == 0) {
		/* not cooled page */
		if (PageActive(page))
		    list_add(&page->lru, &l_active);
		else
		    list_add(&page->lru, &l_inactive);
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
    
    return nr_taken;
}

static void cooling_node(pg_data_t *pgdat, struct mem_cgroup *memcg, bool v2)
{
    unsigned long nr_to_scan, nr_scanned = 0, nr_max_scan = 12;
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    struct mem_cgroup_per_node *pn = memcg->nodeinfo[pgdat->node_id];
    enum lru_list lru = LRU_ACTIVE_ANON; 

re_cooling:
    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    do {
	unsigned long scan = nr_to_scan >> 3; /* 12.5% */

	if (!scan)
	    scan = nr_to_scan;
	/* limits the num. of scanned pages to reduce the lock holding time */
	nr_scanned += cooling_active_list(scan, lruvec, lru);
	nr_max_scan--;
    } while (nr_scanned < nr_to_scan && nr_max_scan);

    if (v2 && is_active_lru(lru)) {
	lru = LRU_INACTIVE_ANON;
	nr_max_scan = 12;
	nr_scanned = 0;
	goto re_cooling;
    }

    /* active file list */
    cooling_active_list(lruvec_lru_size(lruvec, LRU_ACTIVE_FILE, MAX_NR_ZONES),
					lruvec, LRU_ACTIVE_FILE);
    if (nr_scanned >= nr_to_scan)
	WRITE_ONCE(pn->need_cooling, false);
}

static unsigned long adjusting_lru_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru, unsigned int *nr_huge, unsigned int *nr_base)
{
    unsigned long nr_taken;
    pg_data_t *pgdat = lruvec_pgdat(lruvec);
    struct mem_cgroup *memcg = lruvec_memcg(lruvec);
    LIST_HEAD(l_hold);
    LIST_HEAD(l_active);
    LIST_HEAD(l_inactive);
    int file = is_file_lru(lru);
    bool active = is_active_lru(lru);

    unsigned int nr_split_cand = 0, nr_split_hot = 0;

    if (file)
	return 0;

    lru_add_drain();

    spin_lock_irq(&lruvec->lru_lock);
    nr_taken = isolate_lru_pages(nr_to_scan, lruvec, lru, &l_hold, 0);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON, nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    cond_resched();
    while (!list_empty(&l_hold)) {
	struct page *page;
	int status;

	page = lru_to_page(&l_hold);
	list_del(&page->lru);

	if (unlikely(!page_evictable(page))) {
	    putback_lru_page(page);
	    continue;
	}
#ifdef DEFERRED_SPLIT_ISOLATED
	if (PageCompound(page) && check_split_huge_page(memcg, get_meta_page(page), false)) {

	    spin_lock_irq(&lruvec->lru_lock);
	    if (!deferred_split_huge_page_for_htmm(compound_head(page))) {
	        if (PageActive(page))
		    list_add(&page->lru, &l_active);
		else
		    list_add(&page->lru, &l_inactive);
	    }
	    spin_unlock_irq(&lruvec->lru_lock);
	    continue;
	}
#endif
	if (PageTransHuge(compound_head(page))) {
	    struct page *meta = get_meta_page(page);
	    
	    if (meta->idx >= memcg->active_threshold)
		status = 2;
	    else
		status = 1;
	    nr_split_hot++;
	}
	else {
	    status = page_check_hotness(page, memcg);
	    nr_split_cand++;
	}

	if (status == 2) {
	    if (active) {
		list_add(&page->lru, &l_active);
		continue;
	    }

	    SetPageActive(page);
	    list_add(&page->lru, &l_active);
	} else if (status == 0) {
	    if (PageActive(page))
		list_add(&page->lru, &l_active);
	    else
		list_add(&page->lru, &l_inactive);
	} else if (status == -1) {
	    //printk("sth wrong in the lru list.... page_check_hotness() returns -1\n");
	} else {
	    if (!active) {
		list_add(&page->lru, &l_inactive);
		continue;
	    }

	    ClearPageActive(page);
	    SetPageWorkingset(page);
	    list_add(&page->lru, &l_inactive);

	}
    }

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &l_active);
    move_pages_to_lru(lruvec, &l_inactive);
    list_splice(&l_inactive, &l_active);

    __mod_node_page_state(pgdat, NR_ISOLATED_ANON, -nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&l_active);
    free_unref_page_list(&l_active);

    *nr_huge += nr_split_hot;
    *nr_base += nr_split_cand;

    return nr_taken;
}

static void adjusting_node(pg_data_t *pgdat, struct mem_cgroup *memcg, bool active)
{
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    struct mem_cgroup_per_node *pn = memcg->nodeinfo[pgdat->node_id];
    enum lru_list lru = active ? LRU_ACTIVE_ANON : LRU_INACTIVE_ANON;
    unsigned long nr_to_scan, nr_scanned = 0, nr_max_scan =12;
    unsigned int nr_huge = 0, nr_base = 0;

    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    do {
	unsigned long scan = nr_to_scan >> 3;

	if (!scan)
	    scan = nr_to_scan;
	nr_scanned += adjusting_lru_list(scan, lruvec, lru, &nr_huge, &nr_base);
	nr_max_scan--;
    } while (nr_scanned < nr_to_scan && nr_max_scan);
    
    if (nr_scanned >= nr_to_scan)
	WRITE_ONCE(pn->need_adjusting, false);
    if (nr_scanned >= nr_to_scan && !active)
	WRITE_ONCE(pn->need_adjusting_all, false);
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
    const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);
    bool split = false;

    if (!cpumask_empty(cpumask))
	do_set_cpus_allowed(pgdat->kmigraterd, cpumask);

    for ( ; ; ) {
	struct mem_cgroup_per_node *pn;
	struct mem_cgroup *memcg;
	unsigned long nr_exceeded = 0;
	LIST_HEAD(split_list);

	if (kthread_should_stop())
	    break;

	/*if (htmm_mode == HTMM_NO_MIG) {
	    msleep_interruptible(1000);
	    continue;
	}*/

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

	/* performs split */
	if (htmm_thres_split != 0 &&
		!list_empty(&(&pn->deferred_split_queue)->split_queue)) {
	    unsigned long nr_split;
	    nr_split = deferred_split_scan_for_htmm(pn, &split_list);
	    if (!list_empty(&split_list)) {
		putback_split_pages(&split_list, mem_cgroup_lruvec(memcg, pgdat));
	    }
	    if (nr_split != 0)
		split = true;
	    //printk("nr_split by kdemotiond: %lu\n", nr_split);
	}

	/* performs cooling */
	if (need_lru_cooling(pn))
	    cooling_node(pgdat, memcg, true);
	else if (need_lru_adjusting(pn)) {
	    adjusting_node(pgdat, memcg, true);
	    if (pn->need_adjusting_all == true)
		adjusting_node(pgdat, memcg, false);
	}

	/* demotes inactive lru pages */
	if (need_toptier_demotion(pgdat, memcg, &nr_exceeded)) {
	    demote_node(pgdat, memcg, nr_exceeded);
	}

	/* default: wait 100 ms */
	msleep_interruptible(htmm_demotion_period_in_ms);
    }
    return 0;
}

static int kmigraterd_promotion(pg_data_t *pgdat)
{
    const struct cpumask *cpumask;
    bool split = false;

    if (htmm_cxl_mode)
    	cpumask = cpumask_of_node(pgdat->node_id);
    else
	cpumask = cpumask_of_node(pgdat->node_id - 2);

    if (!cpumask_empty(cpumask))
	do_set_cpus_allowed(pgdat->kmigraterd, cpumask);

    for ( ; ; ) {
	struct mem_cgroup_per_node *pn;
	struct mem_cgroup *memcg;
	LIST_HEAD(split_list);

	if (kthread_should_stop())
	    break;

	/*if (htmm_mode == HTMM_NO_MIG) {
	    msleep_interruptible(1000);
	    continue;
	}*/

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

	/* performs split */
	if (htmm_thres_split != 0 &&
		!list_empty(&(&pn->deferred_split_queue)->split_queue)) {
	    unsigned long nr_split;
	    nr_split = deferred_split_scan_for_htmm(pn, &split_list);
	    if (!list_empty(&split_list)) {
		putback_split_pages(&split_list, mem_cgroup_lruvec(memcg, pgdat));
	    }
	    if (nr_split != 0)
		split = true;
	    //printk("nr_splitted by kpromoted: %lu\n", nr_split);
	}

	if (need_lru_cooling(pn))
	    cooling_node(pgdat, memcg, true);
	else if (need_lru_adjusting(pn)) {
	    adjusting_node(pgdat, memcg, true);
	    if (pn->need_adjusting_all == true)
		adjusting_node(pgdat, memcg, false);
	}

	/* promotes hot pages to fast memory node */
	if (need_lowertier_promotion(pgdat, memcg)) {
	    promote_node(pgdat, memcg);
	}
	
	msleep_interruptible(htmm_promotion_period_in_ms);
    }

    return 0;
}

static int kmigraterd(void *p)
{
    pg_data_t *pgdat = (pg_data_t *)p;
    int nid = pgdat->node_id;

    if (htmm_cxl_mode) {
	if (nid == 0)
	    return kmigraterd_demotion(pgdat);
	else
	    return kmigraterd_promotion(pgdat);
    }

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
