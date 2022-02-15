/*
 *
 */
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/memcontrol.h>
#include <linux/delay.h>
#include <linux/node.h>
#include <linux/htmm.h>

#include "internal.h"

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

static unsigned long get_nr_lru_pages_node(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec;
    unsigned long nr_pages = 0;
    enum lru lru;

    lruvec = mem_cgroup_lruvec = (lruvec, memcg, pgdat);

    for_each_lru(lru)
	nr_pages = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    
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
    nr_lru_pages = get_nr_lru_pages_node(memcg, pgdat->node_id);

    if (nr_lru_pages > max_nr_base_pages + HTMM_MIN_FREE_PAGES) {
	int target_nid = next_demotion_node(pgdat->node_id);
	pg_data_t *target_pgdat;
	
	if (target_nid == NUMA_NO_NODE)
	    return false;

	target_pgdat= NODE_DATA(target_nid);
	if (need_lowertier_promotion(target_pgdat, memcg))
	    return true;
    }

    return false;
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

	printk("pn is not null\n");

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


	/* wait 50 ms */
	msleep_interruptible(50);
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
	
	
	msleep_interruptible(100);
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
