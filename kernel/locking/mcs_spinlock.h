/*
 * MCS lock defines
 *
 * This file contains the main data structure and API definitions of MCS lock.
 *
 * The MCS lock (proposed by Mellor-Crummey and Scott) is a simple spin-lock
 * with the desirable properties of being fair, and with each cpu trying
 * to acquire the lock spinning on a local variable.
 * It avoids expensive cache bouncings that common test-and-set spin-lock
 * implementations incur.
 */
#ifndef __LINUX_MCS_SPINLOCK_H
#define __LINUX_MCS_SPINLOCK_H

#include <asm/mcs_spinlock.h>
#include <linux/osq_lock.h>

struct mcs_spinlock {
	struct mcs_spinlock *next;
	int locked; /* 1 if lock acquired */
	int count; /* nesting count, see qspinlock.c */
};

/*
 * Note: the smp_load_acquire/smp_store_release pair is not
 * sufficient to form a full memory barrier across
 * cpus for many architectures (except x86) for mcs_unlock and mcs_lock.
 * For applications that need a full barrier across multiple cpus
 * with mcs_unlock and mcs_lock pair, smp_mb__after_unlock_lock() should be
 * used after mcs_lock.
 */

/*
 * In order to acquire the lock, the caller should declare a local node and
 * pass a reference of the node to this function in addition to the lock.
 * If the lock has already been acquired, then this will proceed to spin
 * on this node->locked until the previous lock holder sets the node->locked
 * in mcs_spin_unlock().
 *
 * We don't inline mcs_spin_lock() so that perf can correctly account for the
 * time spent in this lock function.
 */
static inline
void mcs_spin_lock(struct mcs_spinlock **lock, struct mcs_spinlock *node)
{
	struct mcs_spinlock *prev;

	/* Init node */
	node->locked = 0;
	node->next   = NULL;

	prev = xchg(lock, node);
	if (likely(prev == NULL)) {
		/*
		 * Lock acquired, don't need to set node->locked to 1. Threads
		 * only spin on its own node->locked value for lock acquisition.
		 * However, since this thread can immediately acquire the lock
		 * and does not proceed to spin on its own node->locked, this
		 * value won't be used. If a debug mode is needed to
		 * audit lock status, then set node->locked value here.
		 */
		return;
	}
	/*
	 * BluesMan: Change from ACCESS_ONCE to WRITE_ONCE
	 *
	 */
	WRITE_ONCE(prev->next, node);

	/* Wait until the lock holder passes the lock down. */
	arch_mcs_spin_lock_contended(&node->locked);
}

/*
 * Releases the lock. The caller should pass in the corresponding node that
 * was used to acquire the lock.
 */
static inline
void mcs_spin_unlock(struct mcs_spinlock **lock, struct mcs_spinlock *node)
{
	struct mcs_spinlock *next = READ_ONCE(node->next);

	if (likely(!next)) {
		/*
		 * Release the lock by setting it to NULL
		 */
		if (likely(cmpxchg(lock, node, NULL) == node))
			return;
		/* Wait until the next pointer is set */
		while (!(next = READ_ONCE(node->next))) {
			/* TEST ONLY : Disable cpu_relax_lowlatency */
			arch_mutex_cpu_relax();
			/*cpu_relax_lowlatency(); */
		}
	}

	/* Pass lock to next waiter. */
	arch_mcs_spin_unlock_contended(&next->locked);
}

/*
 * Cancellable version of the MCS lock above.
 *
 * Intended for adaptive spinning of sleeping locks:
 * mutex_lock()/rwsem_down_{read,write}() etc.
 */

struct optimistic_spin_node {
	struct optimistic_spin_node *next, *prev;
	int locked; /* 1 if lock acquired */
	int cpu; /* encoded CPU # value */
};

extern bool osq_lock(struct optimistic_spin_queue *lock);
extern void osq_unlock(struct optimistic_spin_queue *lock);

#endif /* __LINUX_MCS_SPINLOCK_H */
