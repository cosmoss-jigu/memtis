/*
 *
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

static bool cooled = false;
static bool __split = false;

void htmm_mm_init(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    int i;

    mm->htmm_enabled = false;

    spin_lock_init(&mm->hri.lock);
    for (i = 0; i < NR_REGION_LIST; i++)
	INIT_LIST_HEAD(&mm->hri.region_list[i]);

    if (!memcg || !memcg->htmm_enabled) {
	return;
    }

    mm->htmm_enabled = true;
    INIT_LIST_HEAD(&mm->memcg_entry);
    mm->htmm_count = 0;

    //register_memcg_for_cooling(memcg);
}

void htmm_mm_exit(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct mm_struct *tmp, *_tmp;
    int i;

    if (!memcg)
	return;

    spin_lock(&mm->hri.lock);
    for (i = 0; i < NR_REGION_LIST; i++) {
	huge_region_t *node, *tmp;

	if (list_empty(&mm->hri.region_list[i]))
	    continue;

	list_for_each_entry_safe(node, tmp, &mm->hri.region_list[i], hr_entry) {
	    if (!node) {
		list_del(&node->hr_entry);
		huge_region_delete(mm, node->haddr);
		huge_region_free(node);
	    }
	}
    }
    spin_unlock(&mm->hri.lock);

    spin_lock(&memcg->htmm_mm_list_lock);
    list_for_each_entry_safe (tmp, _tmp, &memcg->htmm_mm_list, memcg_entry) {
	if (tmp == mm) {
	    list_del(&mm->memcg_entry);
	    break;
	}
    }
    spin_unlock(&memcg->htmm_mm_list_lock);
}

/* Hugepage uses tail pages to store access information.
 * See struct page declaration in linux/mm_types.h */
void __prep_transhuge_page_for_htmm(struct page *page)
{
    int i, idx, offset;
    struct mem_cgroup *memcg = page_memcg(page);
    
    pginfo_t pginfo = { 0, 0, 0, false, };

    /* third tail page */
    page[3].hot_utils = 0;
    page[3].total_accesses = 0;
    page[3].skewness_idx = 0;
    //page[3].acc_accesses = 0;
    page[3].idx = 0;
    SetPageHtmm(&page[3]);
    /* fourth~ tail pages */
    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 4;
	offset = i % 4;
	
	page[idx].compound_pginfo[offset] = pginfo;
	SetPageHtmm(&page[idx]);
    }

    if (!memcg)
	return;

    page[3].cooling_clock = memcg->cooling_clock;
}

void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
				  struct page *page)
{
    prep_transhuge_page(page);

    if (vma->vm_mm->htmm_enabled)
	__prep_transhuge_page_for_htmm(page);
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
    int check = 0;

    VM_BUG_ON_PAGE(!PageCompound(page), page);
    VM_BUG_ON_PAGE(!PageCompound(newpage), newpage);

    page = compound_head(page);
    newpage = compound_head(newpage);

    if (!PageHtmm(&page[3]))
	return;

    newpage[3].hot_utils = page[3].hot_utils;
    newpage[3].total_accesses = page[3].total_accesses;
    newpage[3].skewness_idx = page[3].skewness_idx;
    //newpage[3].acc_accesses = page[3].acc_accesses;
    newpage[3].cooling_clock = page[3].cooling_clock;
    newpage[3].idx = page[3].idx;

    SetPageHtmm(&newpage[3]);

    for (i = 0; i < HPAGE_PMD_NR; i++) {
	unsigned int nr_accesses;
	idx = 4 + i / 4;
	offset = i % 4;

	newpage[idx].compound_pginfo[offset].nr_accesses = page[idx].compound_pginfo[offset].nr_accesses;
	newpage[idx].compound_pginfo[offset].total_accesses = page[idx].compound_pginfo[offset].total_accesses;
	if (newpage[idx].compound_pginfo[offset].total_accesses)
	    check++;
	//newpage[idx].compound_pginfo[offset] = page[idx].compound_pginfo[offset];
	
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
    unsigned long prev_accessed, prev_idx, cur_idx;
    pginfo_t *pginfo;
    bool cooling_status, need_cooling = false;
    int i, idx, offset, bal = 0, _bal = 0;

    unsigned int memcg_cclock;

    if (!memcg || !memcg->htmm_enabled)
	return;

    meta_page = get_meta_page(page);

    spin_lock(&memcg->access_lock);
    /* check cooling */
    memcg_cclock = READ_ONCE(memcg->cooling_clock);
    if (memcg_cclock > meta_page->cooling_clock) {

	    int j;
	    unsigned int diff = memcg_cclock - meta_page->cooling_clock;
	    unsigned int skewness = 0;
	    unsigned int hot_refs = 0;

	    unsigned int for_analysis = 0;

	    /* perform cooling */
	    meta_page->hot_utils = 0;

	    for (i = 0; i < HPAGE_PMD_NR; i++) {
		idx = 4 + i / 4;
		offset = i % 4;

		pginfo =&(page[idx].compound_pginfo[offset]);
		prev_idx = get_idx(pginfo->total_accesses);
		if (prev_idx >= memcg->active_threshold) {
		    meta_page->hot_utils++;
		    hot_refs += get_accesses_from_idx(prev_idx);
		}
		skewness += (pginfo->total_accesses * pginfo->total_accesses);
		
		if (prev_idx >= (memcg->bp_active_threshold))
		    pginfo->may_hot = true;
		else
		    pginfo->may_hot = false;

		for (j = 0; j < diff; j++)
		    pginfo->total_accesses >>= 1;

		//skewness += (pginfo->total_accesses * pginfo->total_accesses);
		
		if (pginfo->nr_accesses >= 1) { // && addr != NULL) {
		    //unsigned long *tmp = (unsigned long) addr;
		    //unsigned long addr_offset = *tmp;
		    //addr_offset >>= PAGE_SHIFT;
		    //addr_offset += i;
		    //trace_printk("clock: %u addr: %lu access: %u\n", memcg_cclock, addr_offset, pginfo->nr_accesses);
		    for_analysis++;
		}

		pginfo->nr_accesses = 0;

		cur_idx = get_idx(pginfo->total_accesses);
		memcg->ebp_hotness_hg[cur_idx]++;
	    }

	    //trace_printk("memcg_id: %d access: %u util: %u\n",memcg->id.id, (unsigned int)meta_page->total_accesses, for_analysis); 
	    //memcg->pages_per_util[for_analysis]++;

	    /* for histogram management */
	    for (i = 0; i < diff; i++)		
		meta_page->total_accesses >>= 1;

	    cur_idx = get_idx(meta_page->total_accesses);

	    //spin_lock(&memcg->access_lock);

	    if (htmm_mode == HTMM_HUGEPAGE_OPT) {
		unsigned long hot_idx;
		
		hot_idx = meta_page->total_accesses + meta_page->hot_utils * htmm_util_weight / 10;
		hot_idx = get_idx(hot_idx);
		memcg->hp_hotness_hg[hot_idx] += HPAGE_PMD_NR;
		meta_page->idx = hot_idx;

		/* updates skewness */
		if (meta_page->hot_utils == 0 || meta_page->hot_utils == HPAGE_PMD_NR)
		    skewness = 0;
		else if (meta_page->idx >= 13) // very hot pages 
		    skewness = 0;
		else {
		    skewness = (skewness / HPAGE_PMD_NR) / 32; // 32 is for balancing histogram
		    skewness = skewness / (meta_page->hot_utils);
		    skewness = get_skew_idx(skewness);
		}
		memcg->access_map[skewness] += 1;//HPAGE_PMD_NR;
		meta_page->skewness_idx = skewness;
		/* ---------------- */
		/* updates avg_util */
		if (meta_page->hot_utils) {
		    hot_refs /= HPAGE_PMD_NR;
		    memcg->sum_util += hot_refs;
		    //memcg->sum_util += meta_page->hot_utils;
		    memcg->num_util += 1;
		}
		//memcg->num_util += 1;
	    }
	    else {
		memcg->access_map[cur_idx] += HPAGE_PMD_NR;
		meta_page->idx = cur_idx;
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
    bool cooling_status;
    unsigned int memcg_cclock;

    if (!memcg || !memcg->htmm_enabled)
	return;

    spin_lock(&memcg->access_lock);
    memcg_cclock = READ_ONCE(memcg->cooling_clock);
    if (memcg_cclock > pginfo->cooling_clock) {
	unsigned int diff = memcg_cclock - pginfo->cooling_clock;    
	int j;
	    
	prev_accessed = pginfo->total_accesses;
	pginfo->nr_accesses = 0;
	if (get_idx(prev_accessed) >= (memcg->bp_active_threshold))
	    pginfo->may_hot = true;
	else
	    pginfo->may_hot = false;
	for (j = 0; j < diff; j++)
	    pginfo->total_accesses >>= 1;
	    
	cur_idx = get_idx(pginfo->total_accesses);
	if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	    memcg->bp_hotness_hg[cur_idx]++;
	    memcg->ebp_hotness_hg[cur_idx]++;
	    //memcg->access_map[get_idx(prev_accessed)]--;
	    //memcg->access_map[cur_idx]++;
	}
	else
	    memcg->access_map[cur_idx]++;

	pginfo->cooling_clock = memcg_cclock;
    } else {
	pginfo->cooling_clock = memcg_cclock;
    }
    spin_unlock(&memcg->access_lock);
}

void set_page_coolstatus(struct page *page, pte_t *pte, struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct page *pte_page;
    pginfo_t *pginfo;

    if (!memcg || !memcg->htmm_enabled)
	return;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	return;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	return;
    
    pginfo->cooling_clock = READ_ONCE(memcg->cooling_clock);
    pginfo->may_hot = false;
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
	memcg->hp_hotness_hg[idx] += HPAGE_PMD_NR;
	spin_unlock(&memcg->access_lock);
    }
}

unsigned long deferred_split_scan_for_htmm(struct mem_cgroup_per_node *pn,
	struct list_head *split_list)
{
    struct deferred_split *ds_queue = &pn->deferred_split_queue;
    struct list_head *deferred_list = &pn->deferred_list;
    unsigned long flags;
    LIST_HEAD(list), *pos, *next;
    LIST_HEAD(failed_list);
    struct page *page;
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

	page = list_entry((void *)pos, struct page, deferred_list);
	page = compound_head(page);

	if (!PageLRU(page)) {
	    skip_iso = true;
	    goto skip_isolation;
	}

	if (lruvec != &pn->lruvec) {
	    printk("lruvec mismatch\n");
	    continue;
	}

	spin_lock_irq(&lruvec->lru_lock);
	if (!__isolate_lru_page_prepare(page, 0)) {
	    spin_unlock(&lruvec->lru_lock);
	    continue;
	}

	if (unlikely(!get_page_unless_zero(page))) {
	    spin_unlock(&lruvec->lru_lock);
	    continue;
	}

	if (!TestClearPageLRU(page)) {
	    put_page(page);
	    spin_unlock(&lruvec->lru_lock); 
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
    /*list_for_each_entry(page, &failed_list, lru) {
	struct page *tmp_page = compound_head(page);
	if (!list_empty(page_deferred_list(tmp_page))) {
	    ds_queue->split_queue_len--;
	    list_del_init(page_deferred_list(tmp_page));
	    ClearPageNeedSplit(tmp_page);
	}
    }*/
    list_splice_tail(&list, &ds_queue->split_queue);
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);
    
    putback_movable_pages(&failed_list); 
    if (split)
	__split = true;
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

#if 0
	if (page_count(page) == 1) {
	    ClearPageActive(page);
	    ClearPageUnevictable(page);
	    if (unlikely(__PageMovable(page))) {
		lock_page(page);
		if (!PageMovable(page))
		    __ClearPageIsolated(page);
		unlock_page(page);
	    }
	}
#endif

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

    accesses = (accesses + accesses << 1) >> 1;

    return accesses;
}

unsigned int get_idx(unsigned long num)
{
    unsigned int cnt = 0;
    unsigned int tmp, base = 1;

#if 1
    while (1) {
	num = num >> 1;
	if (num)
	    cnt++;
	else	
	    return cnt;
	
	if (cnt == 15)
	    break;
    }
#else
    tmp = num;
    while (1) {
	tmp = tmp >> 1;
	if (tmp) {
	    cnt++;
	    base <<= 1;
	}
	else
	    break;

	if (cnt == 12)
	    return cnt;
    }

    if (num > base)
	cnt++;
#endif

    return cnt;
}

int get_base_idx(unsigned int num)
{
    int cnt = num;
   
    if (num == 0)
	return -1;

    cnt = num - 1;
    if (cnt > 15)
	cnt = 15;

    return cnt;
}

int get_skew_idx(unsigned int num)
{
    int cnt = 0, tmp;
    
    /* 0, 1-3, 4-15, 16-63, 64-255, 256-1023, 1024-2047, 2048-3071, ... */
    tmp = num;
    if (tmp >= 1024) {
	while (tmp >= 1024 && cnt < 16) {
	    tmp -= 1024;
	    cnt++;
	}
	cnt += 5;
    }
    else {
	while (tmp) {
	    tmp >>= 2;
	    cnt++;
	}
    }

    return cnt;

/*
    cnt = num / 256;
    if (cnt > 20)
	cnt = 20;
*/
    return cnt;
}

unsigned int get_weight(uint8_t history)
{
    unsigned int w = 0;

    while (history != 0) {
	history &= (history - 1);
	w++;
    }

    return w;
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
    unsigned int idx, idx2;
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
    idx2 = get_base_idx(pginfo->nr_accesses);

    spin_lock(&memcg->access_lock);
    if (memcg->bp_hotness_hg[idx] > 0)
	memcg->bp_hotness_hg[idx]--;
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

	if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	    idx = meta->idx;
	}
	else
	    idx = get_idx(meta->total_accesses);

	spin_lock(&memcg->access_lock);
	if (memcg->hp_hotness_hg[idx] >= nr_pages)
	    memcg->hp_hotness_hg[idx] -= nr_pages;
	else
	    memcg->hp_hotness_hg[idx] = 0;
	
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

void charge_htmm_page(struct page *page, struct mem_cgroup *memcg)
{
    unsigned int nr_pages = thp_nr_pages(page);

    // nop. will be removed.
    return;
}

bool region_for_toptier(huge_region_t *node)
{
    unsigned long hotness;
    
    return false;
}

static bool reach_cooling_thres(pginfo_t *pginfo, struct page *meta_page, bool hugepage)
{
    if (hugepage) {
	if (htmm_mode == HTMM_BASELINE)
	    return meta_page->total_accesses >= htmm_thres_cold;
	else if (htmm_mode == HTMM_HUGEPAGE_OPT)
	    return pginfo->nr_accesses >= htmm_thres_cold;
	else
	    return false;
    }

    return pginfo->nr_accesses >= htmm_thres_cold;
}

static bool need_lru_cooling(struct mm_struct *mm,
			     struct page *page)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct lruvec *lruvec;
    unsigned long cur, next, now = jiffies;
    unsigned long nr_active_pages = 0;
    int nid;

    cur = memcg->htmm_next_cooling;
    //if (time_before(now, cur))
//	return false;

    if (time_after(now, cur + msecs_to_jiffies(htmm_max_cooling_interval - htmm_min_cooling_interval)))
	goto need_cooling;
    
    for_each_node_state(nid, N_MEMORY) {
        lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
	nr_active_pages += lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);
    }

    memcg->nr_active_pages = nr_active_pages;
    if (nr_active_pages <= memcg->max_nr_dram_pages)
	return false;

need_cooling:
    /* need cooling */
    /*
     * The next cooling operation can be executed only after
     * the htmm_min_cooling_interval has elapsed.
     */
    next = now + msecs_to_jiffies(htmm_min_cooling_interval);
    if (cmpxchg(&memcg->htmm_next_cooling, cur, next) != cur)
	return false;

    return true;
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

static void update_huge_region(struct vm_area_struct *vma, 
	unsigned long haddr, bool newly_hot)
{
    struct mm_struct *mm = vma->vm_mm;
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    huge_region_t *node;

    node = huge_region_lookup(mm, haddr);
    if (!node) {
	node = huge_region_alloc();
	if (!node)
	    return;

	node->vma = vma;
	node->haddr = haddr;
	huge_region_insert(vma->vm_mm, haddr, node);
	
	spin_lock(&mm->hri.lock);
    } else {
	spin_lock(&mm->hri.lock);
	if (list_empty(&node->hr_entry)) {
	    spin_unlock(&mm->hri.lock);
	    return;
	}
	list_del(&node->hr_entry);
    }
    
    node->total_accesses;
    if (newly_hot) {
	node->hot_utils++;
    }
    
    if (node->hot_utils >= (HPAGE_PMD_NR * 95 / 100))
	list_add(&node->hr_entry, &mm->hri.region_list[HUGE_TOPTIER]);
    else
	list_add(&node->hr_entry, &mm->hri.region_list[BASE_PAGES]);
    
    spin_unlock(&mm->hri.lock);
}

bool check_split_huge_page(struct mem_cgroup *memcg,
	struct page *meta, bool hot)
{
    unsigned long split_thres = memcg->split_threshold;
    bool tail_idx = false;
    
    if (htmm_thres_split == 0)
	return false;

    if (memcg->nr_split == 0) {
	if (memcg->nr_split_tail_idx == 0)
	    return false;
	if (split_thres == 1)
	    return false;
	tail_idx = true;
	split_thres--;
    }

    //if (meta->idx >= memcg->split_active_threshold)
//	return false;
    
    if (!list_empty(page_deferred_list(compound_head(meta)))) {
	return false;
    }


    if (meta->skewness_idx >= split_thres) {
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

    return false;
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
    int prev_base_idx, cur_base_idx;
    bool hot, cooling_status;

    /* check cooling status and perform cooling if the page needs to be cooled */
    check_base_cooling(pginfo, page, false);

    prev_accessed = pginfo->total_accesses;
    //prev_base_idx = get_base_idx(pginfo->nr_accesses);
    pginfo->nr_accesses++;
    pginfo->total_accesses += HPAGE_PMD_NR;
    
    if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	prev_idx = get_idx(prev_accessed);
	cur_idx = get_idx(pginfo->total_accesses);
	
	spin_lock(&memcg->access_lock);
	
	if (prev_idx != cur_idx) {
	    if (memcg->bp_hotness_hg[prev_idx] > 0)
		memcg->bp_hotness_hg[prev_idx]--;
	    memcg->bp_hotness_hg[cur_idx]++;
	    if (memcg->ebp_hotness_hg[prev_idx] > 0)
		memcg->ebp_hotness_hg[prev_idx]--;
	    memcg->ebp_hotness_hg[cur_idx]++;
	}
	
	if (pginfo->nr_accesses == 1)
	    memcg->base_map[0]++;
	if (pginfo->may_hot == true)
	    memcg->base_map[1]++;
	if (cur_idx >= (memcg->bp_active_threshold))
	    pginfo->may_hot = true;
	else
	    pginfo->may_hot = false;
	
	spin_unlock(&memcg->access_lock);
    } else {
	prev_idx = get_idx(prev_accessed);
	cur_idx = get_idx(pginfo->total_accesses);
	if (prev_idx != cur_idx) {
	    spin_lock(&memcg->access_lock);
	    
	    if (memcg->access_map[prev_idx] > 0)
		memcg->access_map[prev_idx]--;
	    memcg->access_map[cur_idx]++;
	    
	    spin_unlock(&memcg->access_lock);
	}
    }

    hot = cur_idx >= memcg->active_threshold;
    if (PageActive(page) && !hot)
	move_page_to_inactive_lru(page);
    else if (!PageActive(page) && hot)
	move_page_to_active_lru(page);
}

static void update_huge_page(struct vm_area_struct *vma, pmd_t *pmd,
	struct page *page, unsigned long address)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);
    struct page *meta_page;
    pginfo_t *pginfo;
    unsigned long prev_idx, cur_idx;
    bool hot, cooling_status;
    bool pg_split = false;
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

    if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	/* base page */
	prev_idx = get_idx(pginfo_prev);
	cur_idx = get_idx(pginfo->total_accesses);
	spin_lock(&memcg->access_lock);
	if (prev_idx != cur_idx) {
	    if (memcg->ebp_hotness_hg[prev_idx] > 0)
		memcg->ebp_hotness_hg[prev_idx]--;
	    memcg->ebp_hotness_hg[cur_idx]++;
	}
	if (pginfo_prev == 0)
	    memcg->base_map[0]++;
	if (pginfo->may_hot == true)
	    memcg->base_map[1]++;
	if (cur_idx >= (memcg->bp_active_threshold))
	    pginfo->may_hot = true;
	else
	    pginfo->may_hot = false;
	spin_unlock(&memcg->access_lock);
	
	/* hugepage */
	prev_idx = meta_page->idx;
	cur_idx = meta_page->total_accesses + meta_page->hot_utils * htmm_util_weight / 10;
	cur_idx = get_idx(cur_idx);
	if (prev_idx != cur_idx) {
	    spin_lock(&memcg->access_lock);
	    if (memcg->hp_hotness_hg[prev_idx] >= HPAGE_PMD_NR)
		memcg->hp_hotness_hg[prev_idx] -= HPAGE_PMD_NR;
	    else
		memcg->hp_hotness_hg[prev_idx] = 0;
	    memcg->hp_hotness_hg[cur_idx] += HPAGE_PMD_NR;
	    spin_unlock(&memcg->access_lock);
	}
	meta_page->idx = cur_idx;
    } else {
	prev_idx = meta_page->idx;
	cur_idx = get_idx(meta_page->total_accesses);
        if (prev_idx != cur_idx) {
	    spin_lock(&memcg->access_lock);
	    if (memcg->access_map[prev_idx] >= HPAGE_PMD_NR)
		memcg->access_map[prev_idx] -= HPAGE_PMD_NR;
	    else
		memcg->access_map[prev_idx] = 0;
	    memcg->access_map[cur_idx] += HPAGE_PMD_NR;
	    spin_unlock(&memcg->access_lock);
	}
	meta_page->idx = cur_idx;
    }

    if (pg_split)
	return;

    hot = cur_idx >= memcg->active_threshold;
    if (PageActive(page) && !hot) {
	move_page_to_inactive_lru(page);
    } else if (!PageActive(page) && hot) {
	move_page_to_active_lru(page);
    }
}

static void update_huge_page_loop(struct vm_area_struct *vma,
	struct page *page, unsigned long address)
{
    struct page *meta_page;
    pginfo_t *pginfo;
    unsigned long pg_offset = (address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT;
    unsigned long start, end;

    if (pg_offset >= htmm_base_spatial_count)
	start = address - (htmm_base_spatial_count << PAGE_SHIFT);
    else
	start = address - (pg_offset << PAGE_SHIFT);
    
    if (pg_offset < (HPAGE_PMD_NR - htmm_base_spatial_count))
	end = address + (htmm_base_spatial_count << PAGE_SHIFT);
    else
	end = address + ((HPAGE_PMD_NR - pg_offset - 1) << PAGE_SHIFT);

    meta_page = get_meta_page(page);
    for (; start <= end; start += PAGE_SIZE) {
	pginfo = get_compound_pginfo(page, start);
	
	pginfo->nr_accesses++;
	meta_page->total_accesses++;

	if (pginfo->nr_accesses == htmm_thres_hot)
	    meta_page->hot_utils++;

	/* if page is active, hotness will be updated by the cooling op */
	if (!PageActive(page)) {
	    struct mem_cgroup *memcg = page_memcg(page);

	    BUG_ON(!memcg->htmm_enabled);

	    //meta_page->prev_hv = meta_page->cur_hv;
	    //meta_page->cur_hv = cal_huge_hotness(memcg, (void *)meta_page, true);

	    if (is_hot_huge_page(meta_page))
		move_page_to_active_lru(page);
	}

	if (reach_cooling_thres(pginfo, meta_page, true) &&
		need_lru_cooling(vma->vm_mm, page))
	    set_lru_cooling(vma->vm_mm);
    }
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
pte_retry:
    ptent = *pte;
    if (!pte_present(ptent))
	goto pte_unlock;

    page = vm_normal_page(vma, address, ptent);
    if (!page || PageKsm(page))
	goto pte_unlock;

    //if (!PageLocked(page))
//	goto pte_unlock;

    if (page != compound_head(page))
	goto pte_unlock;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	goto pte_unlock;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	goto pte_unlock;

    /*if (!trylock_page(page))
	goto pte_unlock;

    if (!pte_same(ptent, *pte)) {
	unlock_page(page);
	goto pte_retry;
    }*/

    update_base_page(vma, page, pginfo);
    pte_unmap_unlock(pte, ptl);
    if (node_is_toptier(page_to_nid(page)))
	return 1;
    else
	return 2;

pte_unlock:
    pte_unmap_unlock(pte, ptl);
    return ret;
}

static int __update_pte_pginfo_loop(struct vm_area_struct *vma, pmd_t *pmd,
				     unsigned long address)
{
    pte_t *pte, ptent;
    spinlock_t *ptl;
    pginfo_t *pginfo;
    struct page *page, *pte_page;
    unsigned long pg_offset = (address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT;
    unsigned long start, end;

    if (pg_offset >= htmm_base_spatial_count)
	start = address - (htmm_base_spatial_count << PAGE_SHIFT);
    else
	start = address - (pg_offset << PAGE_SHIFT);
    
    if (pg_offset < (HPAGE_PMD_NR - htmm_base_spatial_count))
	end = address + (htmm_base_spatial_count << PAGE_SHIFT);
    else
	end = address + ((HPAGE_PMD_NR - pg_offset - 1) << PAGE_SHIFT);

    pte = pte_offset_map_lock(vma->vm_mm, pmd, start, &ptl);
    for (; start <= end; start += PAGE_SIZE, pte++) {
	ptent = *pte;
        if (!pte_present(ptent))
	    continue;

	page = vm_normal_page(vma, start, ptent);
	if (!page || PageKsm(page) || PageLocked(page))
	    continue;

	if (!PageLRU(page))
	    continue;

	if (page != compound_head(page))
	    continue;

	pte_page = virt_to_page((unsigned long)pte);
	if (!PageHtmm(pte_page))
	    continue;

	pginfo = get_pginfo_from_pte(pte);
	if (!pginfo)
	    continue;

	pginfo->nr_accesses++;
	if (pginfo->nr_accesses >= htmm_thres_hot) {
	    bool newly_hot = false;
	    if (!PageActive(page)) {
		move_page_to_active_lru(page);
		newly_hot = true;
	    }
	    if (htmm_mode == HTMM_HUGEPAGE_OPT &&
		transhuge_vma_suitable(vma, start & HPAGE_PMD_MASK)) {
		update_huge_region(vma, start & HPAGE_PMD_MASK, true);
	    }
	}

	if (reach_cooling_thres(pginfo, NULL, false) && need_lru_cooling(vma->vm_mm, page))
	    set_lru_cooling(vma->vm_mm);
    }
    
    pte_unmap_unlock(--pte, ptl);
    return 0;
}

static int __update_pmd_pginfo(struct vm_area_struct *vma, pud_t *pud,
				unsigned long address)
{
    pmd_t *pmd, pmdval;
    spinlock_t *ptl;
    bool ret = 0;

    pmd = pmd_offset(pud, address);
pmd_retry:
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
	
	/*ptl = pmd_lock(vma->vm_mm, pmd);
	if (unlikely(!pmd_same(pmdval, *pmd)))
	    goto pmd_unlock;*/

	page = pmd_page(pmdval);
	if (!page)
	    goto pmd_unlock;
	
	if (!PageCompound(page)) {
	    goto pmd_unlock;
	}
	/*if (!trylock_page(page))
	    goto pmd_unlock;

	if (!pmd_same(pmdval, *pmd)) {
	    unlock_page(page);
	    goto pmd_retry;
	}*/

	update_huge_page(vma, pmd, page, address);
pmd_unlock:
	if (node_is_toptier(page_to_nid(page)))
	    return 1;
	else
	    return 2;
    }

    /* base page */
    if (htmm_base_spatial_count)
	return __update_pte_pginfo_loop(vma, pmd, address);
    else
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
    long nr_dram = memcg->max_nr_dram_pages;
    long nr_extra = memcg->num_util ? (nr_split / HPAGE_PMD_NR) * (memcg->sum_util / memcg->num_util) : 0;
    int i;

#if 1
    if (memcg->nr_split == 0) {
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
#else
    if (memcg->nr_split == 0) {
	memcg->split_active_threshold = 16;
	return;
    }

    spin_lock(&memcg->access_lock);
    memcg->split_active_threshold = 16;
    for (i = 15; i > 0; i--) {
	long nr_bp = memcg->bp_hotness_hg[i];
	long nr_hp = memcg->hp_hotness_hg[i];

	if (nr_bp + nr_hp+ nr_extra < nr_dram) {
	    nr_dram -= (nr_bp + nr_hp);
	    memcg->split_threshold = i;
	    continue;
	}

	if (nr_hp == 0)
	    continue;

	if (nr_hp < nr_split) {
	    nr_split -= nr_hp;
	    continue;
	}
	memcg->split_active_threshold = i;
	break;
    }
    spin_unlock(&memcg->access_lock);
#endif
}

static void set_memcg_nr_split(struct mem_cgroup *memcg)
{
    int i;
    unsigned long total_samples, target_samples;
    unsigned long nr_base = 0, nr_base_samples = 0, nr_cur = 0, nr_cur_samples = 0;
    unsigned long nr_huge = 0;
    unsigned int avg_util;
    long nr_ebp = 0;
    bool stop = false;

    memcg->nr_split = 0;
    memcg->nr_split_tail_idx = 0;
    if (memcg->prev_max_dram_sampled <= (memcg->prev_dram_sampled * 105 / 100))
	return;
    
    if (memcg->num_util == 0)
	return;
    
    /* cooling halves the access counts so that
     * NR_SAMPLE(n) = Cooling_thres + NR_SAMPLE(n-1) / 2
     */
    total_samples = 2 * htmm_thres_cold - (htmm_thres_cold >> (memcg->cooling_clock - 1));

    for (i = 15; i > 0; i--) {
	nr_huge += memcg->hp_hotness_hg[i];
    }

    avg_util = memcg->sum_util / memcg->num_util;
    //memcg->nr_split = nr_huge * (memcg->prev_max_dram_sampled - memcg->prev_dram_sampled) / total_samples;
    memcg->nr_split = (memcg->prev_max_dram_sampled - memcg->prev_dram_sampled) / avg_util;
    memcg->nr_split *= HPAGE_PMD_NR;
    //printk("memcg->nr_split: %lu\n", memcg->nr_split);
    return;

#if 0 
    total_samples = 2 * htmm_thres_cold - (htmm_thres_cold >> (memcg->cooling_clock - 1));
    target_samples = total_samples * memcg->prev_max_dram_sampled / htmm_thres_cold;
    
    spin_lock(&memcg->access_lock);
    /* base histogram */
    for (i = 15; i > 0; i--) {
	unsigned long nr_accesses, i2a;

	i2a = get_accesses_from_idx(i);
	nr_accesses = memcg->ebp_hotness_hg[i] * i2a;
	nr_accesses /= HPAGE_PMD_NR;
	//nr_ebp += memcg->ebp_hotness_hg[i];
	if (stop)
	    continue;

	if (nr_accesses + nr_base_samples < target_samples) {
	    nr_base += memcg->ebp_hotness_hg[i];
	    nr_base_samples += nr_accesses;
	}
	else {
	    unsigned long nr_pages = (target_samples - nr_base_samples) * HPAGE_PMD_NR / i2a;
	    nr_base += nr_pages;
	    nr_base_samples += nr_pages * i2a / HPAGE_PMD_NR;
	    stop = true;
	    break;
	}
    }

    if (memcg->num_util == 0)
	avg_util = 0;
    else
	avg_util = (memcg->sum_util / memcg->num_util);

    //if (nr_ebp < memcg->max_nr_dram_pages)
//	nr_ebp = memcg->max_nr_dram_pages - nr_ebp;
  //  else
//	nr_ebp = 0;

    /* normal histogram */
    for (i = 15; i > 0; i--) {
	unsigned long nr_accesses, i2a;

	i2a = get_accesses_from_idx(i);
	nr_accesses = memcg->hp_hotness_hg[i] * i2a / HPAGE_PMD_NR; // due to the size of huge pages
	nr_accesses += memcg->bp_hotness_hg[i] * i2a / HPAGE_PMD_NR; // due to the base's hotness calculation method

	//nr_ebp -= memcg->hp_hotness_hg[i] * (HPAGE_PMD_NR - avg_util);
	//if (nr_ebp <= 0) {
	//    memcg->split_active_threshold = i + 1;
	//}

	if (nr_accesses + nr_cur_samples < target_samples) {
	    nr_cur += memcg->hp_hotness_hg[i];
	    //nr_cur += memcg->bp_hotness_hg[i];
	    nr_cur_samples += nr_accesses;
	}
	else {
	    unsigned long nr_pages = (target_samples - nr_cur_samples) * HPAGE_PMD_NR / i2a;
	    nr_cur += nr_pages;
	    break;
	    if (memcg->hp_hotness_hg[i] >= nr_pages)
		nr_cur += nr_pages;
	    else {
		nr_pages = (target_samples - nr_cur_samples - memcg->hp_hotness_hg[i] * i2a) * HPAGE_PMD_NR / i2a;
		nr_cur += (memcg->hp_hotness_hg[i] + nr_pages);
	    }
	    break;
	}
    }

    if (nr_cur < nr_base) {
	//printk("abnormal status... nr_cur: %lu is smaller than nr_base: %lu\n", nr_cur, nr_base);
	nr_cur = nr_base;
    }

    avg_util = avg_util * 100 / HPAGE_PMD_NR;
    memcg->nr_split = 100 * (nr_cur - nr_base) / (100 - avg_util);
    spin_unlock(&memcg->access_lock);
#endif
}

static void __cooling(struct mm_struct *mm,
	struct mem_cgroup *memcg, bool enforced)
{
    int nid, i;
    bool cool = false;
    unsigned long nr_active = 0, nr_dram_sampled = 0;

    /* check whether the previous cooling is done or not. */
    for_each_node_state(nid, N_MEMORY) {
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];

	if (pn && READ_ONCE(pn->need_cooling))
	    return;
    }

    if (!enforced && htmm_mode == HTMM_HUGEPAGE_OPT) {
	spin_lock(&memcg->access_lock);
	for (i = 15; i >= (memcg->active_threshold - 1) && i > 0; i--) {
	    unsigned long nr_pages = memcg->hotness_hg[i];
	    if (nr_active + nr_pages > memcg->max_nr_dram_pages) {
		if (nr_active < (memcg->max_nr_dram_pages * 8 / 10))
		    cool = true;
	    }
	    nr_active += nr_pages;
	}
	spin_unlock(&memcg->access_lock);
    }

    if (!enforced && !cool)
	return;

    spin_lock(&memcg->access_lock);
    
#if 0
    nr_dram_sampled = memcg->base_map[1];
    if (nr_dram_sampled > memcg->max_nr_dram_pages)
	nr_dram_sampled -= memcg->max_nr_dram_pages;
    else
	nr_dram_sampled = 0;
    memcg->prev_max_dram_sampled = htmm_thres_cold - nr_dram_sampled;
#endif
    memcg->prev_max_dram_sampled >>= 1;
    memcg->prev_max_dram_sampled += memcg->base_map[1];

    memcg->cooling_status = !(memcg->cooling_status);
    if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	for (i = 0; i <= 15; i++) {
	    memcg->hotness_hg[i] = 0;
	    memcg->hp_hotness_hg[i] = 0;
	    memcg->bp_hotness_hg[i] = 0;
	    memcg->ebp_hotness_hg[i] = 0;
	}
	for (i = 0; i < 21; i++)
	    memcg->access_map[i] = 0;
	for (i = 0; i < 2; i++)
	    memcg->base_map[i] = 0;
    } else {
	for (i = 0; i <= 15; i++) {
	    memcg->access_map[i] = 0;
	}
    }
    memcg->cooling_clock++;
    memcg->bp_active_threshold--;
    memcg->sum_util = 0;
    memcg->num_util = 0;

    smp_mb();
    spin_unlock(&memcg->access_lock);
    set_lru_cooling(mm);
    cooled = true;
}

static bool __adjust_active_threshold(struct mm_struct *mm,
	struct mem_cgroup *memcg)
{
    unsigned long nr_active = 0;
    bool cool = false;
    bool need_warm = false;
    int idx_hot, idx_bp;

    if (need_cooling(memcg))
	return cool;

    if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	spin_lock(&memcg->access_lock);

	for (idx_hot = 15; idx_hot >= 0; idx_hot--) {
	    unsigned long nr_pages = memcg->hp_hotness_hg[idx_hot] + memcg->bp_hotness_hg[idx_hot];
	    
	    if (nr_active + nr_pages > memcg->max_nr_dram_pages) {
		if (idx_hot != 15)
		    idx_hot++;
		break;
	    }
	    nr_active += nr_pages;
	}

	if (nr_active < (memcg->max_nr_dram_pages * 9 / 10))
	    need_warm = true;

	nr_active = 0;
	for (idx_bp = 15; idx_bp >= 0; idx_bp--) {
	    unsigned long nr_pages = memcg->ebp_hotness_hg[idx_bp];
	    if (nr_active + nr_pages > memcg->max_nr_dram_pages) {
		if (idx_bp != 15)
		    idx_bp++;
		break;
	    }
	    nr_active += nr_pages;
	}
	spin_unlock(&memcg->access_lock);
    } else {
	spin_lock(&memcg->access_lock);
	for (idx_hot = 15; idx_hot >= 0; idx_hot--) {
	    nr_active += memcg->access_map[idx_hot];
	    if (nr_active >= memcg->max_nr_dram_pages)
		break;
	}
	spin_unlock(&memcg->access_lock);
    }

    if (idx_hot < 0)
	idx_hot = 1;
    if (idx_bp < 0)
	idx_bp = 1;


    if (cooled) {
	if (idx_hot < memcg->active_threshold) {
	    printk("hotness idx: %u --> %u", memcg->active_threshold, memcg->active_threshold - 1);
	    if (memcg->active_threshold > 1)
		memcg->active_threshold--;
	}
	if (idx_bp < memcg->bp_active_threshold) {
	    //printk("bp_hotness_hg: %u --> %d\n", memcg->bp_active_threshold, idx_bp);
	    memcg->bp_active_threshold = idx_bp;
	}
#if 1
	if (memcg->need_split) {
	    /* set the target number of pages to be split */
	    set_memcg_nr_split(memcg);
	    
	    /* set the split threshold */
	    set_memcg_split_thres(memcg);
	    
	    if (memcg->num_util)
		printk("[split info] nr_split: %u, split_thres: %u, avg_util: %u\n", memcg->nr_split, memcg->split_threshold, memcg->sum_util / memcg->num_util);

	    memcg->nr_sampled_for_split = 0;
	}
#endif
	cooled = false;
	memcg->need_split = false;
	set_lru_adjusting(memcg, true);
    } else {
	if (idx_hot > memcg->active_threshold) {
	    printk("hotness idx: %u --> %d", memcg->active_threshold, idx_hot);
	    memcg->active_threshold = idx_hot;
	    set_lru_adjusting(memcg, false);
	} else if (__split && htmm_thres_split == 2) {
	    printk("hotness idx: %u --> %u due to split\n", memcg->active_threshold, idx_hot);
	    memcg->active_threshold = idx_hot;
	    set_lru_adjusting(memcg, true);
	    __split = false;
	}

	if (idx_bp > 0) {
	    //printk("bp_hotness_hg: %u --> %d\n", memcg->bp_active_threshold, idx_bp);
	    memcg->bp_active_threshold = idx_bp;
	}
    }

    if (need_warm && htmm_static_thres != 2)
	memcg->warm_threshold = memcg->active_threshold - 1;
    else
	memcg->warm_threshold = memcg->active_threshold;

    if (htmm_static_thres == 1) {
	memcg->active_threshold = htmm_thres_hot;
	memcg->warm_threshold = htmm_thres_hot;
    }

    return cool;
}

static void __enable_huge_split(struct mem_cgroup *memcg)
{
    memcg->need_split = true;
}

void update_pginfo(pid_t pid, unsigned long address, enum events e)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_struct ? pid_task(pid_struct, PIDTYPE_PID) : NULL;
    struct mm_struct *mm = p ? p->mm : NULL;
    struct vm_area_struct *vma; 
    struct mem_cgroup *memcg;
    bool missed = true;
    int ret;

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
    if (ret == 1) {
	memcg->nr_sampled++;
	memcg->nr_sampled_for_split++;
	memcg->nr_dram_sampled++;
    }
    else if (ret == 2) {
	memcg->nr_sampled++;
	memcg->nr_sampled_for_split++;
    } else
	goto mmap_unlock;

    /* periodic events */
    if (memcg->nr_sampled % htmm_thres_cold == 0) {
	__cooling(mm, memcg, true);
	memcg->prev_dram_sampled >>= 1;
	memcg->prev_dram_sampled += memcg->nr_dram_sampled;
	memcg->nr_dram_sampled = 0;
	
	if (!memcg->need_split) {
	    unsigned long usage = page_counter_read(&memcg->memory);
	    usage >>= htmm_thres_huge_hot;
	    if (memcg->nr_sampled_for_split > usage) {
		memcg->need_split = true;
	    }
	}

	printk("max_dram_ratio: %lu dram ratio: %lu \n", memcg->prev_max_dram_sampled, memcg->prev_dram_sampled);
    } else if (memcg->nr_sampled % htmm_thres_adjust == 0) {
	__adjust_active_threshold(mm, memcg);
    }

mmap_unlock:
    mmap_read_unlock(mm);
put_task:
    put_pid(pid_struct);
}

void set_lru_split_pid(pid_t pid)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_task(pid_struct, PIDTYPE_PID);
    struct mm_struct *mm = p ? p->mm : NULL;
    struct mem_cgroup *memcg = mm ? get_mem_cgroup_from_mm(mm) : NULL;

    if (!mm)
	goto put_task;

    if (!memcg)
	goto put_task;

    memcg->need_split = true;
    //set_lru_adjusting(mm, true);
    
put_task:
    put_pid(pid_struct);
}

void adjust_active_threshold(pid_t pid)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_task(pid_struct, PIDTYPE_PID);
    struct mm_struct *mm = p ? p->mm : NULL;
    struct mem_cgroup *memcg = mm ? get_mem_cgroup_from_mm(mm) : NULL;
    
    if (!mm)
	goto put_task;
    
    if (!memcg)
	goto put_task;

    __adjust_active_threshold(mm, memcg);
put_task:
    put_pid(pid_struct);
}

void set_lru_cooling_pid(pid_t pid)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_task(pid_struct, PIDTYPE_PID);
    struct mm_struct *mm = p ? p->mm : NULL;
    struct mem_cgroup *memcg;

    if (!mm)
	goto put_task;

    memcg = get_mem_cgroup_from_mm(mm);
    if (!memcg || !memcg->htmm_enabled)
	goto put_task;

    __cooling(mm, memcg, false);
put_task:
    put_pid(pid_struct);
}

struct kmem_cache *huge_region_cachep;

static int __init huge_region_kmem_cache_init(void)
{
    huge_region_cachep = kmem_cache_create("huge_region",
					   sizeof(huge_region_t), 0,
					   SLAB_PANIC, NULL);
    return 0;
}
core_initcall(huge_region_kmem_cache_init);

huge_region_t *huge_region_alloc(void)
{
    huge_region_t *node;
    node =(huge_region_t *) kmem_cache_zalloc(huge_region_cachep, GFP_KERNEL);
    if (!node)
	return NULL;

    /* initialize */
    INIT_LIST_HEAD(&node->hr_entry);
    spin_lock_init(&node->lock);
    node->vma = NULL;
    return node;
}

void huge_region_free(huge_region_t *node)
{
    kmem_cache_free(huge_region_cachep, (void *)node);
}

void *huge_region_lookup(struct mm_struct *mm, unsigned long addr)
{
    return xa_load(&mm->root_huge_region_map, addr >> HPAGE_SHIFT);
}

void *huge_region_delete(struct mm_struct *mm, unsigned long addr)
{
    return xa_erase(&mm->root_huge_region_map, addr >> HPAGE_SHIFT);
}

void *huge_region_insert(struct mm_struct *mm, unsigned long addr,
			 huge_region_t *node)
{
    return xa_store(&mm->root_huge_region_map, addr >> HPAGE_SHIFT,
		    (void *)node, GFP_KERNEL);
}
