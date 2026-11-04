/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compatibility shims for building EROFS on Linux 4.4.
 *
 * Only helpers that are missing from 4.4 and can be provided without
 * changing semantics live here; anything else is adapted in place.
 */
#ifndef __EROFS_COMPAT_H
#define __EROFS_COMPAT_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/bio.h>
#include <linux/gfp.h>
#include <linux/overflow.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/processor.h>

/* super_block flags were renamed from MS_* to SB_* in Linux 4.15 */
#ifndef SB_RDONLY
#define SB_RDONLY	MS_RDONLY
#define SB_NOATIME	MS_NOATIME
#define SB_POSIXACL	MS_POSIXACL
#define SB_ACTIVE	MS_ACTIVE
#endif

static inline bool sb_rdonly(const struct super_block *sb)
{
	return sb->s_flags & SB_RDONLY;
}

#ifndef page_ref_count
#define page_ref_count(page)	page_count(page)
#endif

/* simplified atomic_cond_read_relaxed() (Linux 4.17+): spin until @cond */
#ifndef atomic_cond_read_relaxed
#define atomic_cond_read_relaxed(v, c)					\
({									\
	int VAL;							\
									\
	for (;;) {							\
		VAL = atomic_read(v);					\
		if (c)							\
			break;						\
		cpu_relax();						\
	}								\
	VAL;								\
})
#endif

/* clear_and_wake_up_bit() was introduced in Linux 4.17 */
static inline void clear_and_wake_up_bit(int bit, void *word)
{
	clear_bit_unlock(bit, word);
	/* See wake_up_bit() for which memory barrier you need to use. */
	smp_mb__after_atomic();
	wake_up_bit(word, bit);
}

/* block layer: no bio_set_dev() and no bi_opf in 4.4 */
#ifndef bio_set_dev
#define bio_set_dev(bio, bdev)	((bio)->bi_bdev = (bdev))
#endif

#ifndef readahead_gfp_mask
static inline gfp_t readahead_gfp_mask(struct address_space *x)
{
	return mapping_gfp_mask(x) | __GFP_NORETRY | __GFP_NOWARN;
}
#endif

/*
 * kvmalloc() family (Linux 4.12+): try the slab first without hammering
 * reclaim, and fall back to vmalloc for larger requests.
 */
static inline void *kvmalloc(size_t size, gfp_t flags)
{
	void *p;

	if (size <= PAGE_SIZE)
		return kmalloc(size, flags);

	p = kmalloc(size, (flags | __GFP_NOWARN | __GFP_NORETRY) &
			  ~__GFP_NOFAIL);
	if (p)
		return p;
	return __vmalloc(size, flags | __GFP_HIGHMEM, PAGE_KERNEL);
}

static inline void *kvzalloc(size_t size, gfp_t flags)
{
	return kvmalloc(size, flags | __GFP_ZERO);
}

static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return kvmalloc(n * size, flags);
}

#endif /* __EROFS_COMPAT_H */
