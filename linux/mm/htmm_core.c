/* memtis core functions
 * author: Taehyung Lee (Sungkyunkwan Univ.)
 * mail: taehyunggg@skku.edu
 */
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/huge_mm.h>
#include <linux/mm_inline.h>
#include <linux/pid.h>
#include <linux/htmm.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/sched/task.h>
#include <linux/xarray.h>
#include <linux/math.h>
#include <linux/random.h>
#include <trace/events/htmm.h>

#include "internal.h"
#include <asm/pgtable.h>

void htmm_mm_init(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

    if (!memcg || !memcg->htmm_enabled) {
	mm->htmm_enabled = false;
	return;
    }
    mm->htmm_enabled = true;
}

void htmm_mm_exit(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    if (!memcg)
	return;
    /* do nothing */
}

/* Hugepage uses tail pages to store access information.
 * See struct page declaration in linux/mm_types.h */
void __prep_transhuge_page_for_htmm(struct mm_struct *mm, struct page *page)
{
    int i, idx, offset;
    struct mem_cgroup *memcg = mm ? get_mem_cgroup_from_mm(mm) : NULL;
    pginfo_t pginfo = { 0, 0, 0, false, };
    int hotness_factor = memcg ? get_accesses_from_idx(memcg->active_threshold + 1) : 0;
    /* third tail page */
    page[3].hot_utils = 0;
    page[3].total_accesses = hotness_factor;
    page[3].skewness_idx = 0;
    page[3].idx = 0;
    SetPageHtmm(&page[3]);

    if (hotness_factor < 0)
	hotness_factor = 0;
    pginfo.total_accesses = hotness_factor;
    pginfo.nr_accesses = hotness_factor;
    /* fourth~ tail pages */
    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 4;
	offset = i % 4;
	
	page[idx].compound_pginfo[offset] = pginfo;
	SetPageHtmm(&page[idx]);
    }

    if (!memcg)
	return;

    if (htmm_skip_cooling)
	page[3].cooling_clock = memcg->cooling_clock + 1;
    else
	page[3].cooling_clock = memcg->cooling_clock;

    ClearPageActive(page);
}

void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
				  struct page *page)
{
    prep_transhuge_page(page);

    if (vma->vm_mm->htmm_enabled)
	__prep_transhuge_page_for_htmm(vma->vm_mm, page);
    else
	return;
}

void clear_transhuge_pginfo(struct page *page)
{
    INIT_LIST_HEAD(&page->lru);
    set_page_private(page, 0);
}

void copy_transhuge_pginfo(struct page *page,
			   struct page *newpage)
{
    int i, idx, offset;
    pginfo_t zero_pginfo = { 0 };

    VM_BUG_ON_PAGE(!PageCompound(page), page);
    VM_BUG_ON_PAGE(!PageCompound(newpage), newpage);

    page = compound_head(page);
    newpage = compound_head(newpage);

    if (!PageHtmm(&page[3]))
	return;

    newpage[3].hot_utils = page[3].hot_utils;
    newpage[3].total_accesses = page[3].total_accesses;
    newpage[3].skewness_idx = page[3].skewness_idx;
    newpage[3].cooling_clock = page[3].cooling_clock;
    newpage[3].idx = page[3].idx;

    SetPageHtmm(&newpage[3]);

    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 4;
	offset = i % 4;

	newpage[idx].compound_pginfo[offset].nr_accesses =
			page[idx].compound_pginfo[offset].nr_accesses;
	newpage[idx].compound_pginfo[offset].total_accesses =
			page[idx].compound_pginfo[offset].total_accesses;
	
	page[idx].compound_pginfo[offset] = zero_pginfo;
	page[idx].mapping = TAIL_MAPPING;
	SetPageHtmm(&newpage[idx]);
    }
}

pginfo_t *get_compound_pginfo(struct page *page, unsigned long address)
{
    int idx, offset;
    VM_BUG_ON_PAGE(!PageCompound(page), page);
    
    idx = 4 + ((address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT) / 4;
    offset = ((address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT) % 4;

    return &(page[idx].compound_pginfo[offset]);
}

void check_transhuge_cooling(void *arg, struct page *page, bool locked)
{
    struct mem_cgroup *memcg = arg ? (struct mem_cgroup *)arg : page_memcg(page);
    struct page *meta_page;
    pginfo_t *pginfo;
    int i, idx, offset;
    unsigned int memcg_cclock;

    if (!memcg || !memcg->htmm_enabled)
	return;

    meta_page = get_meta_page(page);

    spin_lock(&memcg->access_lock);
    /* check cooling */
    memcg_cclock = READ_ONCE(memcg->cooling_clock);
    if (memcg_cclock > meta_page->cooling_clock) {
	    unsigned int diff = memcg_cclock - meta_page->cooling_clock;
	    unsigned long prev_idx, cur_idx, skewness = 0;
	    unsigned int refs = 0;
	    unsigned int bp_hot_thres = min(memcg->active_threshold,
					 memcg->bp_active_threshold);

	    /* perform cooling */
	    meta_page->hot_utils = 0;
	    for (i = 0; i < HPAGE_PMD_NR; i++) { // subpages
		int j;

		idx = 4 + i / 4;
		offset = i % 4;
		pginfo =&(page[idx].compound_pginfo[offset]);
		prev_idx = get_idx(pginfo->total_accesses);
		if (prev_idx >= bp_hot_thres) {
		    meta_page->hot_utils++;
		    refs += pginfo->total_accesses;
		}

		/* get the sum of the square of H_ij*/
		skewness += (pginfo->total_accesses * pginfo->total_accesses);
		if (prev_idx >= (memcg->bp_active_threshold))
		    pginfo->may_hot = true;
		else
		    pginfo->may_hot = false;

		/* halves access counts of subpages */
		for (j = 0; j < diff; j++)
		    pginfo->total_accesses >>= 1;

		/* updates estimated base page histogram */
		cur_idx = get_idx(pginfo->total_accesses);
		memcg->ebp_hotness_hg[cur_idx]++;
	    }

	    /* halves access count for a huge page */
	    for (i = 0; i < diff; i++)		
		meta_page->total_accesses >>= 1;

	    cur_idx = meta_page->total_accesses;
	    cur_idx = get_idx(cur_idx);
	    memcg->hotness_hg[cur_idx] += HPAGE_PMD_NR;
	    meta_page->idx = cur_idx;

	    /* updates skewness */
	    if (meta_page->hot_utils == 0)
		skewness = 0;
	    else if (meta_page->idx >= 13) // very hot pages 
		skewness = 0;
	    else {
		skewness /= 11; /* scale down */
		skewness = skewness / (meta_page->hot_utils);
		skewness = skewness / (meta_page->hot_utils);
		skewness = get_skew_idx(skewness);
	    }
	    meta_page->skewness_idx = skewness;
	    memcg->access_map[skewness] += 1;

	    if (meta_page->hot_utils) {
		refs /= HPAGE_PMD_NR; /* actual access counts */
		memcg->sum_util += refs; /* total accesses to huge pages */
		memcg->num_util += 1; /* the number of huge pages */
	    }

	    meta_page->cooling_clock = memcg_cclock;
    } else
	meta_page->cooling_clock = memcg_cclock;

    spin_unlock(&memcg->access_lock);
}

void check_base_cooling(pginfo_t *pginfo, struct page *page, bool locked)
{
    struct mem_cgroup *memcg = page_memcg(page);
    unsigned long prev_accessed, cur_idx;
    unsigned int memcg_cclock;

    if (!memcg || !memcg->htmm_enabled)
	return;

    spin_lock(&memcg->access_lock);
    memcg_cclock = READ_ONCE(memcg->cooling_clock);
    if (memcg_cclock > pginfo->cooling_clock) {
	unsigned int diff = memcg_cclock - pginfo->cooling_clock;    
	int j;
	    
	prev_accessed = pginfo->total_accesses;
	cur_idx = get_idx(prev_accessed);
	if (cur_idx >= (memcg->bp_active_threshold))
	    pginfo->may_hot = true;
	else
	    pginfo->may_hot = false;

	/* halves access count */
	for (j = 0; j < diff; j++)
	    pginfo->total_accesses >>= 1;
	//if (pginfo->total_accesses == 0)
	  //  pginfo->total_accesses = 1;

	cur_idx = get_idx(pginfo->total_accesses);
	memcg->hotness_hg[cur_idx]++;
	memcg->ebp_hotness_hg[cur_idx]++;

	pginfo->cooling_clock = memcg_cclock;
    } else
	pginfo->cooling_clock = memcg_cclock;
    spin_unlock(&memcg->access_lock);
}

int set_page_coolstatus(struct page *page, pte_t *pte, struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct page *pte_page;
    pginfo_t *pginfo;
    int hotness_factor;

    if (!memcg || !memcg->htmm_enabled)
	return 0;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	return 0;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	return 0;
    
    hotness_factor = get_accesses_from_idx(memcg->active_threshold + 1);
    
    pginfo->total_accesses = hotness_factor;
    pginfo->nr_accesses = hotness_factor;
    if (htmm_skip_cooling)
	pginfo->cooling_clock = READ_ONCE(memcg->cooling_clock) + 1;
    else
	pginfo->cooling_clock = READ_ONCE(memcg->cooling_clock);
    pginfo->may_hot = false;

    return 0;
}

struct deferred_split *get_deferred_split_queue_for_htmm(struct page *page)
{
    struct mem_cgroup *memcg = page_memcg(compound_head(page));
    struct mem_cgroup_per_node *pn = memcg->nodeinfo[page_to_nid(page)];

    if (!memcg || !memcg->htmm_enabled)
	return NULL;
    else
	return &pn->deferred_split_queue;
}

struct list_head *get_deferred_list(struct page *page)
{
    struct mem_cgroup *memcg = page_memcg(compound_head(page));
    struct mem_cgroup_per_node *pn = memcg->nodeinfo[page_to_nid(page)];

    if (!memcg || !memcg->htmm_enabled)
	return NULL;
    else
	return &pn->deferred_list; 
}

bool deferred_split_huge_page_for_htmm(struct page *page)
{
    struct deferred_split *ds_queue = get_deferred_split_queue(page);
    unsigned long flags;

    VM_BUG_ON_PAGE(!PageTransHuge(page), page);

    if (PageSwapCache(page))
	return false;

    if (!ds_queue)
	return false;
    
    spin_lock_irqsave(&ds_queue->split_queue_lock, flags);
    if (list_empty(page_deferred_list(page))) {
	count_vm_event(THP_DEFERRED_SPLIT_PAGE);
	list_add_tail(page_deferred_list(page), &ds_queue->split_queue);
	ds_queue->split_queue_len++;

	if (node_is_toptier(page_to_nid(page)))
	    count_vm_event(HTMM_MISSED_DRAMREAD);
	else
	    count_vm_event(HTMM_MISSED_NVMREAD);
    }
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);
    return true;
}

void check_failed_list(struct mem_cgroup_per_node *pn,
	struct list_head *tmp, struct list_head *failed_list)
{
    struct mem_cgroup *memcg = pn->memcg;

    while (!list_empty(tmp)) {
	struct page *page = lru_to_page(tmp);
	struct page *meta;
	unsigned int idx;

	list_move(&page->lru, failed_list);
	
	if (!PageTransHuge(page))
	    VM_WARN_ON(1);

	if (PageLRU(page)) {
	    if (!TestClearPageLRU(page)) {
		VM_WARN_ON(1);
	    }
	}

	meta = get_meta_page(page);
	idx = meta->idx;

	spin_lock(&memcg->access_lock);
	memcg->hotness_hg[idx] += HPAGE_PMD_NR;
	spin_unlock(&memcg->access_lock);
    }
}

unsigned long deferred_split_scan_for_htmm(struct mem_cgroup_per_node *pn,
	struct list_head *split_list)
{
    struct deferred_split *ds_queue = &pn->deferred_split_queue;
    //struct list_head *deferred_list = &pn->deferred_list;
    unsigned long flags;
    LIST_HEAD(list), *pos, *next;
    LIST_HEAD(failed_list);
    struct page *page;
    unsigned int nr_max = 50; // max: 100MB
    int split = 0;

    spin_lock_irqsave(&ds_queue->split_queue_lock, flags);
    list_for_each_safe(pos, next, &ds_queue->split_queue) {
	page = list_entry((void *)pos, struct page, deferred_list);
	page = compound_head(page);
    
	if (page_count(page) < 1) {
	    list_del_init(page_deferred_list(page));
	    ds_queue->split_queue_len--;
	}
	else { 
	    list_move(page_deferred_list(page), &list);
	}
    }
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);

    list_for_each_safe(pos, next, &list) {
	LIST_HEAD(tmp);
	struct lruvec *lruvec = mem_cgroup_page_lruvec(page);
	bool skip_iso = false;

	if (split >= nr_max)
	    break;

	page = list_entry((void *)pos, struct page, deferred_list);
	page = compound_head(page);

	if (!PageLRU(page)) {
	    skip_iso = true;
	    goto skip_isolation;
	}

	if (lruvec != &pn->lruvec) {
	    continue;
	}

	spin_lock_irq(&lruvec->lru_lock);
	if (!__isolate_lru_page_prepare(page, 0)) {
	    spin_unlock_irq(&lruvec->lru_lock);
	    continue;
	}

	if (unlikely(!get_page_unless_zero(page))) {
	    spin_unlock_irq(&lruvec->lru_lock);
	    continue;
	}

	if (!TestClearPageLRU(page)) {
	    put_page(page);
	    spin_unlock_irq(&lruvec->lru_lock); 
	    continue;
	}
    
	list_move(&page->lru, &tmp);
	update_lru_size(lruvec, page_lru(page), page_zonenum(page),
		    -thp_nr_pages(page));
	spin_unlock_irq(&lruvec->lru_lock);
skip_isolation:
	if (skip_iso) {
	    if (page->lru.next != LIST_POISON1 || page->lru.prev != LIST_POISON2)
		continue;
	    list_add(&page->lru, &tmp);
	}
	
	if (!trylock_page(page)) {
	    list_splice_tail(&tmp, split_list);
	    continue;
	}

	if (!split_huge_page_to_list(page, &tmp)) {
	    split++;
	    list_splice(&tmp, split_list);
	} else {
	    check_failed_list(pn, &tmp, &failed_list);
	}

	unlock_page(page);
    }
    putback_movable_pages(&failed_list);

    /* handle list and failed_list */
    spin_lock_irqsave(&ds_queue->split_queue_lock, flags); 
    list_splice_tail(&list, &ds_queue->split_queue);
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);
    
    putback_movable_pages(&failed_list); 
    if (split)
	pn->memcg->split_happen = true;
    return split;
}

void putback_split_pages(struct list_head *split_list, struct lruvec *lruvec)
{
    LIST_HEAD(l_active);
    LIST_HEAD(l_inactive);

    while (!list_empty(split_list)) {
	struct page *page;

	page = lru_to_page(split_list);
	list_del(&page->lru);

	if (unlikely(!page_evictable(page))) {
	    putback_lru_page(page);
	    continue;
	}

	VM_WARN_ON(PageLRU(page));

	if (PageActive(page))
	    list_add(&page->lru, &l_active);
	else
	    list_add(&page->lru, &l_inactive);
    }

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &l_active);
    move_pages_to_lru(lruvec, &l_inactive);
    list_splice(&l_inactive, &l_active);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&l_active);
    free_unref_page_list(&l_active);
}

struct page *get_meta_page(struct page *page)
{
    page = compound_head(page);
    return &page[3];
}

unsigned int get_accesses_from_idx(unsigned int idx)
{
    unsigned int accesses = 1;
    
    if (idx == 0)
	return 0;

    while (idx--) {
	accesses <<= 1;
    }

    return accesses;
}

unsigned int get_idx(unsigned long num)
{
    unsigned int cnt = 0;
   
    num++;
    while (1) {
	num = num >> 1;
	if (num)
	    cnt++;
	else	
	    return cnt;
	
	if (cnt == 15)
	    break;
    }

    return cnt;
}

int get_skew_idx(unsigned long num)
{
    int cnt = 0;
    unsigned long tmp;
    
    /* 0, 1-3, 4-15, 16-63, 64-255, 256-1023, 1024-2047, 2048-3071, ... */
    tmp = num;
    if (tmp >= 1024) {
	while (tmp > 1024 && cnt < 9) { // <16
	    tmp -= 1024;
	    cnt++;
	}
	cnt += 11;
    }
    else {
	while (tmp) {
	    tmp >>= 1; // >>2
	    cnt++;
	}
    }

    return cnt;
}

/* linux/mm.h */
void free_pginfo_pte(struct page *pte)
{
    if (!PageHtmm(pte))
	return;

    BUG_ON(pte->pginfo == NULL);
    kmem_cache_free(pginfo_cache, pte->pginfo);
    pte->pginfo = NULL;
    ClearPageHtmm(pte);
}

void uncharge_htmm_pte(pte_t *pte, struct mem_cgroup *memcg)
{
    struct page *pte_page;
    unsigned int idx;
    pginfo_t *pginfo;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	return;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	return;

    idx = get_idx(pginfo->total_accesses);
    spin_lock(&memcg->access_lock);
    if (memcg->hotness_hg[idx] > 0)
	memcg->hotness_hg[idx]--;
    if (memcg->ebp_hotness_hg[idx] > 0)
	memcg->ebp_hotness_hg[idx]--;
    spin_unlock(&memcg->access_lock);
}

void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg)
{
    unsigned int nr_pages = thp_nr_pages(page);
    unsigned int idx;
    int i;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    page = compound_head(page);
    if (nr_pages != 1) { // hugepage
	struct page *meta = get_meta_page(page);

	idx = meta->idx;

	spin_lock(&memcg->access_lock);
	if (memcg->hotness_hg[idx] >= nr_pages)
	    memcg->hotness_hg[idx] -= nr_pages;
	else
	    memcg->hotness_hg[idx] = 0;
	
	for (i = 0; i < HPAGE_PMD_NR; i++) {
	    int base_idx = 4 + i / 4;
	    int offset = i % 4;
	    pginfo_t *pginfo;

	    pginfo = &(page[base_idx].compound_pginfo[offset]);
	    idx = get_idx(pginfo->total_accesses);
	    if (memcg->ebp_hotness_hg[idx] > 0)
		memcg->ebp_hotness_hg[idx]--;
	}
	spin_unlock(&memcg->access_lock);
    }
}

static bool need_cooling(struct mem_cgroup *memcg)
{
    struct mem_cgroup_per_node *pn;
    int nid;

    for_each_node_state(nid, N_MEMORY) {
	pn = memcg->nodeinfo[nid];
	if (!pn)
	    continue;
    
	if (READ_ONCE(pn->need_cooling))
	    return true;
    }
    return false;
}

static void set_lru_cooling(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct mem_cgroup_per_node *pn;
    int nid;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    for_each_node_state(nid, N_MEMORY) {
	pn = memcg->nodeinfo[nid];
	if (!pn)
	    continue;
    
	WRITE_ONCE(pn->need_cooling, true);
    }
}

void set_lru_adjusting(struct mem_cgroup *memcg, bool inc_thres)
{
    struct mem_cgroup_per_node *pn;
    int nid;

    for_each_node_state(nid, N_MEMORY) {
    
	pn = memcg->nodeinfo[nid];
	if (!pn)
	    continue;

	WRITE_ONCE(pn->need_adjusting, true);
	if (inc_thres)
	    WRITE_ONCE(pn->need_adjusting_all, true);
    }
}

bool check_split_huge_page(struct mem_cgroup *memcg,
	struct page *meta, bool hot)
{
    unsigned long split_thres = memcg->split_threshold;
    unsigned long split_thres_tail = split_thres - 1;
    bool tail_idx = false;
   
    /* check split enable/disable status */
    if (htmm_thres_split == 0)
	return false;

    /* no need to split */
    if (split_thres == 0)
	return false;
    
    /* already in the split queue */
    if (!list_empty(page_deferred_list(compound_head(meta)))) {
	return false;
    }

    /* check split thres */
    if (meta->skewness_idx < split_thres_tail)
	return false;
    else if (meta->skewness_idx == split_thres_tail)
	tail_idx = true;
    if (memcg->nr_split == 0)
	tail_idx = true;

    if (tail_idx && memcg->nr_split_tail_idx == 0)
	return false;
    
    spin_lock(&memcg->access_lock);
    if (tail_idx) {
	if (memcg->nr_split_tail_idx >= HPAGE_PMD_NR)
	    memcg->nr_split_tail_idx -= HPAGE_PMD_NR;
	else
	    memcg->nr_split_tail_idx = 0;
    } else {
	if (memcg->nr_split >= HPAGE_PMD_NR)
	    memcg->nr_split -= HPAGE_PMD_NR;
	else
	    memcg->nr_split = 0;
    }
    if (memcg->access_map[meta->skewness_idx] != 0)
	memcg->access_map[meta->skewness_idx]--;
    spin_unlock(&memcg->access_lock);
    return true;
}

bool move_page_to_deferred_split_queue(struct mem_cgroup *memcg, struct page *page)
{
    struct lruvec *lruvec;
    bool ret = false;

    page = compound_head(page);

    lruvec = mem_cgroup_page_lruvec(page);
    spin_lock_irq(&lruvec->lru_lock);
    
    if (!PageLRU(page))
	goto lru_unlock;

    if (deferred_split_huge_page_for_htmm(compound_head(page))) {
	ret = true;
	goto lru_unlock;
    }
    
lru_unlock:
    spin_unlock_irq(&lruvec->lru_lock);

    return ret;
}

void move_page_to_active_lru(struct page *page)
{
    struct lruvec *lruvec;
    LIST_HEAD(l_active);

    lruvec = mem_cgroup_page_lruvec(page);
    
    spin_lock_irq(&lruvec->lru_lock);
    if (PageActive(page))
	goto lru_unlock;

    if (!__isolate_lru_page_prepare(page, 0))
	goto lru_unlock;

    if (unlikely(!get_page_unless_zero(page)))
	goto lru_unlock;

    if (!TestClearPageLRU(page)) {
	put_page(page);
	goto lru_unlock;
    }
    
    list_move(&page->lru, &l_active);
    update_lru_size(lruvec, page_lru(page), page_zonenum(page),
		    -thp_nr_pages(page));
    SetPageActive(page);

    if (!list_empty(&l_active))
	move_pages_to_lru(lruvec, &l_active);
lru_unlock:
    spin_unlock_irq(&lruvec->lru_lock);

    if (!list_empty(&l_active))
	BUG();
}

void move_page_to_inactive_lru(struct page *page)
{
    struct lruvec *lruvec;
    LIST_HEAD(l_inactive);

    lruvec = mem_cgroup_page_lruvec(page);
    
    spin_lock_irq(&lruvec->lru_lock);
    if (!PageActive(page))
	goto lru_unlock;

    if (!__isolate_lru_page_prepare(page, 0))
	goto lru_unlock;

    if (unlikely(!get_page_unless_zero(page)))
	goto lru_unlock;

    if (!TestClearPageLRU(page)) {
	put_page(page);
	goto lru_unlock;
    }
    
    list_move(&page->lru, &l_inactive);
    update_lru_size(lruvec, page_lru(page), page_zonenum(page),
		    -thp_nr_pages(page));
    ClearPageActive(page);

    if (!list_empty(&l_inactive))
	move_pages_to_lru(lruvec, &l_inactive);
lru_unlock:
    spin_unlock_irq(&lruvec->lru_lock);

    if (!list_empty(&l_inactive))
	BUG();
}

static void update_base_page(struct vm_area_struct *vma,
	struct page *page, pginfo_t *pginfo)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);
    unsigned long prev_accessed, prev_idx, cur_idx;
    bool hot;

    /* check cooling status and perform cooling if the page needs to be cooled */
    check_base_cooling(pginfo, page, false);

    prev_accessed = pginfo->total_accesses;
    pginfo->nr_accesses++;
    pginfo->total_accesses += HPAGE_PMD_NR;
    
    prev_idx = get_idx(prev_accessed);
    cur_idx = get_idx(pginfo->total_accesses);

    spin_lock(&memcg->access_lock);

    if (prev_idx != cur_idx) {
	if (memcg->hotness_hg[prev_idx] > 0)
	    memcg->hotness_hg[prev_idx]--;
	memcg->hotness_hg[cur_idx]++;

	if (memcg->ebp_hotness_hg[prev_idx] > 0)
	    memcg->ebp_hotness_hg[prev_idx]--;
	memcg->ebp_hotness_hg[cur_idx]++;
    }

    if (pginfo->may_hot == true)
	memcg->max_dram_sampled++;
    if (cur_idx >= (memcg->bp_active_threshold))
	pginfo->may_hot = true;
    else
	pginfo->may_hot = false;

    spin_unlock(&memcg->access_lock);

    hot = cur_idx >= memcg->active_threshold;
    
    if (PageActive(page) && !hot)
	move_page_to_inactive_lru(page);
    else if (!PageActive(page) && hot)
	move_page_to_active_lru(page);
    
    if (hot)
	move_page_to_active_lru(page);
    else if (PageActive(page))
	move_page_to_inactive_lru(page);
}

static void update_huge_page(struct vm_area_struct *vma, pmd_t *pmd,
	struct page *page, unsigned long address)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);
    struct page *meta_page;
    pginfo_t *pginfo;
    unsigned long prev_idx, cur_idx;
    bool hot, pg_split = false;
    unsigned long pginfo_prev;

    meta_page = get_meta_page(page);
    pginfo = get_compound_pginfo(page, address);

    /* check cooling status */
    check_transhuge_cooling((void *)memcg, page, false);

    pginfo_prev = pginfo->total_accesses;
    pginfo->nr_accesses++;
    pginfo->total_accesses += HPAGE_PMD_NR;
    
    meta_page->total_accesses++;

#ifndef DEFERRED_SPLIT_ISOLATED
    if (check_split_huge_page(memcg, meta_page, false)) {
	pg_split = move_page_to_deferred_split_queue(memcg, page);
    }
#endif

    /*subpage */
    prev_idx = get_idx(pginfo_prev);
    cur_idx = get_idx(pginfo->total_accesses);
    spin_lock(&memcg->access_lock);
    if (prev_idx != cur_idx) {
	if (memcg->ebp_hotness_hg[prev_idx] > 0)
	    memcg->ebp_hotness_hg[prev_idx]--;
	memcg->ebp_hotness_hg[cur_idx]++;
    }
    if (pginfo->may_hot == true)
	memcg->max_dram_sampled++;
    if (cur_idx >= (memcg->bp_active_threshold))
	pginfo->may_hot = true;
    else
	pginfo->may_hot = false;
    spin_unlock(&memcg->access_lock);

    /* hugepage */
    prev_idx = meta_page->idx;
    cur_idx = meta_page->total_accesses;
    cur_idx = get_idx(cur_idx);
    if (prev_idx != cur_idx) {
	spin_lock(&memcg->access_lock);
	if (memcg->hotness_hg[prev_idx] >= HPAGE_PMD_NR)
	    memcg->hotness_hg[prev_idx] -= HPAGE_PMD_NR;
	else
	    memcg->hotness_hg[prev_idx] = 0;

	memcg->hotness_hg[cur_idx] += HPAGE_PMD_NR;
	spin_unlock(&memcg->access_lock);
    }
    meta_page->idx = cur_idx;

    if (pg_split)
	return;

    hot = cur_idx >= memcg->active_threshold;
    if (PageActive(page) && !hot) {
	move_page_to_inactive_lru(page);
    } else if (!PageActive(page) && hot) {
	move_page_to_active_lru(page);
    }
    
    if (hot)
	move_page_to_active_lru(page);
    else if (PageActive(page))
	move_page_to_inactive_lru(page);
}

static int __update_pte_pginfo(struct vm_area_struct *vma, pmd_t *pmd,
				unsigned long address)
{
    pte_t *pte, ptent;
    spinlock_t *ptl;
    pginfo_t *pginfo;
    struct page *page, *pte_page;
    int ret = 0;

    pte = pte_offset_map_lock(vma->vm_mm, pmd, address, &ptl);
    ptent = *pte;
    if (!pte_present(ptent))
	goto pte_unlock;

    page = vm_normal_page(vma, address, ptent);
    if (!page || PageKsm(page))
	goto pte_unlock;

    if (page != compound_head(page))
	goto pte_unlock;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	goto pte_unlock;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	goto pte_unlock;

    update_base_page(vma, page, pginfo);
    pte_unmap_unlock(pte, ptl);
    if (htmm_cxl_mode) {
	if (page_to_nid(page) == 0)
	    return 1;
	else
	    return 2;
    }
    else {
	if (node_is_toptier(page_to_nid(page)))
	    return 1;
	else
	    return 2;
    }

pte_unlock:
    pte_unmap_unlock(pte, ptl);
    return ret;
}

static int __update_pmd_pginfo(struct vm_area_struct *vma, pud_t *pud,
				unsigned long address)
{
    pmd_t *pmd, pmdval;
    bool ret = 0;

    pmd = pmd_offset(pud, address);
    if (!pmd || pmd_none(*pmd))
	return ret;
    
    if (is_swap_pmd(*pmd))
	return ret;

    if (!pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))) {
	pmd_clear_bad(pmd);
	return ret;
    }

    pmdval = *pmd;
    if (pmd_trans_huge(pmdval) || pmd_devmap(pmdval)) {
	struct page *page;

	if (is_huge_zero_pmd(pmdval))
	    return ret;
	
	page = pmd_page(pmdval);
	if (!page)
	    goto pmd_unlock;
	
	if (!PageCompound(page)) {
	    goto pmd_unlock;
	}

	update_huge_page(vma, pmd, page, address);
	if (htmm_cxl_mode) {
	    if (page_to_nid(page) == 0)
		return 1;
	    else
		return 2;
	}
	else {
	    if (node_is_toptier(page_to_nid(page)))
		return 1;
	    else
		return 2;
	}
pmd_unlock:
	return 0;
    }

    /* base page */
    return __update_pte_pginfo(vma, pmd, address);
}

static int __update_pginfo(struct vm_area_struct *vma, unsigned long address)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;

    pgd = pgd_offset(vma->vm_mm, address);
    if (pgd_none_or_clear_bad(pgd))
	return 0;
    
    p4d = p4d_offset(pgd, address);
    if (p4d_none_or_clear_bad(p4d))
	return 0;
    
    pud = pud_offset(p4d, address);
    if (pud_none_or_clear_bad(pud))
	return 0;
    
    return __update_pmd_pginfo(vma, pud, address);
}

static void set_memcg_split_thres(struct mem_cgroup *memcg)
{
    long nr_split = memcg->nr_split;
    int i;

    if (memcg->nr_split == 0) { // no split
	memcg->split_threshold = 21;
	return;
    }

    spin_lock(&memcg->access_lock);
    for (i = 20; i > 0; i--) {
	long nr_pages = memcg->access_map[i] * HPAGE_PMD_NR;

	if (nr_split < nr_pages) {
	    memcg->nr_split_tail_idx = nr_split;
	    memcg->nr_split -= nr_split;
	    break;
	}
	else
	    nr_split -= nr_pages;
    }
    
    if (i != 20)
	memcg->split_threshold = i + 1;
    spin_unlock(&memcg->access_lock);
}

static void set_memcg_nr_split(struct mem_cgroup *memcg)
{
    unsigned long ehr, rhr;
    unsigned long captier_lat = htmm_cxl_mode ?
		    CXL_ACCESS_LATENCY : NVM_ACCESS_LATENCY;
    unsigned long nr_records;
    unsigned int avg_accesses_hp;

    memcg->nr_split = 0;
    memcg->nr_split_tail_idx = 0;
    
    ehr = memcg->prev_max_dram_sampled * 95 / 100;
    rhr = memcg->prev_dram_sampled;
    if (ehr <= rhr)
	return;
    if (memcg->num_util == 0)
	return;
    
    /* cooling halves the access counts so that
     * NR_SAMPLE(n) = cooling_period + NR_SAMPLE(n-1) / 2
     * --> n = htmm_cooling_period * 2 - (htmm_cooling_period >> cooling_counts)
     */
    avg_accesses_hp = memcg->sum_util / memcg->num_util;
    if (avg_accesses_hp == 0)
	return;

    nr_records = (htmm_cooling_period << 1) -
	    (htmm_cooling_period >> (memcg->cooling_clock - 1));

    /* N = (eHR - rHR) * (nr_samples / avg_accesses_hp) * (delta lat / fast lat);
     * >> (eHR - rHR) == ('ehr' - 'rhr') / 'nr_records';
     * >> 'ehr' - 'rhr' == (eHR - rHR) * nr_records
     * To reflect actual accesses to huge pages, calibrates nr_records to
     * memcg->sum_util;
     */
    memcg->nr_split = (ehr - rhr) * memcg->sum_util / nr_records;
    memcg->nr_split /= avg_accesses_hp;
    /* reflects latency gap */
    memcg->nr_split *= (captier_lat - DRAM_ACCESS_LATENCY);
    memcg->nr_split /= DRAM_ACCESS_LATENCY;
    /* multiply hugepage size (counting granularity) */
    memcg->nr_split *= HPAGE_PMD_NR;
    /* scale down */
    memcg->nr_split *= htmm_gamma;
    memcg->nr_split /= 10;
}

/* protected by memcg->access_lock */
static void reset_memcg_stat(struct mem_cgroup *memcg)
{
    int i;

    for (i = 0; i < 16; i++) {
	memcg->hotness_hg[i] = 0;
	memcg->ebp_hotness_hg[i] = 0;
    }

    for (i = 0; i < 21; i++)
	memcg->access_map[i] = 0;

    memcg->sum_util = 0;
    memcg->num_util = 0;
}

static bool __cooling(struct mm_struct *mm,
	struct mem_cgroup *memcg)
{
    int nid;

    /* check whether the previous cooling is done or not. */
    for_each_node_state(nid, N_MEMORY) {
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];
	if (pn && READ_ONCE(pn->need_cooling)) {
	    spin_lock(&memcg->access_lock);
	    memcg->cooling_clock++;
	    spin_unlock(&memcg->access_lock);
	    return false;
	}
    }

    spin_lock(&memcg->access_lock);

    reset_memcg_stat(memcg); 
    memcg->cooling_clock++;
    memcg->bp_active_threshold--;
    memcg->cooled = true;
    smp_mb();
    spin_unlock(&memcg->access_lock);
    set_lru_cooling(mm);
    return true;
}

static void __adjust_active_threshold(struct mm_struct *mm,
	struct mem_cgroup *memcg)
{
    unsigned long nr_active = 0;
    unsigned long max_nr_pages = memcg->max_nr_dram_pages -
	    get_memcg_promotion_watermark(memcg->max_nr_dram_pages);
    bool need_warm = false;
    int idx_hot, idx_bp;

    //if (need_cooling(memcg))
//	return;

    spin_lock(&memcg->access_lock);

    for (idx_hot = 15; idx_hot >= 0; idx_hot--) {
	unsigned long nr_pages = memcg->hotness_hg[idx_hot];
	if (nr_active + nr_pages > max_nr_pages)
	    break;
	nr_active += nr_pages;
    }
    if (idx_hot != 15)
	idx_hot++;

    if (nr_active < (max_nr_pages * 75 / 100))
	need_warm = true;

    /* for the estimated base page histogram */
    nr_active = 0;
    for (idx_bp = 15; idx_bp >= 0; idx_bp--) {
	unsigned long nr_pages = memcg->ebp_hotness_hg[idx_bp];
	if (nr_active + nr_pages > max_nr_pages)
	    break;
	nr_active += nr_pages;
    }
    if (idx_bp != 15)
	idx_bp++;

    spin_unlock(&memcg->access_lock);

    // minimum hot threshold
    if (idx_hot < htmm_thres_hot)
	idx_hot = htmm_thres_hot;
    if (idx_bp < htmm_thres_hot)
	idx_bp = htmm_thres_hot;

    /* some pages may not be reflected in the histogram when cooling happens */
    if (memcg->cooled) {
	/* when cooling happens, thres will be current - 1 */
	if (idx_hot < memcg->active_threshold)
	    if (memcg->active_threshold > 1)
		memcg->active_threshold--;
	if (idx_bp < memcg->bp_active_threshold)
	    memcg->bp_active_threshold = idx_bp;
	
	memcg->cooled = false;
	set_lru_adjusting(memcg, true);

	if (memcg->need_split) {
	    /* set the target number of pages to be split */
	    set_memcg_nr_split(memcg);
	    /* set the split factor thres */
	    set_memcg_split_thres(memcg);
	    /* reset stat for split */
	    memcg->nr_sampled_for_split = 0;
	    memcg->need_split = false;
	    //trace_printk("memcg->nr_split: %lu, memcg->split_thres: %lu\n", memcg->nr_split, memcg->split_threshold);
	}
    }
    else { /* normal case */
	if (idx_hot > memcg->active_threshold) {
	    //printk("thres: %d -> %d\n", memcg->active_threshold, idx_hot);
	    memcg->active_threshold = idx_hot;
	    set_lru_adjusting(memcg, true);
	}
	else if (memcg->split_happen && htmm_thres_split &&
		idx_hot < memcg->active_threshold) {
	    /* if split happens, histogram may be changed.
	     * Thus, hot-thres could be decreased */
	    memcg->active_threshold = idx_hot;
	    set_lru_adjusting(memcg, true);
	    //memcg->split_happen = false;
	}
	/* estimated base page histogram */
	memcg->bp_active_threshold = idx_bp;
    }

    /* set warm threshold */
    if (!htmm_nowarm) { // warm enabled
	if (need_warm)
	    memcg->warm_threshold = memcg->active_threshold - 1;
	else
	    memcg->warm_threshold = memcg->active_threshold;
    } else { // disable warm
	memcg->warm_threshold = memcg->active_threshold;
    }
}

static bool need_memcg_cooling (struct mem_cgroup *memcg)
{
    unsigned long usage = page_counter_read(&memcg->memory);
    if (memcg->nr_alloc + htmm_thres_cooling_alloc <= usage) {
	memcg->nr_alloc = usage;
	return true;	
    }
    return false;
}

void update_pginfo(pid_t pid, unsigned long address, enum events e)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_struct ? pid_task(pid_struct, PIDTYPE_PID) : NULL;
    struct mm_struct *mm = p ? p->mm : NULL;
    struct vm_area_struct *vma; 
    struct mem_cgroup *memcg;
    int ret;
    static unsigned long last_thres_adaptation;
    last_thres_adaptation= jiffies;

    if (htmm_mode == HTMM_NO_MIG)
	goto put_task;

    if (!mm) {
	goto put_task;
    }

    if (!mmap_read_trylock(mm))
	goto put_task;

    vma = find_vma(mm, address);
    if (unlikely(!vma))
	goto mmap_unlock;
    
    if (!vma->vm_mm || !vma_migratable(vma) ||
	(vma->vm_file && (vma->vm_flags & (VM_READ | VM_WRITE)) == (VM_READ)))
	goto mmap_unlock;
    
    memcg = get_mem_cgroup_from_mm(mm);
    if (!memcg || !memcg->htmm_enabled)
	goto mmap_unlock;
    
    /* increase sample counts only for valid records */
    ret = __update_pginfo(vma, address);
    if (ret == 1) { /* memory accesses to DRAM */
	memcg->nr_sampled++;
	memcg->nr_sampled_for_split++;
	memcg->nr_dram_sampled++;
	memcg->nr_max_sampled++;
    }
    else if (ret == 2) {
	memcg->nr_sampled++;
	memcg->nr_sampled_for_split++;
	memcg->nr_max_sampled++;
    } else
	goto mmap_unlock;
    
    /* cooling and split decision */
    if (memcg->nr_sampled % htmm_cooling_period == 0 ||
	    need_memcg_cooling(memcg)) {
	/* cooling -- updates thresholds and sets need_cooling flags */
	if (__cooling(mm, memcg)) {
	    unsigned long temp_rhr = memcg->prev_dram_sampled;
	    /* updates actual access stat */
	    memcg->prev_dram_sampled >>= 1;
	    memcg->prev_dram_sampled += memcg->nr_dram_sampled;
	    memcg->nr_dram_sampled = 0;
	    /* updates estimated access stat */
	    memcg->prev_max_dram_sampled >>= 1;
	    memcg->prev_max_dram_sampled += memcg->max_dram_sampled;
	    memcg->max_dram_sampled = 0;

	    /* split decision period */
	    /* split should be performed after cooling due to skewness factor */
	    if (!memcg->need_split && htmm_thres_split) {
		unsigned long usage = page_counter_read(&memcg->memory);
		/* htmm_split_period: 2 by default
		 * This means that the number of sampled records should 
		 * exceed a quarter of the WSS
		 */
		usage >>= htmm_split_period;
		// the num. of samples must be larger than the fast tier size.
		usage = max(usage, memcg->max_nr_dram_pages);
	    
		if (memcg->nr_sampled_for_split > usage) {
		    /* if split is already performed in the previous
		     * and rhr is not improved, stop split huge pages */
		    if (memcg->split_happen) {
			if (memcg->prev_dram_sampled < (temp_rhr * 103 / 100)) { // 3%
			    htmm_thres_split = 0;
			    goto mmap_unlock;
			}
		    }
		    memcg->split_happen = false;
		    memcg->need_split = true;
		} else {
		    /* re-calculate split threshold due to cooling */
		    memcg->nr_split = memcg->nr_split + memcg->nr_split_tail_idx;
		    memcg->nr_split_tail_idx = 0;
		    set_memcg_split_thres(memcg);
		}
	    }
	    printk("total_accesses: %lu max_dram_hits: %lu cur_hits: %lu \n",
		    memcg->nr_max_sampled, memcg->prev_max_dram_sampled, memcg->prev_dram_sampled);
	    memcg->nr_max_sampled >>= 1;
	}
    }
    /* threshold adaptation */
    else if (memcg->nr_sampled % htmm_adaptation_period == 0) {
	__adjust_active_threshold(mm, memcg);
    }

mmap_unlock:
    mmap_read_unlock(mm);
put_task:
    put_pid(pid_struct);
}
