/*
 * XXXXXXXXXXXXXXXXXXXXXXXXXX
 * REMOVED
 *
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/freezer.h>
#include <linux/htmm.h>
#include <linux/delay.h>


#include <asm/pgtable.h>

struct task_struct *kcoolingd = NULL;
static DEFINE_SPINLOCK(kcoolingd_lock);
static LIST_HEAD(kcoolingd_memcg_list);
static DECLARE_WAIT_QUEUE_HEAD(kcoolingd_wait);

void wakeup_kcoolingd(void)
{
    if (!waitqueue_active(&kcoolingd_wait))
	return;

    wake_up_interruptible(&kcoolingd_wait);
}

void register_memcg_for_cooling(struct mem_cgroup *memcg)
{
    struct mem_cgroup *tmp;

    spin_lock(&kcoolingd_lock);
    list_for_each_entry(tmp, &kcoolingd_memcg_list, cooling_entry) {
	if (tmp == memcg)
	    goto reg_unlock;
    }
    list_add_tail(&memcg->cooling_entry, &kcoolingd_memcg_list);
reg_unlock:
    spin_unlock(&kcoolingd_lock);

    wakeup_kcoolingd();
}

struct mem_cgroup *get_next_memcg(void)
{

    struct mem_cgroup *memcg = NULL;
    
    spin_lock(&kcoolingd_lock);
    if (!list_empty(&kcoolingd_memcg_list)) {
	memcg = list_first_entry(&kcoolingd_memcg_list, typeof(*memcg), cooling_entry);
	list_del(&memcg->cooling_entry);
    }
    spin_unlock(&kcoolingd_lock);

    return memcg;
}

void kcoolingd_try_to_sleep(void)
{
    DEFINE_WAIT(wait);

    if (freezing(current) || kthread_should_stop())
	return;
    
    prepare_to_wait(&kcoolingd_wait, &wait, TASK_INTERRUPTIBLE);
    if (list_empty(&kcoolingd_memcg_list)) {
	if (!kthread_should_stop())
	    schedule();
    }

    finish_wait(&kcoolingd_wait, &wait);
}

struct mm_struct *get_next_mm(struct mem_cgroup *memcg,
				     unsigned long count)
{
    struct mm_struct *target = NULL, *mm, *tmp;

    spin_lock(&memcg->htmm_mm_list_lock);
    list_for_each_entry_safe(mm, tmp, &memcg->htmm_mm_list, memcg_entry) {
	if (!mm->htmm_enabled) {
	    list_del(&mm->memcg_entry);
	    continue;
	}
	
	if (mm->htmm_count < count) {
	    target = mm;
	    target->htmm_count = count;
	    break;
	}
    }
    spin_unlock(&memcg->htmm_mm_list_lock);
    
    return target;
}

void update_base_lru(struct vm_area_struct *vma, pginfo_t *pginfo, struct page *page)
{
    unsigned long cur_idx;
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);

    cur_idx = pginfo->nr_accesses;
    cur_idx *= 512;
    cur_idx = get_idx(cur_idx);
    
    if (cur_idx >= (memcg->active_threshold - 1)) {
	if (!PageActive(page))
	    move_page_to_active_lru(page);
    } else {
	if (PageActive(page))
	    move_page_to_inactive_lru(page);
    }
}

void update_huge_lru(struct vm_area_struct *vma, struct page *page)
{
    struct page *meta;
    unsigned long cur_idx;
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);

    meta = get_meta_page(page);
    cur_idx = meta->total_accesses;
    cur_idx += meta->hot_utils * htmm_util_weight / 10;
    cur_idx = get_idx(cur_idx);

    if (cur_idx >= (memcg->active_threshold - 1)) {
	if (!PageActive(page))
	    move_page_to_active_lru(page);
    } else {
	if (PageActive(page))
	    move_page_to_inactive_lru(page);
    }
}

void __cooling_pginfo_pte(struct vm_area_struct *vma, pmd_t *pmd,
				 unsigned long start, unsigned long end)
{
    pte_t *pte, pteval;
    spinlock_t *ptl;
    struct page *page, *pte_page;
    pginfo_t *pginfo;

    pte = pte_offset_map_lock(vma->vm_mm, pmd, start, &ptl);
    if (!pte)
	return;

    do {
pte_retry:
	pteval = *pte;
	if (!pte_present(pteval))
	    continue;
	
	page = vm_normal_page(vma, start, pteval);
	if (!page || PageKsm(page) || PageLocked(page))
	    continue;

	pte_page = virt_to_page((unsigned long)pte);
	if (!PageHtmm(pte_page))
	    continue;

	pginfo = get_pginfo_from_pte(pte);
	if (!pginfo)
	    continue;

	/*if (!trylock_page(page))
	    lock_page(page);

	if (!pte_same(pteval, *pte)) {
	    unlock_page(page);
	    goto pte_retry;
	}*/

	check_base_cooling(pginfo, page, false);
	//unlock_page(page);
	//update_base_lru(vma, pginfo, page);
    } while (pte++, start += PAGE_SIZE, start != end);

    pte_unmap_unlock(--pte, ptl);
}

void __cooling_pginfo_pmd(struct vm_area_struct *vma, pud_t *pud,
				 unsigned long start, unsigned long end)
{
    pmd_t *pmd, pmdval;
    unsigned long next;

    pmd = pmd_offset(pud, start);
    do {
	next = pmd_addr_end(start, end);
pmd_retry:
	if (!pmd || pmd_none(*pmd))
	    continue;

	if (is_swap_pmd(*pmd))
	    continue;

	if (!pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))) {
	    pmd_clear_bad(pmd);
	    continue;
	}

	pmdval = *pmd;
	if (pmd_trans_huge(pmdval) || pmd_devmap(pmdval)) {
	    struct page *page;

	    if (is_huge_zero_pmd(pmdval))
		continue; 

	    page = pmd_page(pmdval);
	    if (!page || PageLocked(page))
		continue;

	    /*if (!trylock_page(page))
		lock_page(page);
	    
	    if (!pmd_same(pmdval, *pmd)) {
		unlock_page(page);
		goto pmd_retry;
		continue;
	    }*/

	    check_transhuge_cooling((void *)&start, page, false);
	    //unlock_page(page);
	    //update_huge_lru(vma, page);
	    continue;
	}
	
	__cooling_pginfo_pte(vma, pmd, start, next);
    } while (pmd++, start = next, start != end);
}

void __cooling_pginfo_pud(struct vm_area_struct *vma, p4d_t *p4d,
				 unsigned long start, unsigned long end)
{
    pud_t *pud;
    unsigned long next;
    
    pud = pud_offset(p4d, start);
    do {
	next = pud_addr_end(start, end);
	if (pud_none_or_clear_bad(pud))
	    continue;
	__cooling_pginfo_pmd(vma, pud, start, next);
    } while (pud++, start = next, start != end);
}

void __cooling_pginfo_p4d(struct vm_area_struct *vma, pgd_t *pgd,
				 unsigned long start, unsigned long end)
{
    p4d_t *p4d;
    unsigned long next;

    p4d = p4d_offset(pgd, start);
    do {
	next = p4d_addr_end(start, end);
	if (p4d_none_or_clear_bad(p4d))
	    continue;
	__cooling_pginfo_pud(vma, p4d, start, next);
    } while (p4d++, start = next, start != end);
}

void __cooling_pginfo(struct vm_area_struct *vma,
			     unsigned long start, unsigned long end)
{
    pgd_t *pgd;
    unsigned long next;

    pgd = pgd_offset(vma->vm_mm, start);
    do {
	next = pgd_addr_end(start, end);
	if (pgd_none_or_clear_bad(pgd))
	    continue;
	__cooling_pginfo_p4d(vma, pgd, start, next);
    } while (pgd++, start = next, start != end);
}

void cooling_pginfo(struct mm_struct *mm)
{
    struct vm_area_struct *vma;
    
    vma = mm->mmap;
    for ( ; vma; vma = vma->vm_next) {
	if (!vma_migratable(vma) || is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP))
	    continue;

	if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
	    continue;

	__cooling_pginfo(vma, vma->vm_start, vma->vm_end);
    }
}

void end_cooling(struct mem_cgroup *memcg)
{
    int nid, i;
    unsigned long nr_active = 0;

    spin_lock(&memcg->access_lock);
    if (htmm_mode == HTMM_HUGEPAGE_OPT) {
	for (i = 11; i > 0; i--) {
	    nr_active += memcg->hotness_hg[i];
	    if (nr_active >= memcg->max_nr_dram_pages)
		break;
	}
    } else {
	for (i = 11; i > 0; i--) {
	    nr_active += memcg->access_map[i];
	    if (nr_active >= memcg->max_nr_dram_pages)
		break;
	}
    }
    spin_unlock(&memcg->access_lock);

    if (i != 11)
	i++;
    
    if (memcg->active_threshold > i + 1) {
	printk("cooling does not halve the access counts... \n");
	memcg->active_threshold--;
    }
    else if (memcg->active_threshold < i) {
	memcg->active_threshold = i;
//	set_lru_adjusting(memcg, false);	
    }
    else
	memcg->active_threshold = i;

    for_each_node_state(nid, N_MEMORY) {
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];
	WRITE_ONCE(pn->need_cooling, false);
    }
}

void do_cooling_for_memcg(struct mem_cgroup *memcg)
{
    struct mm_struct *mm;

    memcg->count++;
    printk("cooling start...\n");
    for ( ; ; ) {
	mm = get_next_mm(memcg, memcg->count);
	if (!mm)
	    break;

	if (!mmap_read_trylock(mm)) {
	    /* try this mm next time */
	    spin_lock(&memcg->htmm_mm_list_lock);
	    mm->htmm_count--;
	    list_move_tail(&mm->memcg_entry, &memcg->htmm_mm_list);
	    spin_unlock(&memcg->htmm_mm_list_lock);
	    continue;
	}
	
	cooling_pginfo(mm);
	mmap_read_unlock(mm);
    } 
    /* end cooling */
   // end_cooling(memcg);
}

static int _kcoolingd(void *p)
{
    struct mem_cgroup *memcg;

    while (!kthread_should_stop()) {

try_to_sleep:
	memcg = get_next_memcg();
	if (!memcg) {
	    msleep_interruptible(1000);
	    //kcoolingd_try_to_sleep();
	    continue;
	}

	if (!memcg->htmm_enabled)
	    goto try_to_sleep;
	
	do_cooling_for_memcg(memcg);
	memcg->cooling_clock++;
    }

    return 0;
}

static int kcoolingd_run(void)
{
    int err = 0;
    
    if (!kcoolingd) {
	kcoolingd = kthread_run(_kcoolingd, NULL, "kcoolingd");
	if (IS_ERR(kcoolingd)) {
	    err = PTR_ERR(kcoolingd);
	    kcoolingd = NULL;
	}
    }

    return err;
}

int kcoolingd_init(void)
{
    if (kcoolingd)
	return 0;
    
    return kcoolingd_run();
}

void kcoolingd_exit(void)
{
    kthread_stop(kcoolingd);
}
