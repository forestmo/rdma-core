/*
 * Copyright (c) 2014-2019 Dell EMC, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ntrdma_ioctl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/ioctl.h>
#include "ntrdma.h"

static inline int make_ntrdma_send_wqe(struct ntrdma_send_wqe *wqe,
				struct ibv_send_wr *swr,
				int available_size)
{
	int available_size_in = available_size;
	bool is_inline;
	size_t tail_size;
	void *ptr;
	size_t len;
	int i;

	static_assert(sizeof(struct ibv_sge) == sizeof(swr->sg_list[0]),
		"Expecting struct ibv_sge in swr->sg_list");

	if (available_size < (int)sizeof(*wqe))
		return -ENOMEM;
	available_size -= sizeof(*wqe);

	if ((swr->opcode == IBV_WR_RDMA_WRITE) &&
		(swr->num_sge == 1) && (swr->sg_list[0].length <= 8))
		swr->send_flags = swr->send_flags | IBV_SEND_INLINE;

	is_inline = ((swr->opcode == IBV_WR_RDMA_WRITE) &&
		(swr->send_flags & IBV_SEND_INLINE));

	if (is_inline) {
		for ((i = 0), (tail_size = 0); i < swr->num_sge; i++) {
			tail_size += swr->sg_list[i].length;
			if (available_size < tail_size)
				return -ENOMEM;
		}
		available_size -= tail_size;
	} else {
		tail_size = swr->num_sge * sizeof(struct ibv_sge);
		if (available_size < tail_size)
			return -ENOMEM;
		available_size -= tail_size;
	}

	wqe->ulp_handle = swr->wr_id;
	wqe->op_code = swr->opcode;
	wqe->rdma_sge.addr = swr->wr.rdma.remote_addr;
	wqe->rdma_sge.lkey = swr->wr.rdma.rkey;
	wqe->imm_data = swr->imm_data;
	wqe->flags = swr->send_flags;
	if (is_inline)
		wqe->inline_len = tail_size;
	else
		wqe->sg_count = swr->num_sge;

	if (!tail_size)
		return available_size_in - available_size;

	if (is_inline)
		for ((i = 0), (ptr = wqe + 1); i < swr->num_sge;
		     (i++), (ptr += len)) {
			len = swr->sg_list[i].length;
			if (!len)
				continue;
			memcpy(ptr, (void *)swr->sg_list[i].addr, len);
		}
	else
		memcpy(wqe + 1, swr->sg_list, tail_size);

	return available_size_in - available_size;
}

int ntrdma_query_device(struct ibv_context *context,
			struct ibv_device_attr *device_attr)
{
	uint64_t raw_fw_ver;
	struct ibv_query_device cmd;

	return ibv_cmd_query_device(context, device_attr,
				    &raw_fw_ver, &cmd, sizeof cmd);
}

int ntrdma_query_port(struct ibv_context *context, uint8_t port_num,
		      struct ibv_port_attr *port_attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(context, port_num, port_attr,
				  &cmd, sizeof cmd);
}

struct ibv_pd *ntrdma_alloc_pd(struct ibv_context *context)
{
	struct ibv_pd *pd;
	struct ibv_alloc_pd cmd;
	struct ib_uverbs_alloc_pd_resp resp;

	pd = malloc(sizeof(*pd));
	if (!pd)
		return NULL;

	memset(pd, 0, sizeof(*pd));

	errno = ibv_cmd_alloc_pd(context, pd,
				 &cmd, sizeof cmd,
				 &resp, sizeof resp);
	if (errno)
		goto err_free;

	return pd;

err_free:
	free(pd);
	return NULL;
}

int ntrdma_dealloc_pd(struct ibv_pd *pd)
{
	int ret;

	ret = ibv_cmd_dealloc_pd(pd);
	if (ret)
		return ret;

	free(pd);
	return 0;
}

struct ibv_mr *ntrdma_reg_mr(struct ibv_pd *pd, void *addr,
			     size_t length, int access)
{
	struct verbs_mr *vmr;
	struct ibv_reg_mr cmd;
	struct ib_uverbs_reg_mr_resp resp;

	vmr = malloc(sizeof(*vmr));
	if (!vmr)
		return NULL;

	memset(vmr, 0, sizeof(*vmr));

	errno = ibv_cmd_reg_mr(pd, addr,
			       length, (unsigned long)addr,
			       access, vmr,
			       &cmd, sizeof cmd,
			       &resp, sizeof resp);
	if (errno) {
		free(vmr);
		return NULL;
	}

	return &vmr->ibv_mr;
}

int ntrdma_dereg_mr(struct verbs_mr *vmr)
{
	int ret;

	ret = ibv_cmd_dereg_mr(vmr);
	if (ret)
		return ret;

	free(vmr);
	return 0;
}

struct ibv_cq *ntrdma_create_cq(struct ibv_context *context, int cqe,
				struct ibv_comp_channel *channel,
				int comp_vector)
{
	struct ntrdma_cq *cq;
	struct {
		struct ibv_create_cq cmd;
		struct ntrdma_create_cq_ext ext;
	} ext_cmd;
	struct {
		struct ib_uverbs_create_cq_resp resp;
		struct ntrdma_create_cq_resp_ext ext;
	} ext_resp = {
		.ext = { .cqfd = -1 }
	};

	/* resp must be first member of ext_resp. */
	assert((void *)&ext_resp == (void *)&ext_resp.resp);

	/* cmd must be first member of ext_cmd. */
	assert((void *)&ext_cmd == (void *)&ext_cmd.cmd);

	cq = malloc(sizeof(*cq));
	if (!cq)
		return NULL;

	memset(cq, 0, sizeof(*cq));

	pthread_mutex_init(&cq->mutex, NULL);

	cq->buffer_size = sysconf(_SC_PAGESIZE);
	cq->buffer = memalign(cq->buffer_size, cq->buffer_size);
	if (!cq->buffer)
		goto err;
	ext_cmd.ext.poll_page_ptr = (unsigned long)cq->buffer;

	ntrdma_ioctl_if_init_desc(&ext_cmd.ext.desc);

	errno = ibv_cmd_create_cq(context, cqe, channel,
				comp_vector, &cq->ibv_cq,
				&ext_cmd.cmd, sizeof ext_cmd,
				&ext_resp.resp, sizeof ext_resp);
	if (errno)
		goto err;

	cq->fd = ext_resp.ext.cqfd;

	PRINT_DEBUG_KMSG("NTRDMADEB %s: cq->fd = %d\n", __func__, cq->fd);

	if (cq->fd < 0) {
		free(cq->buffer);
		cq->buffer = NULL;
	}

	return &cq->ibv_cq;

 err:
	if (cq->buffer)
		free(cq->buffer);

	pthread_mutex_destroy(&cq->mutex);

	free(cq);
	return NULL;
}

int ntrdma_poll_cq(struct ibv_cq *_cq, int num_entries, struct ibv_wc *wc)
{
 	DEFINE_NTC_FUNC_PERF_TRACKER(perf, 1 << 20);
	struct ntrdma_cq *cq = to_ntrdma_cq(_cq);
	struct ntrdma_poll_hdr *hdr;
	int max_entries, req_entries, resp_entries, total;
	int rc;

	static_assert(sizeof(*wc) == sizeof(struct ntrdma_ibv_wc),
		"Must match");

	if (num_entries < 0)
		return -EINVAL;

	if (num_entries == 0)
		return 0;

	if (cq->fd < 0) {
		rc = ibv_cmd_poll_cq(_cq, num_entries, wc);
		NTC_PERF_MEASURE(perf);
		return rc;
	}

	pthread_mutex_lock(&cq->mutex);

	rc = 0;
	total = 0;
	max_entries = (cq->buffer_size - sizeof(*hdr))  / sizeof(*wc);
	hdr = cq->buffer;
	while (num_entries) {
		if (num_entries > max_entries)
			req_entries = max_entries;
		else
			req_entries = num_entries;

		hdr->wc_counter = req_entries;
		rc = ioctl(cq->fd, NTRDMA_IOCTL_POLL);
		if ((rc == -1) && (errno > 0))
			rc = -errno;
		if (rc < 0)
			break;
		resp_entries = hdr->wc_counter;
		if (!resp_entries)
			break;

		memcpy(wc + total,
			cq->buffer + sizeof(*hdr) + sizeof(*wc) * total,
			sizeof(*wc) * resp_entries);

		total += resp_entries;
		num_entries -= resp_entries;

		if (resp_entries < req_entries)
			break;
	}

	pthread_mutex_unlock(&cq->mutex);

 	NTC_PERF_MEASURE(perf);

	if (total)
		rc = total;

	return rc;
}

int ntrdma_destroy_cq(struct ibv_cq *_cq)
{
	struct ntrdma_cq *cq = to_ntrdma_cq(_cq);
	int ret;

	if (cq->fd >= 0) {
		close(cq->fd);
		cq->fd = -1;
	}

	ret = ibv_cmd_destroy_cq(&cq->ibv_cq);
	if (ret)
		return ret;

	free(cq->buffer);

	pthread_mutex_destroy(&cq->mutex);

	free(cq);
	return 0;
}

struct ibv_qp *ntrdma_create_qp(struct ibv_pd *pd,
				struct ibv_qp_init_attr *attr)
{
	struct ntrdma_qp *qp;
	struct {
		struct ibv_create_qp cmd;
		struct ntrdma_create_qp_ext ext;
	} ext_cmd;
	struct {
		 /* resp must be first member of this struct. */
		struct ib_uverbs_create_qp_resp resp;
		struct ntrdma_create_qp_resp_ext ext;
	} ext_resp = {
		.ext = { .qpfd = -1 }
	};

	/* resp must be first member of ext_resp. */
	assert((void *)&ext_resp == (void *)&ext_resp.resp);

	/* cmd must be first member of ext_cmd. */
	assert((void *)&ext_cmd == (void *)&ext_cmd.cmd);

	qp = malloc(sizeof(*qp));
	if (!qp)
		return NULL;

	memset(qp, 0, sizeof(*qp));

	pthread_mutex_init(&qp->mutex, NULL);

	qp->buffer_size = sysconf(_SC_PAGESIZE);
	qp->buffer = memalign(qp->buffer_size, qp->buffer_size);
	if (!qp->buffer)
		goto err;
	ext_cmd.ext.send_page_ptr = (unsigned long)qp->buffer;

	ntrdma_ioctl_if_init_desc(&ext_cmd.ext.desc);

	errno = ibv_cmd_create_qp(pd, &qp->ibv_qp, attr,
				&ext_cmd.cmd, sizeof ext_cmd,
				&ext_resp.resp, sizeof ext_resp);
	if (errno)
		goto err;

	qp->fd = ext_resp.ext.qpfd;

	PRINT_DEBUG_KMSG("NTRDMADEB %s: qp->fd = %d\n", __func__, qp->fd);

	if (qp->fd < 0) {
		free(qp->buffer);
		qp->buffer = NULL;
	}

	return &qp->ibv_qp;

 err:
	if (qp->buffer)
		free(qp->buffer);

	pthread_mutex_destroy(&qp->mutex);

	free(qp);
	return NULL;
}

int ntrdma_modify_qp(struct ibv_qp *qp,
		     struct ibv_qp_attr *attr, int attr_mask)
{
	struct ibv_modify_qp cmd;

	return ibv_cmd_modify_qp(qp, attr, attr_mask,
				&cmd, sizeof cmd);
}

int ntrdma_destroy_qp(struct ibv_qp *_qp)
{
	struct ntrdma_qp *qp = to_ntrdma_qp(_qp);
	int ret;

	if (qp->fd >= 0) {
		close(qp->fd);
		qp->fd = -1;
	}

	ret = ibv_cmd_destroy_qp(&qp->ibv_qp);
	if (ret)
		return ret;

	free(qp->buffer);

	pthread_mutex_destroy(&qp->mutex);

	free(qp);
	return 0;
}

int ntrdma_query_qp(struct ibv_qp *qp,
		    struct ibv_qp_attr *qp_attr, int attr_mask,
		    struct ibv_qp_init_attr *qp_init_attr)
{
	struct ibv_query_qp cmd;

	return ibv_cmd_query_qp(qp, qp_attr, attr_mask, qp_init_attr,
				&cmd, sizeof cmd);
}

int ntrdma_post_send(struct ibv_qp *_qp, struct ibv_send_wr *swr,
		     struct ibv_send_wr **bad)
{
	DEFINE_NTC_FUNC_PERF_TRACKER(perf, 1 << 20);
	struct ntrdma_qp *qp = to_ntrdma_qp(_qp);
	int offset;
	struct ntrdma_send_wqe *wqe;
	struct ntrdma_snd_hdr *hdr = qp->buffer;
	uint32_t *wqehdr;
	int wqe_counter;
	struct ibv_send_wr *swr_first;
	int rc = 0;

	if (qp->fd < 0) {
		rc = ibv_cmd_post_send(_qp, swr, bad);
		NTC_PERF_MEASURE(perf);
		return rc;
	}

	pthread_mutex_lock(&qp->mutex);

	while (swr) {
		swr_first = swr;

		wqehdr = &hdr->first_wqe_size;
		for ((wqe_counter = 0), (offset = sizeof(*hdr));
		     swr; (swr = swr->next), (wqe_counter++)) {
			wqe = qp->buffer + offset;
			rc = make_ntrdma_send_wqe(wqe, swr,
						qp->buffer_size - offset);
			if (rc < 0)
				break;
			*wqehdr = rc;
			offset += rc;
			wqehdr = &wqe->recv_key;
		}

		if (!wqe_counter)
			break;

		if (unlikely(swr))
			PRINT_DEBUG_KMSG("NTRDMADEB: %s: LONG SEND LIST. "
					"Sending %d WRs in one ioctl\n",
					__func__, wqe_counter);

		hdr->wqe_counter = wqe_counter;

		rc = ioctl(qp->fd, NTRDMA_IOCTL_SEND);

		if (rc >= 0)
			continue;

		if ((rc == -1) && (errno > 0))
			rc = -errno;

		for ((wqe_counter = hdr->wqe_counter), (swr = swr_first);
		     wqe_counter; wqe_counter--)
			swr = swr->next;
		break;
	}

	pthread_mutex_unlock(&qp->mutex);

	*bad = swr;

	NTC_PERF_MEASURE(perf);

	return rc;
}

int ntrdma_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *rwr,
		     struct ibv_recv_wr **bad)
{
	return ibv_cmd_post_recv(qp, rwr, bad);
}

struct ibv_ah *ntrdma_create_ah(struct ibv_pd *pd,
				struct ibv_ah_attr *attr)
{
	errno = ENOSYS;
	return NULL;
}

int ntrdma_destroy_ah(struct ibv_ah *ah)
{
	errno = ENOSYS;
	return -1;
}

int ntrdma_req_notify_cq(struct ibv_cq *cq, int solicited_only)
{
	return ibv_cmd_req_notify_cq(cq, solicited_only);
}
