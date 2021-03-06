/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Write data to fd
 * We try to use writev() if possible in order to minimize number of
 * syscalls made and packets sent.  It also just might allow the worker
 * thread to complete the request without holding stuff locked.
 */

#include "config.h"

#include <sys/types.h>
#ifdef SENDFILE_WORKS
#  if defined(__FreeBSD__) || defined(__DragonFly__)
#    include <sys/socket.h>
#  elif defined(__linux__)
#    include <sys/sendfile.h>
#  elif defined(__sun)
#    include <sys/sendfile.h>
#  else
#     error Unknown sendfile() implementation
#  endif
#endif /* SENDFILE_WORKS */
#include <sys/uio.h>

#include <stdio.h>

#include "cache.h"
#include "vtim.h"

/*--------------------------------------------------------------------
 */

int
WRW_Error(const struct worker *wrk)
{

	return (wrk->wrw.werr);
}

void
WRW_Reserve(struct worker *wrk, int *fd)
{
	struct wrw *wrw;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;
	AZ(wrw->wfd);
	wrw->werr = 0;
	wrw->liov = 0;
	wrw->niov = 0;
	wrw->ciov = wrw->siov;
	wrw->wfd = fd;
}

static void
WRW_Release(struct worker *wrk)
{
	struct wrw *wrw;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;
	AN(wrw->wfd);
	wrw->werr = 0;
	wrw->liov = 0;
	wrw->niov = 0;
	wrw->ciov = wrw->siov;
	wrw->wfd = NULL;
}

static void
wrw_prune(struct wrw *wrw, ssize_t bytes)
{
	ssize_t used = 0;
	ssize_t j, used_here;

	for (j = 0; j < wrw->niov; j++) {
		if (used + wrw->iov[j].iov_len > bytes) {
			/* Cutoff is in this iov */
			used_here = bytes - used;
			wrw->iov[j].iov_len -= used_here;
			wrw->iov[j].iov_base =
			    (char*)wrw->iov[j].iov_base + used_here;
			memmove(wrw->iov, &wrw->iov[j],
			    (wrw->niov - j) * sizeof(struct iovec));
			wrw->niov -= j;
			wrw->liov -= bytes;
			return;
		}
		used += wrw->iov[j].iov_len;
	}
	assert(wrw->liov == 0);
}

unsigned
WRW_Flush(struct worker *wrk)
{
	ssize_t i;
	struct wrw *wrw;
	char cbuf[32];

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;
	AN(wrw->wfd);

	/* For chunked, there must be one slot reserved for the chunked tail */
	if (wrw->ciov < wrw->siov)
		assert(wrw->niov < wrw->siov);

	if (*wrw->wfd >= 0 && wrw->liov > 0 && wrw->werr == 0) {
		if (wrw->ciov < wrw->siov && wrw->cliov > 0) {
			/* Add chunk head & tail */
			bprintf(cbuf, "00%zx\r\n", wrw->cliov);
			i = strlen(cbuf);
			wrw->iov[wrw->ciov].iov_base = cbuf;
			wrw->iov[wrw->ciov].iov_len = i;
			wrw->liov += i;

			wrw->iov[wrw->niov].iov_base = cbuf + i - 2;
			wrw->iov[wrw->niov++].iov_len = 2;
			wrw->liov += 2;
		} else if (wrw->ciov < wrw->siov) {
			wrw->iov[wrw->ciov].iov_base = cbuf;
			wrw->iov[wrw->ciov].iov_len = 0;
		}

		i = writev(*wrw->wfd, wrw->iov, wrw->niov);
		while (i != wrw->liov && i > 0) {
			/* Remove sent data from start of I/O vector,
			 * then retry; we hit a timeout, but some data
			 * was sent.
			 *
			 * XXX: Add a "minimum sent data per timeout
			 * counter to prevent slowlaris attacks
			*/

			if (VTIM_real() - wrk->sp->req->t_resp >
			    cache_param->send_timeout) {
				WSL(wrk, SLT_Debug, *wrw->wfd,
				    "Hit total send timeout, "
				    "wrote = %ld/%ld; not retrying",
				    i, wrw->liov);
				i = -1;
				break;
			}

			WSL(wrk, SLT_Debug, *wrw->wfd,
			    "Hit send timeout, wrote = %ld/%ld; retrying",
			    i, wrw->liov);

			wrw_prune(wrw, i);
			i = writev(*wrw->wfd, wrw->iov, wrw->niov);
		}
		if (i <= 0) {
			wrw->werr++;
			WSL(wrk, SLT_Debug, *wrw->wfd,
			    "Write error, retval = %zd, len = %zd, errno = %s",
			    i, wrw->liov, strerror(errno));
		}
	}
	wrw->liov = 0;
	wrw->cliov = 0;
	wrw->niov = 0;
	if (wrw->ciov < wrw->siov)
		wrw->ciov = wrw->niov++;
	return (wrw->werr);
}

unsigned
WRW_FlushRelease(struct worker *wrk)
{
	unsigned u;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	AN(wrk->wrw.wfd);
	u = WRW_Flush(wrk);
	WRW_Release(wrk);
	return (u);
}

unsigned
WRW_WriteH(struct worker *wrk, const txt *hh, const char *suf)
{
	unsigned u;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	AN(wrk->wrw.wfd);
	AN(wrk);
	AN(hh);
	AN(hh->b);
	AN(hh->e);
	u = WRW_Write(wrk, hh->b, hh->e - hh->b);
	if (suf != NULL)
		u += WRW_Write(wrk, suf, -1);
	return (u);
}

unsigned
WRW_Write(struct worker *wrk, const void *ptr, int len)
{
	struct wrw *wrw;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;
	AN(wrw->wfd);
	if (len == 0 || *wrw->wfd < 0)
		return (0);
	if (len == -1)
		len = strlen(ptr);
	if (wrw->niov >= wrw->siov - (wrw->ciov < wrw->siov ? 1 : 0))
		(void)WRW_Flush(wrk);
	wrw->iov[wrw->niov].iov_base = TRUST_ME(ptr);
	wrw->iov[wrw->niov].iov_len = len;
	wrw->liov += len;
	wrw->niov++;
	if (wrw->ciov < wrw->siov) {
		assert(wrw->niov < wrw->siov);
		wrw->cliov += len;
	}
	return (len);
}

void
WRW_Chunked(struct worker *wrk)
{
	struct wrw *wrw;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;

	assert(wrw->ciov == wrw->siov);
	/*
	 * If there are not space for chunked header, a chunk of data and
	 * a chunk tail, we might as well flush right away.
	 */
	if (wrw->niov + 3 >= wrw->siov)
		(void)WRW_Flush(wrk);
	wrw->ciov = wrw->niov++;
	wrw->cliov = 0;
	assert(wrw->ciov < wrw->siov);
	assert(wrw->niov < wrw->siov);
}

/*
 * XXX: It is not worth the complexity to attempt to get the
 * XXX: end of chunk into the WRW_Flush(), because most of the time
 * XXX: if not always, that is a no-op anyway, because the calling
 * XXX: code already called WRW_Flush() to release local storage.
 */

void
WRW_EndChunk(struct worker *wrk)
{
	struct wrw *wrw;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;

	assert(wrw->ciov < wrw->siov);
	(void)WRW_Flush(wrk);
	wrw->ciov = wrw->siov;
	wrw->niov = 0;
	wrw->cliov = 0;
	(void)WRW_Write(wrk, "0\r\n\r\n", -1);
}


#ifdef SENDFILE_WORKS
void
WRW_Sendfile(struct worker *wrk, int fd, off_t off, unsigned len)
{
	struct wrw *wrw;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	wrw = &wrk->wrw;
	AN(wrw->wfd);
	assert(fd >= 0);
	assert(len > 0);

#if defined(__FreeBSD__) || defined(__DragonFly__)
	do {
		struct sf_hdtr sfh;
		memset(&sfh, 0, sizeof sfh);
		if (wrw->niov > 0) {
			sfh.headers = wrw->iov;
			sfh.hdr_cnt = wrw->niov;
		}
		if (sendfile(fd, *wrw->wfd, off, len, &sfh, NULL, 0) != 0)
			wrw->werr++;
		wrw->liov = 0;
		wrw->niov = 0;
	} while (0);
#elif defined(__linux__)
	do {
		if (WRW_Flush(wrk) == 0 &&
		    sendfile(*wrw->wfd, fd, &off, len) != len)
			wrw->werr++;
	} while (0);
#elif defined(__sun) && defined(HAVE_SENDFILEV)
	do {
		sendfilevec_t svvec[cache_param->http_headers * 2 + 1];
		size_t xferred = 0, expected = 0;
		int i;
		for (i = 0; i < wrw->niov; i++) {
			svvec[i].sfv_fd = SFV_FD_SELF;
			svvec[i].sfv_flag = 0;
			svvec[i].sfv_off = (off_t) wrw->iov[i].iov_base;
			svvec[i].sfv_len = wrw->iov[i].iov_len;
			expected += svvec[i].sfv_len;
		}
		svvec[i].sfv_fd = fd;
		svvec[i].sfv_flag = 0;
		svvec[i].sfv_off = off;
		svvec[i].sfv_len = len;
		expected += svvec[i].sfv_len;
		if (sendfilev(*wrw->wfd, svvec, i, &xferred) == -1 ||
		    xferred != expected)
			wrw->werr++;
		wrw->liov = 0;
		wrw->niov = 0;
	} while (0);
#elif defined(__sun) && defined(HAVE_SENDFILE)
	do {
		if (WRW_Flush(wrk) == 0 &&
		    sendfile(*wrw->wfd, fd, &off, len) != len)
			wrw->werr++;
	} while (0);
#else
#error Unknown sendfile() implementation
#endif
}
#endif /* SENDFILE_WORKS */

