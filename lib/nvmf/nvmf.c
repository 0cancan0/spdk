/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/io_channel.h"
#include "spdk/nvmf.h"
#include "spdk/trace.h"

#include "spdk_internal/log.h"

#include "nvmf_internal.h"
#include "transport.h"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

#define MAX_SUBSYSTEMS 4

#define SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_DEFAULT_MAX_QPAIRS_PER_CTRLR 64
#define SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_DEFAULT_MAX_IO_SIZE 131072

void
spdk_nvmf_tgt_opts_init(struct spdk_nvmf_tgt_opts *opts)
{
	opts->max_queue_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size = SPDK_NVMF_DEFAULT_MAX_IO_SIZE;
}

static void
spdk_nvmf_poll_group_poll(void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;
	int rc;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		rc = spdk_nvmf_transport_poll_group_poll(tgroup);
		if (rc < 0) {
			return;
		}
	}
}

static int
spdk_nvmf_tgt_create_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_transport *transport;
	uint32_t sid;

	TAILQ_INIT(&group->tgroups);

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		spdk_nvmf_poll_group_add_transport(group, transport);
	}

	group->num_sgroups = tgt->max_sid;
	group->sgroups = calloc(group->num_sgroups, sizeof(struct spdk_nvmf_subsystem_poll_group));
	if (!group->sgroups) {
		return -1;
	}

	for (sid = 0; sid < group->num_sgroups; sid++) {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = tgt->subsystems[sid];
		if (!subsystem) {
			continue;
		}

		spdk_nvmf_poll_group_add_subsystem(group, subsystem);
	}

	group->poller = spdk_poller_register(spdk_nvmf_poll_group_poll, group, 0);

	return 0;
}

static void
spdk_nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t sid, nsid;

	spdk_poller_unregister(&group->poller);

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) {
		TAILQ_REMOVE(&group->tgroups, tgroup, link);
		spdk_nvmf_transport_poll_group_destroy(tgroup);
	}

	for (sid = 0; sid < group->num_sgroups; sid++) {
		sgroup = &group->sgroups[sid];

		for (nsid = 0; nsid < sgroup->num_channels; nsid++) {
			if (sgroup->channels[nsid]) {
				spdk_put_io_channel(sgroup->channels[nsid]);
				sgroup->channels[nsid] = NULL;
			}
		}

		free(sgroup->channels);
	}

	free(group->sgroups);
}

struct spdk_nvmf_tgt *
spdk_nvmf_tgt_create(struct spdk_nvmf_tgt_opts *opts)
{
	struct spdk_nvmf_tgt *tgt;

	tgt = calloc(1, sizeof(*tgt));
	if (!tgt) {
		return NULL;
	}

	if (!opts) {
		spdk_nvmf_tgt_opts_init(&tgt->opts);
	} else {
		tgt->opts = *opts;
	}

	tgt->master_thread = spdk_get_thread();
	if (!tgt->master_thread) {
		free(tgt);
		SPDK_ERRLOG("Call spdk_allocate_thread() prior to calling spdk_nvmf_tgt_create()\n");
		return NULL;
	}

	tgt->discovery_genctr = 0;
	tgt->discovery_log_page = NULL;
	tgt->discovery_log_page_size = 0;
	tgt->subsystems = NULL;
	tgt->max_sid = 0;
	TAILQ_INIT(&tgt->transports);

	spdk_io_device_register(tgt,
				spdk_nvmf_tgt_create_poll_group,
				spdk_nvmf_tgt_destroy_poll_group,
				sizeof(struct spdk_nvmf_poll_group));

	SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Max Queue Pairs Per Controller: %d\n",
		      tgt->opts.max_qpairs_per_ctrlr);
	SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Max Queue Depth: %d\n", tgt->opts.max_queue_depth);
	SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Max In Capsule Data: %d bytes\n",
		      tgt->opts.in_capsule_data_size);
	SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Max I/O Size: %d bytes\n", tgt->opts.max_io_size);

	return tgt;
}

void
spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_transport *transport, *transport_tmp;

	TAILQ_FOREACH_SAFE(transport, &tgt->transports, link, transport_tmp) {
		TAILQ_REMOVE(&tgt->transports, transport, link);
		spdk_nvmf_transport_destroy(transport);
	}

	if (tgt->discovery_log_page) {
		free(tgt->discovery_log_page);
	}

	if (tgt->subsystems) {
		free(tgt->subsystems);
	}

	free(tgt);
}

struct spdk_nvmf_tgt_listen_ctx {
	struct spdk_nvmf_transport *transport;
	struct spdk_nvme_transport_id trid;
};

static void
spdk_nvmf_tgt_listen_done(void *io_device, void *c, int status)
{
	free(c);
}

static int
spdk_nvmf_tgt_listen_add_transport(void *io_device,
				   struct spdk_io_channel *ch,
				   void *c)
{
	struct spdk_nvmf_tgt_listen_ctx *ctx = c;
	struct spdk_nvmf_poll_group *group;

	group = spdk_io_channel_get_ctx(ch);

	return spdk_nvmf_poll_group_add_transport(group, ctx->transport);
}

int
spdk_nvmf_tgt_listen(struct spdk_nvmf_tgt *tgt,
		     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_transport *transport;
	int rc;

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trtype);
	if (!transport) {
		struct spdk_nvmf_tgt_listen_ctx *ctx;

		transport = spdk_nvmf_transport_create(tgt, trid->trtype);
		if (!transport) {
			SPDK_ERRLOG("Transport initialization failed\n");
			return -EINVAL;
		}
		TAILQ_INSERT_TAIL(&tgt->transports, transport, link);

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			return -ENOMEM;
		}

		ctx->trid = *trid;
		ctx->transport = transport;

		/* Send a message to each poll group to notify it that a new transport
		 * is available.
		 * TODO: This call does not currently allow the user to wait for these
		 * messages to propagate. It also does not protect against two calls
		 * to this function overlapping
		 */
		spdk_for_each_channel(tgt,
				      spdk_nvmf_tgt_listen_add_transport,
				      ctx,
				      spdk_nvmf_tgt_listen_done);
	}

	rc = spdk_nvmf_transport_listen(transport, trid);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to listen on address '%s'\n", trid->traddr);
		return rc;
	}

	tgt->discovery_genctr++;

	return 0;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t sid;

	if (!subnqn) {
		return NULL;
	}

	for (sid = 0; sid < tgt->max_sid; sid++) {
		subsystem = tgt->subsystems[sid];
		if (subsystem == NULL) {
			continue;
		}

		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			return subsystem;
		}
	}

	return NULL;
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, enum spdk_nvme_transport_type type)
{
	struct spdk_nvmf_transport *transport;

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		if (transport->ops->type == type) {
			return transport;
		}
	}

	return NULL;
}

void
spdk_nvmf_tgt_accept(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_transport *transport, *tmp;

	TAILQ_FOREACH_SAFE(transport, &tgt->transports, link, tmp) {
		spdk_nvmf_transport_accept(transport);
	}
}

struct spdk_nvmf_poll_group *
spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_io_channel *ch;

	ch = spdk_get_io_channel(tgt);
	if (!ch) {
		SPDK_ERRLOG("Unable to get I/O channel for target\n");
		return NULL;
	}

	return spdk_io_channel_get_ctx(ch);
}

void
spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group)
{
	struct spdk_io_channel *ch;

	ch = spdk_io_channel_from_ctx(group);
	spdk_put_io_channel(ch);
}

int
spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	int rc = -1;
	struct spdk_nvmf_transport_poll_group *tgroup;

	qpair->group = group;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			rc = spdk_nvmf_transport_poll_group_add(tgroup, qpair);
			break;
		}
	}

	return rc;
}

int
spdk_nvmf_poll_group_remove(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	int rc = -1;
	struct spdk_nvmf_transport_poll_group *tgroup;

	qpair->group = NULL;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			rc = spdk_nvmf_transport_poll_group_remove(tgroup, qpair);
			break;
		}
	}

	return rc;
}

int
spdk_nvmf_poll_group_add_transport(struct spdk_nvmf_poll_group *group,
				   struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	tgroup = spdk_nvmf_transport_poll_group_create(transport);
	if (!tgroup) {
		SPDK_ERRLOG("Unable to create poll group for transport\n");
		return -1;
	}

	TAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);

	return 0;
}

int
spdk_nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
				   struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_ns *ns;
	uint32_t i;

	if (subsystem->id >= group->num_sgroups) {
		void *buf;


		buf = realloc(group->sgroups, (subsystem->id + 1) * sizeof(*sgroup));
		if (!buf) {
			return -ENOMEM;
		}

		/* Zero out the newly allocated memory */
		memset(&group->sgroups[group->num_sgroups],
		       0,
		       (subsystem->id + 1 - group->num_sgroups) * sizeof(struct spdk_io_channel *));

		group->num_sgroups = subsystem->id + 1;
		group->sgroups = buf;
	}

	sgroup = &group->sgroups[subsystem->id];

	sgroup->num_channels = subsystem->max_nsid;
	sgroup->channels = calloc(sgroup->num_channels, sizeof(struct spdk_io_channel *));
	if (!sgroup->channels) {
		return -1;
	}

	for (i = 0; i < sgroup->num_channels; i++) {
		ns = &subsystem->ns[i];
		if (ns->allocated && sgroup->channels[i] == NULL) {
			sgroup->channels[i] = spdk_bdev_get_io_channel(ns->desc);
			if (sgroup->channels[i] == NULL) {
				return -1;
			}
		}
	}

	return 0;
}

int
spdk_nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t nsid;

	sgroup = &group->sgroups[subsystem->id];

	for (nsid = 0; nsid < sgroup->num_channels; nsid++) {
		if (sgroup->channels[nsid]) {
			spdk_put_io_channel(sgroup->channels[nsid]);
			sgroup->channels[nsid] = NULL;
		}
	}

	sgroup->num_channels = 0;
	free(sgroup->channels);
	sgroup->channels = NULL;

	return 0;
}

int
spdk_nvmf_poll_group_add_ns(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_subsystem *subsystem,
			    struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t ns_idx;

	sgroup = &group->sgroups[subsystem->id];

	/* The index into the channels array is (nsid - 1) */
	ns_idx = ns->id - 1;

	if (ns_idx >= sgroup->num_channels) {
		void *buf;

		buf = realloc(sgroup->channels,
			      ns->id * sizeof(struct spdk_io_channel *));
		if (!buf) {
			return -ENOMEM;
		}

		/* Zero out the newly allocated memory */
		memset(&sgroup->channels[sgroup->num_channels],
		       0,
		       (ns->id - sgroup->num_channels) * sizeof(struct spdk_io_channel *));

		sgroup->num_channels = ns->id;
		sgroup->channels = buf;
	}

	/* The channel could have been created in response to a subsystem creation
	 * event already propagating through the system */
	if (sgroup->channels[ns_idx] == NULL) {
		sgroup->channels[ns_idx] = spdk_bdev_get_io_channel(ns->desc);
	}

	if (sgroup->channels[ns_idx] == NULL) {
		return -1;
	}

	return 0;
}

SPDK_TRACE_REGISTER_FN(nvmf_trace)
{
	spdk_trace_register_object(OBJECT_NVMF_IO, 'r');
	spdk_trace_register_description("NVMF_IO_START", "", TRACE_NVMF_IO_START,
					OWNER_NONE, OBJECT_NVMF_IO, 1, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_READ_START", "", TRACE_RDMA_READ_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_WRITE_START", "", TRACE_RDMA_WRITE_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_READ_COMPLETE", "", TRACE_RDMA_READ_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_WRITE_COMPLETE", "", TRACE_RDMA_WRITE_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_READ_START", "", TRACE_NVMF_LIB_READ_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_WRITE_START", "", TRACE_NVMF_LIB_WRITE_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_COMPLETE", "", TRACE_NVMF_LIB_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_IO_COMPLETION_DONE", "", TRACE_NVMF_IO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
}
