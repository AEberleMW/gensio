/*
 *  gensio - A library for streaming I/O
 *  Copyright (C) 2020  Corey Minyard <minyard@acm.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-only
 *
 *  In addition, as a special exception, the copyright holders of
 *  gensio give you permission to combine gensio with free software
 *  programs or libraries that are released under the GNU LGPL and
 *  with code included in the standard release of OpenSSL under the
 *  OpenSSL license (or modified versions of such code, with unchanged
 *  license). You may copy and distribute such a system following the
 *  terms of the GNU GPL for gensio and the licenses of the other code
 *  concerned, provided that you include the source code of that
 *  other code when and as the GNU GPL requires distribution of source
 *  code.
 *
 *  Note that people who make modified versions of gensio are not
 *  obligated to grant this special exception for their modified
 *  versions; it is their choice whether to do so. The GNU General
 *  Public License gives permission to release a modified version
 *  without this exception; this exception also makes it possible to
 *  release a modified version which carries forward this exception.
 */

/*
 * This file provides an Avahi poll structure based upon
 * gensio_os_funcs.  It's a pretty straightforward translation.
 */

#include "config.h"
#ifdef HAVE_AVAHI

#include <stdlib.h>
#include <assert.h>
#include <gensio/gensio_err.h>
#include "avahi_watcher.h"

struct gensio_avahi_userdata {
    struct gensio_os_funcs *o;

    AvahiPoll *ap;

    /* This lock is used for all callbacks.  Only one callback at a time. */
    struct gensio_lock *lock;

    gensio_avahi_done stop_done;
    void *stop_userdata;
    struct gensio_runner *runner;

    unsigned int refcount;

    bool stopped;
};

static void
gensio_avahi_poll_deref(AvahiPoll *ap)
{
    struct gensio_avahi_userdata *u = ap->userdata;
    struct gensio_os_funcs *o = u->o;

    assert(u->refcount > 0);
    u->refcount--;
    if (u->refcount == 0)
	o->run(u->runner);
}

void
gensio_avahi_lock(AvahiPoll *ap)
{
    struct gensio_avahi_userdata *u = ap->userdata;
    struct gensio_os_funcs *o = u->o;

    o->lock(u->lock);
}

void
gensio_avahi_unlock(AvahiPoll *ap)
{
    struct gensio_avahi_userdata *u = ap->userdata;
    struct gensio_os_funcs *o = u->o;

    o->unlock(u->lock);
}

struct AvahiWatch {
    struct gensio_avahi_userdata *u;
    int fd;
    AvahiWatchEvent events;
    AvahiWatchEvent revents;
    bool freed;
    AvahiWatchCallback callback;
    void *userdata;
};

static void
gensio_avahi_read_handler(int fd, void *cb_data)
{
    AvahiWatch *w = cb_data;
    struct gensio_avahi_userdata *u = w->u;
    struct gensio_os_funcs *o = u->o;

    o->lock(u->lock);
    if (!w->freed) {
	w->revents = AVAHI_WATCH_IN;
	w->callback(w, w->fd, w->revents, w->userdata);
	w->revents = 0;
    }
    o->unlock(u->lock);
}

static void
gensio_avahi_write_handler(int fd, void *cb_data)
{
    AvahiWatch *w = cb_data;
    struct gensio_avahi_userdata *u = w->u;
    struct gensio_os_funcs *o = u->o;

    o->lock(u->lock);
    if (!w->freed) {
	w->revents = AVAHI_WATCH_OUT;
	w->callback(w, w->fd, w->revents, w->userdata);
	w->revents = 0;
    }
    o->unlock(u->lock);
}

static void
gensio_avahi_except_handler(int fd, void *cb_data)
{
    AvahiWatch *w = cb_data;
    struct gensio_avahi_userdata *u = w->u;
    struct gensio_os_funcs *o = u->o;

    o->lock(u->lock);
    if (!w->freed) {
	w->revents = AVAHI_WATCH_ERR;
	w->callback(w, w->fd, w->revents, w->userdata);
	w->revents = 0;
    }
    o->unlock(u->lock);
}

static void
gensio_avahi_cleared_handler(int fd, void *cb_data)
{
    AvahiWatch *w = cb_data;
    struct gensio_avahi_userdata *u = w->u;
    struct gensio_os_funcs *o = u->o;

    o->free(o, w);
    o->lock(u->lock);
    gensio_avahi_poll_deref(u->ap);
    o->unlock(u->lock);
}

static void
gensio_avahi_watch_update(AvahiWatch *w, AvahiWatchEvent event)
{
    struct gensio_avahi_userdata *u = w->u;
    struct gensio_os_funcs *o = u->o;

    o->set_read_handler(o, w->fd, !!(event & AVAHI_WATCH_IN));
    o->set_write_handler(o, w->fd, !!(event & AVAHI_WATCH_OUT));
    o->set_except_handler(o, w->fd, !!(event & AVAHI_WATCH_ERR));
}

static AvahiWatch *
gensio_avahi_watch_new(const AvahiPoll *ap, int fd,
		       AvahiWatchEvent event, AvahiWatchCallback callback,
		       void *userdata)
{
    struct gensio_avahi_userdata *u = ap->userdata;
    struct gensio_os_funcs *o = u->o;
    AvahiWatch *aw;
    int err;

    aw = o->zalloc(o, sizeof(*aw));
    if (!aw)
	return NULL;
    
    aw->u = u;
    aw->fd = fd;
    aw->events = event;
    aw->callback = callback;
    aw->userdata = userdata;

    err = o->set_fd_handlers(o, fd, aw, gensio_avahi_read_handler,
			     gensio_avahi_write_handler,
			     gensio_avahi_except_handler,
			     gensio_avahi_cleared_handler);
    if (err) {
	o->free(o, aw);
	return NULL;
    }
    u->refcount++;

    gensio_avahi_watch_update(aw, event);

    return aw;
}

static AvahiWatchEvent
gensio_avahi_watch_get_events(AvahiWatch *w)
{
    return w->events;
}

static void
gensio_avahi_watch_free(AvahiWatch *w)
{
    struct gensio_avahi_userdata *u = w->u;
    struct gensio_os_funcs *o = u->o;

    assert(!w->freed);
    w->freed = true;
    o->clear_fd_handlers(o, w->fd);
}

struct AvahiTimeout {
    struct gensio_avahi_userdata *u;
    struct gensio_timer *t;
    AvahiTimeoutCallback callback;
    struct timeval tv;
    void *userdata;
    bool stopped;
    bool in_update;
    bool freed;
};

static void
gensio_avahi_timeout(struct gensio_timer *t, void *cb_data)
{
    AvahiTimeout *at = cb_data;
    struct gensio_avahi_userdata *u = at->u;
    struct gensio_os_funcs *o = u->o;

    o->lock(u->lock);
    if (!at->stopped)
	at->callback(at, at->userdata);
    o->unlock(u->lock);
}

static int
tv_cmp(struct timeval *tv1, struct timeval *tv2)
{
    if (tv1->tv_sec < tv2->tv_sec)
	return -1;
    if (tv1->tv_sec > tv2->tv_sec)
	return 1;
    if (tv1->tv_usec < tv2->tv_usec)
	return -1;
    if (tv1->tv_usec > tv2->tv_usec)
	return 1;
    return 0;
}

static void
do_timer_start(AvahiTimeout *at)
{
    struct gensio_avahi_userdata *u = at->u;
    struct gensio_os_funcs *o = u->o;
    struct timeval now, *tv = &at->tv;
    gensio_time gt = { tv->tv_sec, tv->tv_usec * 1000 };

    gettimeofday(&now, NULL);
    if (tv_cmp(tv, &now) <= 0) {
	gt.secs = 0;
	gt.nsecs = 0;
    } else {
	gt.secs = tv->tv_sec - now.tv_sec;
	gt.nsecs = (tv->tv_usec - now.tv_usec) * 1000;
	if (gt.nsecs < 0) {
	    gt.nsecs += 1000000000;
	    gt.secs -= 1;
	}
    }
    o->start_timer(at->t, &gt);
}

static void
i_gensio_avahi_timer_stopped(AvahiTimeout *at)
{
    struct gensio_avahi_userdata *u = at->u;
    struct gensio_os_funcs *o = u->o;

    if (at->freed) {
	o->free_timer(at->t);
	o->free(o, at);
	gensio_avahi_poll_deref(u->ap);
    } else if (at->in_update) {
	at->in_update = false;
	if (!at->stopped)
	    do_timer_start(at);
    }
}

static void
gensio_avahi_timer_stopped(struct gensio_timer *timer, void *userdata)
{
    AvahiTimeout *at = userdata;
    struct gensio_avahi_userdata *u = at->u;
    struct gensio_os_funcs *o = u->o;

    o->lock(u->lock);
    i_gensio_avahi_timer_stopped(at);
    o->unlock(u->lock);
}

static void
gensio_avahi_timeout_update(AvahiTimeout *at, const struct timeval *tv)
{
    struct gensio_avahi_userdata *u = at->u;
    struct gensio_os_funcs *o = u->o;

    if (tv) {
	at->tv = *tv;
	at->stopped = false;
    } else {
	if (at->stopped)
	    return;
	at->stopped = true;
    }

    if (!at->in_update) {
	at->in_update = true;
	if (o->stop_timer_with_done(at->t, gensio_avahi_timer_stopped, at) ==
		GE_TIMEDOUT)
	    i_gensio_avahi_timer_stopped(at);
    }
}

static AvahiTimeout *
gensio_avahi_timeout_new(const AvahiPoll *ap, const struct timeval *tv,
			 AvahiTimeoutCallback callback, void *userdata)
{
    struct gensio_avahi_userdata *u = ap->userdata;
    struct gensio_os_funcs *o = u->o;
    AvahiTimeout *at;

    at = o->zalloc(o, sizeof(*at));
    if (!at)
	return NULL;

    at->t = o->alloc_timer(o, gensio_avahi_timeout, at);
    if (!at->t) {
	o->free(o, at);
	return NULL;
    }

    at->u = u;
    at->callback = callback;
    at->userdata = userdata;
    u->refcount++;
    at->stopped = true;

    gensio_avahi_timeout_update(at, tv);

    return at;
}

static void
gensio_avahi_timeout_free(AvahiTimeout *at)
{
    struct gensio_avahi_userdata *u = at->u;
    struct gensio_os_funcs *o = u->o;

    if (at->freed)
	return;
    at->freed = true;
    at->stopped = true;
    if (o->stop_timer_with_done(at->t, gensio_avahi_timer_stopped, at) ==
		GE_TIMEDOUT) {
	o->free_timer(at->t);
	gensio_avahi_poll_deref(u->ap);
    }
}

static void
gensio_avahi_poll_runner(struct gensio_runner *runner, void *userdata)
{
    struct AvahiPoll *ap = userdata;
    struct gensio_avahi_userdata *u = ap->userdata;
    struct gensio_os_funcs *o = u->o;

    /* Make sure all users are out of their locks. */
    o->lock(u->lock);
    o->unlock(u->lock);

    if (u->stop_done)
	u->stop_done(ap, u->stop_userdata);
    o->free_runner(u->runner);
    o->free_lock(u->lock);
    o->free(o, u);
    o->free(o, ap);
}

struct AvahiPoll *
alloc_gensio_avahi_poll(struct gensio_os_funcs *o)
{
    struct gensio_avahi_userdata *u;
    struct AvahiPoll *ap;

    ap = o->zalloc(o, sizeof(*ap));
    if (!ap)
	return NULL;

    u = o->zalloc(o, sizeof(*u));
    if (!u) {
	o->free(o, ap);
	return NULL;
    }

    u->o = o;
    u->refcount = 1;
    u->ap = ap;

    u->lock = o->alloc_lock(o);
    if (!u->lock) {
	o->free(o, u);
	o->free(o, ap);
	return NULL;
    }

    u->runner = o->alloc_runner(o, gensio_avahi_poll_runner, ap);
    if (!u->runner) {
	o->free_lock(u->lock);
	o->free(o, u);
	o->free(o, ap);
	return NULL;
    }

    ap->userdata = u;
    ap->watch_new = gensio_avahi_watch_new;
    ap->watch_update = gensio_avahi_watch_update;
    ap->watch_get_events = gensio_avahi_watch_get_events;
    ap->watch_free = gensio_avahi_watch_free;
    ap->timeout_new = gensio_avahi_timeout_new;
    ap->timeout_update = gensio_avahi_timeout_update;
    ap->timeout_free = gensio_avahi_timeout_free;

    return ap;
}

void
gensio_avahi_poll_free(AvahiPoll *ap,
		       gensio_avahi_done done, void *userdata)
{
    struct gensio_avahi_userdata *u = ap->userdata;

    if (u->stopped)
	return;
    u->stopped = true;
    u->stop_done = done;
    u->stop_userdata = userdata;
    gensio_avahi_poll_deref(ap);
}
#endif
