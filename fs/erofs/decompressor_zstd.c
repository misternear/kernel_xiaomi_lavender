// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zstandard decompression support for EROFS (Linux 4.4 backport).
 *
 * The on-disk format and the overall design follow the upstream
 * implementation (fs/erofs/decompressor_zstd.c, Linux 6.10), but this
 * variant is built on top of the legacy in-kernel zstd API (ZSTD_*) found
 * in 4.x kernels, and on the 4.4 EROFS backport in this tree.
 *
 * Compressed data of a pcluster is first copied to a contiguous per-stream
 * buffer (which also makes in-place I/O trivially safe since zstd cannot
 * decompress over its own input), then decoded page by page directly into
 * the destination pages.
 */
#include <linux/module.h>
#include <linux/log2.h>
#include <linux/zstd.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include "compress.h"

struct z_erofs_zstd {
	struct z_erofs_zstd *next;
	/* sink for destination pages which nobody asked for */
	u8 bounce[PAGE_SIZE];
	void *wksp;		/* ZSTD_DStream workspace */
	size_t wkspsz;
	void *inbuf;		/* contiguous copy of the compressed data */
	size_t inbufsz;
};

static DEFINE_SPINLOCK(z_erofs_zstd_lock);
static DEFINE_MUTEX(z_erofs_zstd_init_mutex);
static unsigned int z_erofs_zstd_max_dictsize;
static unsigned int z_erofs_zstd_nstrms, z_erofs_zstd_avail_strms;
static struct z_erofs_zstd *z_erofs_zstd_head;
static DECLARE_WAIT_QUEUE_HEAD(z_erofs_zstd_wq);

module_param_named(zstd_streams, z_erofs_zstd_nstrms, uint, 0444);

static struct z_erofs_zstd *z_erofs_zstd_get_strm(void)
{
	struct z_erofs_zstd *strm;

	for (;;) {
		spin_lock(&z_erofs_zstd_lock);
		strm = z_erofs_zstd_head;
		if (strm) {
			z_erofs_zstd_head = strm->next;
			strm->next = NULL;
			spin_unlock(&z_erofs_zstd_lock);
			return strm;
		}
		spin_unlock(&z_erofs_zstd_lock);
		wait_event(z_erofs_zstd_wq, READ_ONCE(z_erofs_zstd_head));
	}
}

static void z_erofs_zstd_put_strm(struct z_erofs_zstd *strm)
{
	spin_lock(&z_erofs_zstd_lock);
	strm->next = z_erofs_zstd_head;
	z_erofs_zstd_head = strm;
	spin_unlock(&z_erofs_zstd_lock);
	wake_up(&z_erofs_zstd_wq);
}

static void z_erofs_zstd_free_strms(void)
{
	struct z_erofs_zstd *strm;

	for (;;) {
		spin_lock(&z_erofs_zstd_lock);
		strm = z_erofs_zstd_head;
		if (strm)
			z_erofs_zstd_head = strm->next;
		spin_unlock(&z_erofs_zstd_lock);
		if (!strm)
			break;
		vfree(strm->wksp);
		vfree(strm->inbuf);
		kfree(strm);
		--z_erofs_zstd_avail_strms;
	}
}

void z_erofs_zstd_exit(void)
{
	mutex_lock(&z_erofs_zstd_init_mutex);
	z_erofs_zstd_free_strms();
	mutex_unlock(&z_erofs_zstd_init_mutex);
}

static int z_erofs_zstd_init(void)
{
	/* by default, use # of possible CPUs instead */
	if (!z_erofs_zstd_nstrms)
		z_erofs_zstd_nstrms = num_possible_cpus();

	for (; z_erofs_zstd_avail_strms < z_erofs_zstd_nstrms;
	     ++z_erofs_zstd_avail_strms) {
		struct z_erofs_zstd *strm;

		strm = kzalloc(sizeof(*strm), GFP_KERNEL);
		if (!strm) {
			z_erofs_zstd_free_strms();
			return -ENOMEM;
		}
		z_erofs_zstd_put_strm(strm);
	}
	return 0;
}

int z_erofs_load_zstd_config(struct super_block *sb,
			     struct erofs_super_block *dsb,
			     struct z_erofs_zstd_cfgs *zstd, int size)
{
	unsigned int dict_size, wkspsz;
	int err;

	if (!zstd || size < sizeof(struct z_erofs_zstd_cfgs) || zstd->format) {
		erofs_err(sb, "unsupported zstd format, size=%u", size);
		return -EINVAL;
	}

	if (zstd->windowlog > ilog2(Z_EROFS_ZSTD_MAX_DICT_SIZE) - 10) {
		erofs_err(sb, "unsupported zstd window log %u (max %u)",
			  zstd->windowlog + 10,
			  ilog2(Z_EROFS_ZSTD_MAX_DICT_SIZE));
		return -EINVAL;
	}
	dict_size = 1U << (zstd->windowlog + 10);

	/* the lazily allocated workspaces are sized for the largest window */
	wkspsz = ZSTD_DStreamWorkspaceBound(dict_size);
	if (!wkspsz) {
		erofs_err(sb, "invalid zstd window size %u", dict_size);
		return -EINVAL;
	}

	mutex_lock(&z_erofs_zstd_init_mutex);
	if (dict_size > z_erofs_zstd_max_dictsize)
		z_erofs_zstd_max_dictsize = dict_size;
	err = z_erofs_zstd_init();
	mutex_unlock(&z_erofs_zstd_init_mutex);
	return err;
}

/* (re)allocate per-stream buffers; the stream is exclusively ours */
static int z_erofs_zstd_prepare_strm(struct z_erofs_zstd *strm,
				     unsigned int inputsize)
{
	const gfp_t gfp = GFP_NOFS | __GFP_HIGHMEM;
	size_t wkspsz = ZSTD_DStreamWorkspaceBound(
			READ_ONCE(z_erofs_zstd_max_dictsize));
	void *p;

	if (strm->wkspsz < wkspsz) {
		p = __vmalloc(wkspsz, gfp, PAGE_KERNEL);
		if (!p)
			return -ENOMEM;
		vfree(strm->wksp);
		strm->wksp = p;
		strm->wkspsz = wkspsz;
	}

	if (strm->inbufsz < inputsize) {
		p = __vmalloc(inputsize, gfp, PAGE_KERNEL);
		if (!p)
			return -ENOMEM;
		vfree(strm->inbuf);
		strm->inbuf = p;
		strm->inbufsz = inputsize;
	}
	return 0;
}

/* zero the output range [from, rq->outputsize) so that no stale data leaks */
static void z_erofs_zstd_zeroout(struct z_erofs_decompress_req *rq,
				 unsigned int from)
{
	unsigned int pos = from;

	while (pos < rq->outputsize) {
		unsigned int off = (rq->pageofs_out + pos) & ~PAGE_MASK;
		unsigned int len = min_t(unsigned int, PAGE_SIZE - off,
					 rq->outputsize - pos);
		struct page *page =
			rq->out[(rq->pageofs_out + pos) >> PAGE_SHIFT];

		if (page) {
			u8 *kaddr = kmap_atomic(page);

			memset(kaddr + off, 0, len);
			kunmap_atomic(kaddr);
		}
		pos += len;
	}
}

int z_erofs_zstd_decompress(struct z_erofs_decompress_req *rq)
{
	const unsigned int nrpages_in =
		PAGE_ALIGN(rq->inputsize) >> PAGE_SHIFT;
	struct z_erofs_zstd *strm;
	ZSTD_DStream *stream;
	ZSTD_inBuffer in_buf;
	unsigned int pos = 0, skip, i;
	int err;

	might_sleep();
	strm = z_erofs_zstd_get_strm();
	err = z_erofs_zstd_prepare_strm(strm, nrpages_in << PAGE_SHIFT);
	if (err)
		goto out_zero;

	/* gather the whole compressed pcluster into a contiguous buffer */
	for (i = 0; i < nrpages_in; ++i) {
		unsigned int len = min_t(unsigned int, PAGE_SIZE,
					 rq->inputsize - i * PAGE_SIZE);
		u8 *kaddr;

		DBG_BUGON(!rq->in[i]);
		kaddr = kmap_atomic(rq->in[i]);
		memcpy(strm->inbuf + i * PAGE_SIZE, kaddr, len);
		kunmap_atomic(kaddr);
	}

	/* skip the leading zero padding (zstd frames never start with 0) */
	for (skip = 0; skip < rq->inputsize; ++skip)
		if (((u8 *)strm->inbuf)[skip])
			break;
	if (skip >= rq->inputsize) {
		erofs_err(rq->sb, "all-zero zstd pcluster, inputsize=%u",
			  rq->inputsize);
		err = -EFSCORRUPTED;
		goto out_zero;
	}

	in_buf.src = strm->inbuf + skip;
	in_buf.size = rq->inputsize - skip;
	in_buf.pos = 0;

	stream = ZSTD_initDStream(z_erofs_zstd_max_dictsize,
				  strm->wksp, strm->wkspsz);
	if (!stream) {
		erofs_err(rq->sb, "failed to initialize zstd stream");
		err = -EIO;
		goto out_zero;
	}

	while (pos < rq->outputsize) {
		const unsigned int off = (rq->pageofs_out + pos) & ~PAGE_MASK;
		const unsigned int chunk = min_t(unsigned int, PAGE_SIZE - off,
						 rq->outputsize - pos);
		struct page *page =
			rq->out[(rq->pageofs_out + pos) >> PAGE_SHIFT];
		ZSTD_outBuffer out_buf;
		u8 *kaddr;

		kaddr = page ? kmap_atomic(page) : strm->bounce;
		out_buf.dst = kaddr + off;
		out_buf.size = chunk;
		out_buf.pos = 0;

		err = 0;
		while (out_buf.pos < out_buf.size) {
			size_t prev_in = in_buf.pos, prev_out = out_buf.pos;
			size_t ret = ZSTD_decompressStream(stream, &out_buf,
							   &in_buf);

			if (ZSTD_isError(ret)) {
				erofs_err(rq->sb, "zstd error %d at out[%u], in[%zu/%zu]",
					  (int)ZSTD_getErrorCode(ret), pos,
					  in_buf.pos, in_buf.size);
				err = -EIO;
				break;
			}
			/* frame ended or input ran out before output is full */
			if (out_buf.pos < out_buf.size &&
			    (!ret || (in_buf.pos == prev_in &&
				      out_buf.pos == prev_out))) {
				erofs_err(rq->sb, "truncated zstd data at out[%u/%u], in[%zu/%zu]",
					  pos + (unsigned int)out_buf.pos,
					  rq->outputsize, in_buf.pos,
					  in_buf.size);
				err = -EIO;
				break;
			}
		}
		pos += out_buf.pos;
		if (page)
			kunmap_atomic(kaddr);
		if (err)
			goto out_zero;
	}
	goto out_put;

out_zero:
	z_erofs_zstd_zeroout(rq, pos);
out_put:
	z_erofs_zstd_put_strm(strm);
	return err;
}
