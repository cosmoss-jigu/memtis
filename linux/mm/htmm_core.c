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

void __prep_transhuge_page_for_htmm(struct page *page)
{
    int i, idx, offset;
    pginfo_t pginfo = { 0, };

    /* third tail page */
    page[3].hot_utils = 0;
    page[3].total_accesses = 0;
    page[3].cur_hv = 0;
    page[3].prev_hv = 0;

    /* fourth~ tail pages */
    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 8;
	offset = i % 8;
	
	page[idx].compound_pginfo[offset] = pginfo;
    }
}

void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
				  struct page *page)
{
    prep_transhuge_page(page);

    if (vma->vm_mm->htmm_enabled)
	__prep_transhuge_page_for_htmm(page);
}

pginfo_t *get_compound_pginfo(struct page *page, unsigned long address)
{
    int idx, offset;
    VM_BUG_ON_PAGE(!PageCompound(page), page);
    
    idx = 4 + (address & ~HPAGE_PMD_MASK) / 8;
    offset = (address & ~HPAGE_PMD_MASK) % 8;

    return &(page[idx].compound_pginfo[offset]);
}

static struct page *get_meta_page(struct page *page)
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

static void set_lru_cooling(struct mm_struct *mm, int nid)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct mem_cgroup_per_node *pn;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    pn = memcg->nodeinfo[nid];
    if (!pn)
	return;
    
    WRITE_ONCE(pn->need_cooling, true);
}

static bool is_hot_page(struct page *page, bool huge)
{

    return false;
}

static void move_page_to_active_lru(struct page *page)
{
    struct lruvec *lruvec;
    LIST_HEAD(l_active);

    if (PageActive(page))
	return;

    lruvec = mem_cgroup_page_lruvec(page);
    
    spin_lock_irq(&lruvec->lru_lock);
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

    pginfo->nr_accesses++;
    if (pginfo->nr_accesses >= htmm_thres_hot) {
	//printk("page %lx is hot... nr_accesses%lu\n", address, pginfo->nr_accesses);
	if (!PageActive(page))
	    move_page_to_active_lru(page);
	if (transhuge_vma_suitable(vma, address & HPAGE_PMD_MASK)) {
	    // TODO huge region
	}
    }

    if (pginfo->nr_accesses >= htmm_thres_cold)
	set_lru_cooling(vma->vm_mm, page_to_nid(page));

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

    if (!pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))) {
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
	
	pginfo = get_compound_pginfo(page, address);
	meta_page = get_meta_page(page);

	pginfo->nr_accesses++;
	meta_page->total_accesses++;
	if (is_hot_page(page, true)) {
	    if (!PageActive(page)) {
		move_page_to_active_lru(page);
		meta_page->hot_utils++;
	    }
	}
	
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
