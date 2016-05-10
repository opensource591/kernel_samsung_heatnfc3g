/**
 * COPYRIGHT (C)  SAMSUNG Electronics CO., LTD (Suwon, Korea). 2009
 * All rights are reserved. Reproduction and redistiribution in whole or
 * in part is prohibited without the written consent of the copyright owner.
 */

/**
 * Trace kmem
 *
 * @autorh kumhyun.cho@samsung.com
 * @since 2014.02.14
 */

#include <linux/mm.h>
#include <linux/swap.h>
#include <trace/mark.h>

#define K(x) ((x) << (PAGE_SHIFT - 10))

#define TRACE_MARK_AVAIL         "avail"
#define TRACE_MARK_CACHED        "cached"
#define TRACE_MARK_SWAP          "swap"
#define TRACE_MARK_ACTIVE_ANON   "act_anon"
#define TRACE_MARK_INACTIVE_ANON "inact_anon"
#define TRACE_MARK_ACTIVE_FILE   "act_file"
#define TRACE_MARK_INACTIVE_FILE "inact_file"

#define TRACE_MARK_LMK_CNT       "lmk_cnt"

static struct trace_kmem_mark_entry {
	const char* str;
	int enable;
} trace_kmem_mark_pages[] = {
	{ "free_pages", 1 },        /* NR_FREE_PAGES */
// 	{ "lru_base", 0},           /* NR_LRU_BASE */
	{ "inact_anon", 1 },        /* NR_INACTIVE_ANON */
	{ "act_anon", 1 },          /* NR_ACTIVE_ANON */
	{ "inact_file", 1 },        /* NR_INACTIVE_FILE */
	{ "act_file", 1 },          /* NR_ACTIVE_FILE */
	{ "unevict", 1 },           /* NR_UNEVICTABLE */
	{ "mlock", 1 },             /* NR_MLOCK */
	{ "anon", 1 },              /* NR_ANON_PAGES */
	{ "file_mapped", 1 },       /* NR_FILE_MAPPED */
	{ "file_pages", 1 },        /* NR_FILE_PAGES */
	{ "file_dirty", 1 },        /* NR_FILE_DIRTY */
	{ "writeback", 1 },         /* NR_WRITEBACK */
	{ "slab_recl", 1 },         /* NR_SLAB_RECLAIMABLE */
	{ "slab_unrecl", 1 },       /* NR_SLAB_UNRECLAIMABLE */
	{ "pagetable", 1 },         /* NR_PAGETABLE */
	{ "kernel statk", 1 },      /* NR_KERNEL_STACK */
	{ "unstable_nfs", 1 },      /* NR_UNSTABLE_NFS */
	{ "bounce", 1 },            /* NR_BOUNCE */
	{ "vmscan_write", 1 },      /* NR_VMSCAN_WRITE */
	{ "vmscan_imm", 1 },        /* NR_VMSCAN_IMMEDIATE */
	{ "wb temp", 1 },           /* NR_WRITEBACK_TEMP */
	{ "isol_anon", 1 },         /* NR_ISOLATED_ANON */
	{ "isol_file", 1 },         /* NR_ISOLATED_FILE */
	{ "shmem", 1 },             /* NR_SHMEM */
	{ "dirtied", 1 },           /* NR_DIRTIED */
	{ "written", 1 },           /* NR_WRITTEN */
#ifdef CONFIG_NUMA
	{ "hit", 1 },               /* NUMA_HIT */
	{ "miss", 1 },              /* NUMA_MISS */
	{ "foreign", 1 },           /* NUMA_FOREIGN */
	{ "inter_hit", 1 },         /* NUMA_INTERLEAVE_HIT */
	{ "local", 1 },             /* NUMA_LOCAL */
	{ "other", 1 },             /* NUMA_OTHER */
#endif
	{ "anon_tr_huge", 1 },      /* NR_ANON_TRANSPARENT_HUGEPAGES */
	{ "free_cma", 1 },          /* NR_FREE_CMA_PAGES */
// 	{ "cma_base", 0},           /* NR_STAT_CMA_BASE */
	{ "cma_inact_anon", 1 },    /* NR_CMA_INACTIVE_ANON */
	{ "cma_act_anon", 1 },      /* NR_CMA_ACTIVE_ANON */
	{ "cma_inact_file", 1 },    /* NR_CMA_INACTIVE_FILE */
	{ "cma_act_file", 1 },      /* NR_CMA_ACTIVE_FILE */
	{ "cma_unevict", 1 },       /* NR_CMA_UNEVICTABLE */
	{ "cma_contig", 1 },        /* NR_CMA_CONTIG_PAGES */
};

static unsigned long avail[2];
static unsigned long cached[2];
static unsigned long swap[2];
static unsigned long pages[2][NR_VM_ZONE_STAT_ITEMS];

static unsigned long lmk_count[2];

// static DEFINE_SPINLOCK(kmem_stat_lock);

void trace_mark_kmem_state_page(enum zone_stat_item item) 
{
	if (!trace_kmem_mark_pages[item].enable) return;

	if (pages[0][item] != pages[1][item])
		trace_mark_int(0, trace_kmem_mark_pages[item].str, K(pages[0][item]), "");
}

void trace_mark_kmem_stat(enum zone_stat_item item)
{
	int i;
	struct sysinfo si;

	// spin_lock(&kmem_stat_lock);

	/**
	 * page state
	 */
	for (i = NR_FREE_PAGES; i < NR_VM_ZONE_STAT_ITEMS; i++)
		pages[0][i] = global_page_state(i);

	if (item < NR_VM_ZONE_STAT_ITEMS) {
		trace_mark_kmem_state_page(item);
	} else {
		for (i = NR_FREE_PAGES; i < NR_VM_ZONE_STAT_ITEMS; i++)
			trace_mark_kmem_state_page(item);
	}

	/**
	 * Free and cached
	 */
	if ((cached[0] = (pages[0][NR_FILE_PAGES] - total_swapcache_pages())) < 0)
		cached[0] = 0;

	if (cached[0] != cached[1])
		trace_mark_int(0, TRACE_MARK_CACHED, K(cached[0]), "");

	if ((avail[0] = pages[0][NR_FREE_PAGES] + cached[0]) != avail[1])
		trace_mark_int(0, TRACE_MARK_AVAIL, K(avail[0]), "");

	/**
	 * Swap
	 */
	si_swapinfo(&si);

	if ((swap[0] = si.totalswap - si.freeswap) != swap[1])
		trace_mark_int(0, TRACE_MARK_SWAP, K(swap[0]), "");

	/**
	 * Backup
	 */
	avail[1] = avail[0];
	cached[1] = cached[0];
	swap[1] = swap[0];
	memcpy(&pages[1], &pages[0], sizeof(pages[0]));

	// spin_unlock(&kmem_stat_lock);
}

void trace_mark_lmk_count(unsigned long count) {
	if ((lmk_count[0] = count) != lmk_count[1]) {
		trace_mark_int(0, TRACE_MARK_LMK_CNT, count, "");

		lmk_count[1] = lmk_count[0];
	}
}
