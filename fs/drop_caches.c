// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the manual drop-all-pagecache function
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/sysctl.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/gfp.h>
#include "internal.h"

/* A global variable is a bit ugly, but it keeps the code simple */
int sysctl_drop_caches;

int sysctl_auto_clear_ram = 1;
int sysctl_auto_clear_ram_interval = 300;
static struct task_struct *auto_clear_ram_task;

static void drop_pagecache_sb(struct super_block *sb, void *unused)
{
	struct inode *inode, *toput_inode = NULL;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		spin_lock(&inode->i_lock);
		/*
		 * We must skip inodes in unusual state. We may also skip
		 * inodes without pages but we deliberately won't in case
		 * we need to reschedule to avoid softlockups.
		 */
		if ((inode->i_state & (I_FREEING|I_WILL_FREE|I_NEW)) ||
		    (inode->i_mapping->nrpages == 0 && !need_resched())) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		spin_unlock(&sb->s_inode_list_lock);

		invalidate_mapping_pages(inode->i_mapping, 0, -1);
		iput(toput_inode);
		toput_inode = inode;

		cond_resched();
		spin_lock(&sb->s_inode_list_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(toput_inode);
}

int drop_caches_sysctl_handler(struct ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write) {
		static int stfu;

		if (sysctl_drop_caches & 1) {
			iterate_supers(drop_pagecache_sb, NULL);
			count_vm_event(DROP_PAGECACHE);
		}
		if (sysctl_drop_caches & 2) {
			drop_slab();
			count_vm_event(DROP_SLAB);
		}
		if (!stfu) {
			pr_info("%s (%d): drop_caches: %d\n",
				current->comm, task_pid_nr(current),
				sysctl_drop_caches);
		}
		stfu |= sysctl_drop_caches & 4;
	}
	return 0;
}


static int auto_clear_ram_thread(void *data)
{
	long interval;

	set_user_nice(current, 19);

	while (!kthread_should_stop()) {
		interval = sysctl_auto_clear_ram_interval;
		if (interval <= 0)
			interval = 300;
		schedule_timeout_interruptible(HZ * interval);
		if (kthread_should_stop())
			break;
		if (!sysctl_auto_clear_ram)
			continue;
		iterate_supers(drop_pagecache_sb, NULL);
		drop_slab();
	}
	return 0;
}

static int __init auto_clear_ram_init(void)
{
	auto_clear_ram_task = kthread_run(auto_clear_ram_thread, NULL,
					"auto_clear_ram");
	if (IS_ERR(auto_clear_ram_task))
		auto_clear_ram_task = NULL;
	return 0;
}
late_initcall(auto_clear_ram_init);
