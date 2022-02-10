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

    spin_lock(&pgdat->kmigraterd_lock);
    list_for_each_entry(mz, &pgdat->kmigraterd_head, kmigraterd_list) {
	if (mz == pn) {
	    list_del(&pn->kmigraterd_list);
	    break;
	}
    }
    spin_unlock(&pgdat->kmigraterd_lock);
}

static struct mem_cgroup_per_node *next_memcg_cand(pg_data_t *pgdat)
{
    struct mem_cgroup_per_node *pn;

    spin_lock(&pgdat->kmigraterd_lock);
    if (!list_empty(&pgdat->kmigraterd_head)) {
	pn = list_entry((&pgdat->kmigraterd_head)->next, typeof(*pn), kmigraterd_list);
	list_move_tail(&pn->kmigraterd_list, &pgdat->kmigraterd_head);
    }
    else
	pn = NULL;
    spin_unlock(&pgdat->kmigraterd_lock);

    return pn;
}

static int kmigraterd_demotion(pg_data_t *pgdat)
{
    do_set_cpus_allowed(pgdat->kmigraterd, cpumask_of_node(pgdat->node_id));

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


	    continue;
	}

	/* demotes inactive lru pages */

	/* performs cooling */

    }
    return 0;
}

static int kmigraterd_promotion(pg_data_t *pgdat)
{
    int target_nid;
    const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

    if (!cpumask_empty(cpumask))
	do_set_cpus_allowed(pgdat->kmigraterd, cpumask);

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

	/* promotes hot pages to fast memory node */

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
    if (pgdat->kmigraterd)
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
