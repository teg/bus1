/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/wait.h>
#include "active.h"
#include "domain.h"
#include "peer.h"

static struct bus1_domain_info *
bus1_domain_info_new(struct user_namespace *user_ns)
{
	struct bus1_domain_info *domain_info;

	domain_info = kmalloc(sizeof(*domain_info), GFP_KERNEL);
	if (!domain_info)
		return ERR_PTR(-ENOMEM);

	kref_init(&domain_info->ref);
	domain_info->peer_ids = 0;
	atomic64_set(&domain_info->seq_ids, 0);
	domain_info->user_ns = get_user_ns(user_ns);

	return domain_info;
}

static void bus1_domain_info_free(struct kref *ref)
{
	struct bus1_domain_info *domain_info =
				container_of(ref, struct bus1_domain_info, ref);

	put_user_ns(domain_info->user_ns);
	kfree(domain_info);
}

static struct bus1_domain_info *
bus1_domain_info_unref(struct bus1_domain_info *domain_info)
{
	if (domain_info)
		kref_put(&domain_info->ref, bus1_domain_info_free);

	return NULL;
}

/**
 * bus1_domain_new() - allocate new domain
 * @user_ns:	user namespace to create domain in
 *
 * Allocate a new, unused domain. On return, the caller will be the only
 * context that can access the domain, and as such has exclusive ownership.
 *
 * The domain is implicitly marked as active by this call, and a domain
 * information object is linked.
 *
 * Return: Pointer to object, ERR_PTR on failure.
 */
struct bus1_domain *bus1_domain_new(struct user_namespace *user_ns)
{
	struct bus1_domain *domain;
	int r;

	domain = kmalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return ERR_PTR(-ENOMEM);

	mutex_init(&domain->lock);
	bus1_active_init(&domain->active);
	seqcount_init(&domain->seqcount);
	init_waitqueue_head(&domain->waitq);
	domain->info = NULL;
	domain->n_peers = 0;
	domain->n_names = 0;
	domain->map_peers = RB_ROOT;
	domain->map_names = RB_ROOT;

	/* domains are implicitly activated during allocation */
	domain->info = bus1_domain_info_new(user_ns);
	if (IS_ERR(domain->info)) {
		r = PTR_ERR(domain->info);
		domain->info = NULL;
		goto error;
	}

	bus1_active_activate(&domain->active);

	return domain;

error:
	bus1_domain_free(domain);
	return ERR_PTR(r);
}

/**
 * bus1_domain_free() - destroy domain
 * @domain:	domain to destroy, or NULL
 *
 * This destroys the passed domain object. The caller must make sure the domain
 * is fully deactivated and torn down. No-one else must access the domain,
 * anymore.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_domain *bus1_domain_free(struct bus1_domain *domain)
{
	if (!domain)
		return NULL;

	WARN_ON(!RB_EMPTY_ROOT(&domain->map_names));
	WARN_ON(!RB_EMPTY_ROOT(&domain->map_peers));
	WARN_ON(domain->n_names > 0);
	WARN_ON(domain->n_peers > 0);
	WARN_ON(domain->info);
	bus1_active_destroy(&domain->active);
	kfree(domain);

	return NULL;
}

static void bus1_domain_cleanup(struct bus1_active *active, void *userdata)
{
	struct bus1_domain *domain = container_of(active, struct bus1_domain,
						  active);

	domain->info = bus1_domain_info_unref(domain->info);
}

/**
 * bus1_domain_teardown() - deactivate and tear down a domain
 * @domain:	domain to tear down
 *
 * This deactivates a domain and tears it down. Any linked peer is deactivated
 * and removed, all associated data is dropped.
 *
 * Once this returns, the domain is fully torn down and no-one else owns an
 * active reference to the domain, anymore.
 *
 * It is safe to call this multiple times (even in parallel) on the same
 * domain.
 */
void bus1_domain_teardown(struct bus1_domain *domain)
{
	struct bus1_peer *peer;
	struct rb_node *n;

	/*
	 * This tears down a whole domain, in a synchronous fashion. This is
	 * non-trivial, as it requires to synchronously drain all peers. So we
	 * first deactivate the domain, which prevents new peers from being
	 * linked to the domain. We also drain the domain to guarantee any
	 * racing connect/reset calls are done. Then we deactivate all peers,
	 * which prevents any new operation to be entered. We also drain all
	 * peers so we're guaranteed all their operations are done.
	 *
	 * At this point, we know that all our objects are drained. However,
	 * they might not have been cleaned up, yet. Therefore, we now take
	 * the domain write seqcount and release every peer, lastly followed
	 * by a release of the domain.
	 */

	bus1_active_deactivate(&domain->active);
	bus1_active_drain(&domain->active, &domain->waitq);

	mutex_lock(&domain->lock);

	for (n = rb_first(&domain->map_peers); n; n = rb_next(n)) {
		peer = container_of(n, struct bus1_peer, rb);
		bus1_active_deactivate(&peer->active);
		bus1_active_drain(&peer->active, &peer->waitq);
	}

	write_seqcount_begin(&domain->seqcount);
	for (n = rb_first(&domain->map_peers); n; n = rb_next(n)) {
		peer = container_of(n, struct bus1_peer, rb);
		bus1_peer_teardown_domain(peer, domain);
	}
	WARN_ON(!RB_EMPTY_ROOT(&domain->map_names));
	WARN_ON(domain->n_names > 0);
	WARN_ON(domain->n_peers > 0);
	domain->map_peers = RB_ROOT;
	write_seqcount_end(&domain->seqcount);

	mutex_unlock(&domain->lock);

	bus1_active_cleanup(&domain->active, &domain->waitq,
			    bus1_domain_cleanup, NULL);
}

/**
 * bus1_domain_acquire() - acquire active reference to domain
 * @domain:	domain to work on, or NULL
 *
 * This tries to acquire a new active reference to the passed domain. If the
 * domain is not active (as such, no active reference can be acquired), NULL is
 * returned.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: Pointer to domain, or NULL on failure.
 */
struct bus1_domain *bus1_domain_acquire(struct bus1_domain *domain)
{
	if (domain && bus1_active_acquire(&domain->active))
		return domain;
	return NULL;
}

/**
 * bus1_domain_release() - release active reference to domain
 * @domain:	domain to work on, or NULL
 *
 * Release an active reference previously acquired via bus1_domain_acquire().
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_domain *bus1_domain_release(struct bus1_domain *domain)
{
	if (domain)
		bus1_active_release(&domain->active, &domain->waitq);
	return NULL;
}
