/*
 *
 */
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/mm_inline.h>
#include <linux/pid.h>
#include <linux/htmm.h>
#include <linux/mempolicy.h>
#include <linux/swap.h>
#include <linux/sched/task.h>
#include <linux/xarray.h>
#include <linux/math.h>

#include "internal.h"
#include <asm/pgtable.h>

void htmm_mm_init(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    int i;

    if (!memcg || !memcg->htmm_enabled) {
	mm->htmm_enabled = false;
	return;
    }
    
    mm->htmm_enabled = true;
    spin_lock_init(&mm->hri.lock);
    for (i = 0; i < NR_REGION_LIST; i++)
	INIT_LIST_HEAD(&mm->hri.region_list[i]);
}

/* Hugepage uses tail pages to store access information.
 * See struct page declaration in linux/mm_types.h */
void __prep_transhuge_page_for_htmm(struct page *page)
{
    int i, idx, offset;
    pginfo_t pginfo = { 0, };

    /* third tail page */
    page[3].hot_utils = 0;
    page[3].total_accesses = 0;
    page[3].cur_hv = 0;
    page[3].prev_hv = 0;
    SetPageHtmm(&page[3]);
    /* fourth~ tail pages */
    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 8;
	offset = i % 8;
	
	page[idx].compound_pginfo[offset] = pginfo;
	SetPageHtmm(&page[idx]);
    }
}

void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
				  struct page *page)
{
    prep_transhuge_page(page);

    if (vma->vm_mm->htmm_enabled)
	__prep_transhuge_page_for_htmm(page);
}

void copy_transhuge_pginfo(struct page *page,
			   struct page *newpage)
{
    int i, idx, offset;
    pginfo_t zero_pginfo = { 0 };

    VM_BUG_ON_PAGE(!PageCompound(page), page);
    VM_BUG_ON_PAGE(!PageCompound(newpage), newpage);

    if (!PageHtmm(&page[3]))
	return;

    newpage[3].hot_utils = page[3].hot_utils;
    newpage[3].total_accesses = page[3].total_accesses;
    newpage[3].cur_hv = page[3].cur_hv;
    newpage[3].prev_hv = page[3].prev_hv;
    SetPageHtmm(&newpage[3]);

    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 8;
	offset = i % 8;

	newpage[idx].compound_pginfo[offset] = page[idx].compound_pginfo[offset];
	page[idx].compound_pginfo[offset] = zero_pginfo;
	SetPageHtmm(&newpage[idx]);
    }
}

pginfo_t *get_compound_pginfo(struct page *page, unsigned long address)
{
    int idx, offset;
    VM_BUG_ON_PAGE(!PageCompound(page), page);
    
    idx = 4 + ((address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT) / 8;
    offset = ((address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT) % 8;

    return &(page[idx].compound_pginfo[offset]);
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

void deferred_split_huge_page_for_htmm(struct page *page)
{
    struct deferred_split *ds_queue = get_deferred_split_queue_for_htmm(page);
    unsigned long flags;

    VM_BUG_ON_PAGE(!PageTransHuge(page), page);

    if (PageSwapCache(page))
	return;

    if (!ds_queue)
	return;
    
    spin_lock_irqsave(&ds_queue->split_queue_lock, flags);
    if (list_empty(page_deferred_list(page))) {
	count_vm_event(THP_DEFERRED_SPLIT_PAGE);
	list_add_tail(page_deferred_list(page), &ds_queue->split_queue);
	ds_queue->split_queue_len++;
    }
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);
}

unsigned long deferred_split_scan_for_htmm(struct mem_cgroup_per_node *pn)
{
    struct deferred_split *ds_queue = &pn->deferred_split_queue;
    unsigned long flags;
    LIST_HEAD(list), *pos, *next;
    struct page *page;
    int split = 0;

    spin_lock_irqsave(&ds_queue->split_queue_lock, flags);
    list_for_each_safe(pos, next, &ds_queue->split_queue) {
	page = list_entry((void *)pos, struct page, deferred_list);
	page = compound_head(page);
	if (get_page_unless_zero(page)) {
	    list_move(page_deferred_list(page), &list);
	} else {
	    list_del_init(page_deferred_list(page));
	    ds_queue->split_queue_len--;
	}
    }
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);

    list_for_each_safe(pos, next, &list) {
	page = list_entry((void *)pos, struct page, deferred_list);
	if (!trylock_page(page))
	    goto next;
	if (!split_huge_page(page))
	    split++;
	unlock_page(page);
next:
	put_page(page);
    }

    spin_lock_irqsave(&ds_queue->split_queue_lock, flags);
    list_splice_tail(&list, &ds_queue->split_queue);
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);

    return split;
}

struct page *get_meta_page(struct page *page)
{
    return &page[3];
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

bool region_for_toptier(huge_region_t *node)
{
    unsigned long hotness;

    hotness = MULTIPLIER * node->cur_hv / 10 + (10 - MULTIPLIER) * node->prev_hv / 10;
    return hotness >= htmm_thres_huge_hot;
}

static bool reach_cooling_thres(pginfo_t *pginfo, struct page *meta_page, bool hugepage)
{
    if (hugepage) {
	if (htmm_mode == HTMM_BASELINE)
	    return meta_page->total_accesses >= htmm_thres_cold;
	else 
	    return pginfo->nr_accesses >= htmm_thres_cold;
    }

    return pginfo->nr_accesses >= htmm_thres_cold;
}

static bool need_lru_cooling(struct mm_struct *mm,
			     struct page *page)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    unsigned long cur, next, now = jiffies;
    unsigned long nr_active_pages = 0;
    int nid;

    cur = memcg->htmm_next_cooling;
    if (time_before(now, cur))
	return false;
    
    for_each_node_state(nid, N_MEMORY) {
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
	nr_active_pages += lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);
    }

    memcg->nr_active_pages = nr_active_pages;
    if (nr_active_pages <= memcg->max_nr_dram_pages)
	return false;
    
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

/*
 * "huge" is true if the target meta is the third tail page of the huge page.
 * "huge" is false if the target meta is the information for huge region.
 */
unsigned long access_benefit(void *meta, bool huge)
{
    if (huge) {
	struct page *meta_page = (struct page *)meta;
	return meta_page->total_accesses * DELTA_CYCLES;
    } else {
	huge_region_t *node = (huge_region_t *)meta;
	return node->total_accesses * DELTA_CYCLES;
    }
}

unsigned long access_penalty(void *meta, bool huge)
{
    if (huge) {
	struct page *meta_page = (struct page *)meta;
	return (HPAGE_PMD_NR - meta_page->hot_utils) * htmm_thres_hot * DELTA_CYCLES;
    } else {
	huge_region_t *node = (huge_region_t *)meta;
	return (HPAGE_PMD_NR - node->hot_utils) * htmm_thres_hot * DELTA_CYCLES;
    }
}

/*
 * If the number of hot pages exceeds the DRAM limit,
 * access_penalty() needs additional penalty.
 */
unsigned long compensated_access_penalty(struct mem_cgroup *memcg, void *meta, bool huge)
{
    unsigned long ap = access_penalty(meta, huge);

    return ap * memcg->nr_active_pages / memcg->max_nr_dram_pages;
}

unsigned long translation_benefit(void *meta, bool huge)
{
    unsigned long benefits;
    unsigned long hot_utils, total_accesses;
    
    if (huge) {
	struct page *meta_page = (struct page *)meta;
	hot_utils = meta_page->hot_utils;
	total_accesses = meta_page->total_accesses;
    } else {
	huge_region_t *node = (huge_region_t *)meta;
	hot_utils = node->hot_utils;
	total_accesses = node->hot_utils;
    }

    if (hot_utils == 0)
	return 0;
   
    hot_utils = int_sqrt(hot_utils);
    benefits = total_accesses;
    benefits *= DRAM_ACCESS_CYCLES;
    benefits = ((hot_utils << 2) - 3) * benefits / (hot_utils << 2);
    /* correction may be neccessary */ 
    return benefits;
}

long cal_huge_hotness(struct mem_cgroup *memcg, void *meta, bool huge)
{
    return access_benefit(meta, huge) - compensated_access_penalty(memcg, meta, huge) + translation_benefit(meta, huge);
}

bool is_hot_huge_page(struct page *meta)
{
    unsigned long hotness;

    hotness = MULTIPLIER * meta->cur_hv / 10 + (10 - MULTIPLIER) * meta->prev_hv / 10;
    return hotness >= htmm_thres_huge_hot;
}

enum region_list hugepage_type(struct page *page)
{
    struct page *meta;
    struct mem_cgroup *memcg;
    VM_BUG_ON_PAGE(!PageTransHuge(page), page);
    
    memcg = page_memcg(compound_head(page));
    if (!memcg->htmm_enabled)
	return -1;

    meta = get_meta_page(page);
    if (is_hot_huge_page(meta))
	return HUGE_TOPTIER;
    else if (compensated_access_penalty(memcg, meta, true) < translation_benefit(meta, true))
	return HUGE_LOWERTIER;
    else
	return BASE_PAGES;
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
	
	/* insert node into mm's region_list */
	spin_lock(&mm->hri.lock);
	list_add_tail(&node->hr_entry, &mm->hri.region_list[BASE_PAGES]);
	spin_unlock(&mm->hri.lock);
    }

    if (!spin_trylock_irq(&node->lock))
	return;

    node->total_accesses++;
    if (newly_hot)
	node->hot_utils++;
    
    /* calculate hotness value */
    node->prev_hv = node->cur_hv;
    node->cur_hv = cal_huge_hotness(memcg, (void *)node, false);

    spin_unlock_irq(&node->lock);
}

static void move_page_to_active_lru(struct page *page)
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

static void __update_pte_pginfo(struct vm_area_struct *vma, pmd_t *pmd,
				unsigned long address)
{
    pte_t *pte, ptent;
    spinlock_t *ptl;
    pginfo_t *pginfo;
    struct page *page, *pte_page;
    
    pte = pte_offset_map_lock(vma->vm_mm, pmd, address, &ptl);
    ptent = *pte;
    if (!pte_present(ptent))
	goto pte_unlock;

    page = vm_normal_page(vma, address, ptent);
    if (!page || PageKsm(page))
	goto pte_unlock;

    if (!PageLRU(page))
	goto pte_unlock;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	goto pte_unlock;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	goto pte_unlock;

    //if (pginfo->nr_accesses <= htmm_thres_cold)
	pginfo->nr_accesses++;

    if (pginfo->nr_accesses >= htmm_thres_hot) {
	bool newly_hot = false;
	if (!PageActive(page)) {
	    move_page_to_active_lru(page);
	    newly_hot = true;
	}
	if (htmm_mode == HTMM_HUGEPAGE_OPT &&
	    transhuge_vma_suitable(vma, address & HPAGE_PMD_MASK)) {
	    update_huge_region(vma, address & HPAGE_PMD_MASK, true);
	}
    }

    if (reach_cooling_thres(pginfo, NULL, false) && need_lru_cooling(vma->vm_mm, page))
	set_lru_cooling(vma->vm_mm);

pte_unlock:
    pte_unmap_unlock(pte, ptl);
}

static void __update_pmd_pginfo(struct vm_area_struct *vma, pud_t *pud,
				unsigned long address)
{
    pmd_t *pmd, pmdval;
    spinlock_t *ptl;

    pmd = pmd_offset(pud, address);
    if (!pmd || pmd_none(*pmd))
	return;
    
    if (is_swap_pmd(*pmd))
	return;

    if (!pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))) {
	pmd_clear_bad(pmd);
	return;
    }

    pmdval = *pmd;
    if (pmd_trans_huge(pmdval) || pmd_devmap(pmdval)) {
	struct page *page, *meta_page;
	pginfo_t *pginfo;

	if (is_huge_zero_pmd(pmdval))
	    return;
	
	ptl = pmd_lock(vma->vm_mm, pmd);
	if (unlikely(!pmd_same(pmdval, *pmd)))
	    goto pmd_unlock;

	page = pmd_page(pmdval);
	if (!page || !PageLRU(page) || PageLocked(page))
	    goto pmd_unlock;

	if (compound_head(page) != page) {
	    printk("page is not compound_head\n");
	    goto pmd_unlock;
	}
	
	pginfo = get_compound_pginfo(page, address);
	meta_page = get_meta_page(page);

	pginfo->nr_accesses++;
	meta_page->total_accesses++;

	if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	    if (pginfo->nr_accesses == htmm_thres_hot)
		meta_page->hot_utils++;

	    /* if page is active, hotness will be updated by the cooling op */
	    if (!PageActive(page)) {
		struct mem_cgroup *memcg = page_memcg(page);

		BUG_ON(!memcg->htmm_enabled);

		meta_page->prev_hv = meta_page->cur_hv;
		meta_page->cur_hv = cal_huge_hotness(memcg, (void *)meta_page, true);

		if (is_hot_huge_page(meta_page))
		    move_page_to_active_lru(page);
	    }
	    
	} else { /* HTMM_BASELINE */
	    if (!PageActive(page) && meta_page->total_accesses >= htmm_thres_hot)
		move_page_to_active_lru(page);
	}

	if (reach_cooling_thres(pginfo, meta_page, true) &&
		need_lru_cooling(vma->vm_mm, page))
	    set_lru_cooling(vma->vm_mm);
	
pmd_unlock:
	spin_unlock(ptl);
	return;
    }
    /* base page */
    __update_pte_pginfo(vma, pmd, address);
}

static void __update_pginfo(struct vm_area_struct *vma, unsigned long address)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;

    pgd = pgd_offset(vma->vm_mm, address);
    if (pgd_none_or_clear_bad(pgd))
	return;
    
    p4d = p4d_offset(pgd, address);
    if (p4d_none_or_clear_bad(p4d))
	return;
    
    pud = pud_offset(p4d, address);
    if (pud_none_or_clear_bad(pud))
	return;
    
    __update_pmd_pginfo(vma, pud, address);
}

void update_pginfo(pid_t pid, unsigned long address)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_task(pid_struct, PIDTYPE_PID);
    struct mm_struct *mm = p ? p->mm : NULL;
    struct vm_area_struct *vma; 

    if (!mm)
	goto put_task;

    if (!mmap_read_trylock(mm))
	goto put_task;

    vma = find_vma(mm, address);
    if (unlikely(!vma))
	goto mmap_unlock;
    
    if (!vma->vm_mm || !vma_migratable(vma) ||
	(vma->vm_file && (vma->vm_flags & (VM_READ | VM_WRITE)) == (VM_READ)))
	goto mmap_unlock;
    
    __update_pginfo(vma, address);
mmap_unlock:
    mmap_read_unlock(mm);
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
    node = kmem_cache_zalloc(huge_region_cachep, GFP_KERNEL);
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
    kmem_cache_free(huge_region_cachep, node);
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
