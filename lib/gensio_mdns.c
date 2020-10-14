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

#include "config.h"
#include <gensio/gensio_mdns.h>

#ifdef HAVE_AVAHI
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <gensio/gensio.h>
#include <gensio/gensio_list.h>
#include <gensio/argvutils.h>
#include "avahi_watcher.h"

/* Returns true on failure (error) */
static bool
dupstr(struct gensio_os_funcs *o, const char *src, char **dest)
{
    if (src) {
	char *ret = gensio_strdup(o, src);
	if (!ret)
	    return true;
	*dest = ret;
    }
    return false;
}

/* Returns true if str1 is NULL or if the strings compare. */
/* FIXME - extend this to add regexp handling. */
static bool
mdns_str_cmp(const char *str1, const char *str2)
{
    if (!str1)
	return true;

    return strcmp(str1, str2) == 0;
}

struct gensio_mdns_service;

struct gensio_mdns {
    struct gensio_os_funcs *o;
    AvahiPoll *ap;
    AvahiClient *ac;
    struct gensio_list services;
    struct gensio_list watches;
    AvahiClientState state;

    unsigned int refcount;

    bool freed;
    gensio_mdns_done free_done;
    void *free_userdata;

    bool runner_pending;
    struct gensio_runner *runner;
    struct gensio_list callbacks;
};

static void
gensio_mdns_vlog(struct gensio_mdns *m, enum gensio_log_levels l,
		 char *fmt, va_list ap)
{
    gensio_vlog(m->o, l, fmt, ap);
}

static void
gensio_mdns_log(struct gensio_mdns *m, enum gensio_log_levels l, char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    gensio_mdns_vlog(m, l, fmt, ap);
    va_end(ap);
}

static void
gensio_mdns_poll_freed(AvahiPoll *ap, void *userdata)
{
    struct gensio_mdns *m = userdata;
    struct gensio_os_funcs *o = m->o;

    /* Make sure everything is out of the lock. */
    gensio_avahi_lock(m->ap);
    gensio_avahi_unlock(m->ap);

    if (m->free_done)
	m->free_done(m, m->free_userdata);
    o->free_runner(m->runner);
    o->free(o, m);
}

static void
gensio_mdns_finish_free(struct gensio_mdns *m)
{
    avahi_client_free(m->ac);
    gensio_avahi_poll_free(m->ap, gensio_mdns_poll_freed, m);
}

static void
gensio_mdns_ref(struct gensio_mdns *m)
{
    m->refcount++;
}

static void
gensio_mdns_deref(struct gensio_mdns *m)
{
    /* Can't use this for the final deref. */
    assert(m->refcount > 1);
    m->refcount--;
}

static void
gensio_mdns_deref_and_unlock(struct gensio_mdns *m)
{
    AvahiPoll *ap = m->ap;

    assert(m->refcount > 0);
    m->refcount--;
    if (m->refcount == 0)
	gensio_mdns_finish_free(m);
    gensio_avahi_unlock(ap);
}

struct gensio_mdns_service {
    struct gensio_link link;
    struct gensio_mdns *m;

    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char *name;
    char *type;
    char *domain;
    char *host;
    int port;
    AvahiStringList *txt;

    /* Used to handle name collisions. */
    unsigned int nameseq;
    char *currname;

    AvahiEntryGroup *group;
};

static void avahi_add_service(struct gensio_mdns *m,
			      struct gensio_mdns_service *s);

/* Lock should already be held when calling this. */
static void
avahi_group_callback(AvahiEntryGroup *group, AvahiEntryGroupState state,
		     void *userdata)
{
    struct gensio_mdns_service *s = userdata;
    struct gensio_mdns *m = s->m;
    struct gensio_os_funcs *o = m->o;

    if (state == AVAHI_ENTRY_GROUP_COLLISION) {
	if (s->currname != s->name)
	    o->free(o, s->currname);
	s->nameseq++;
	s->currname = gensio_alloc_sprintf(o, "%s#%u", s->name, s->nameseq);
	if (!s->currname) {
	    gensio_mdns_log(m, GENSIO_LOG_ERR,
			    "Out of memory in group collision");
	    return;
	}
	avahi_add_service(m, s);
    }
    /* FIXME - handle other states. */
}

/* Must be called with the Avahi poll lock held. */
static void
avahi_add_service(struct gensio_mdns *m, struct gensio_mdns_service *s)
{
    int err;

    if (!s->group)
	s->group = avahi_entry_group_new(m->ac, avahi_group_callback, s);
    if (!s->group) {
	gensio_mdns_log(m, GENSIO_LOG_ERR,
			"Out of memory adding a service");
	return;
    }
    err = avahi_entry_group_add_service_strlst(s->group, s->interface,
					       s->protocol, 0,
					       s->currname, s->type,
					       s->domain, s->host,
					       s->port, s->txt);
    if (err) {
	gensio_mdns_log(m, GENSIO_LOG_ERR,
			"Error adding service strings: %s",
			avahi_strerror(err));
	return;
    }
    err = avahi_entry_group_commit(s->group);
    if (err)
	gensio_mdns_log(m, GENSIO_LOG_ERR,
			"Error commiting service entry: %s",
			avahi_strerror(err));
}

static void
free_service(struct gensio_os_funcs *o, struct gensio_mdns_service *s)
{
    if (s->group)
	avahi_entry_group_free(s->group);
    if (s->currname && s->currname != s->name)
	o->free(o, s->currname);
    if (s->name)
	o->free(o, s->name);
    if (s->type)
	o->free(o, s->type);
    if (s->domain)
	o->free(o, s->domain);
    if (s->host)
	o->free(o, s->host);
    if (s->txt)
	avahi_string_list_free(s->txt);
}

static int
i_gensio_mdns_remove_service(struct gensio_mdns_service *s)
{
    struct gensio_mdns *m = s->m;

    gensio_list_rm(&m->services, &s->link);
    free_service(m->o, s);

    return 0;
}

int
gensio_mdns_remove_service(struct gensio_mdns_service *s)
{
    struct gensio_mdns *m = s->m;
    int err;

    gensio_avahi_lock(m->ap);
    err = i_gensio_mdns_remove_service(s);
    gensio_avahi_unlock(m->ap);

    return err;
}

int
gensio_mdns_add_service(struct gensio_mdns *m,
			int interface, int ipdomain,
			const char *name, const char *type,
			const char *domain, const char *host,
			int port, const char *txt[],
			struct gensio_mdns_service **rservice)
{
    struct gensio_os_funcs *o = m->o;
    struct gensio_mdns_service *s;
    AvahiProtocol protocol;
    int err = GE_NOMEM;

    switch(ipdomain) {
    case GENSIO_NETTYPE_IPV4: protocol = AVAHI_PROTO_INET; break;
    case GENSIO_NETTYPE_IPV6: protocol = AVAHI_PROTO_INET6; break;
    case GENSIO_NETTYPE_UNSPEC: protocol = AVAHI_PROTO_UNSPEC; break;
    default:
	return GE_INVAL;
    }

    if (interface < 0)
	interface = AVAHI_IF_UNSPEC;

    if (!name || !type)
	return GE_INVAL;

    s = o->zalloc(m->o, sizeof(*s));
    if (!s)
	return GE_NOMEM;

    s->m = m;
    s->interface = interface;
    s->protocol = protocol;
    s->port = port;
    s->name = gensio_strdup(o, name);
    if (!s->name)
	goto out_err;
    s->type = gensio_strdup(o, type);
    if (!s->type)
	goto out_err;
    if (dupstr(o, domain, &s->domain))
	goto out_err;
    if (dupstr(o, host, &s->host))
	goto out_err;

    if (txt && txt[0]) {
	s->txt = avahi_string_list_new_from_array(txt, -1);
	if (!s->txt)
	    goto out_err;
    }

    s->currname = s->name;

    gensio_avahi_lock(m->ap);
    gensio_list_add_tail(&m->services, &s->link);
    if (m->state == AVAHI_CLIENT_S_RUNNING)
	avahi_add_service(m, s);
    gensio_avahi_unlock(m->ap);

    if (rservice)
	*rservice = s;

    return 0;

 out_err:
    free_service(o, s);
    return err;
}

struct gensio_mdns_result;

struct gensio_mdns_watch_data {
    struct gensio_mdns_result *result;

    enum gensio_mdns_data_state state;

    int interface;
    int ipdomain;
    char *name;
    char *type;
    char *domain;
    char *host;
    struct gensio_addr *addr;
    const char **txt;
};

static void
gensio_mdns_free_watch_data(struct gensio_os_funcs *o,
			    struct gensio_mdns_watch_data *d)
{
    if (d->name)
	o->free(o, d->name);
    if (d->type)
	o->free(o, d->type);
    if (d->domain)
	o->free(o, d->domain);
    if (d->host)
	o->free(o, d->host);
    if (d->addr)
	gensio_addr_free(d->addr);
    if (d->txt)
	gensio_argv_free(o, d->txt);
    o->free(o, d);
}

struct gensio_mdns_callback {
    struct gensio_link link;

    bool in_queue;

    bool remove;		/* Remove the watch. */

    /* Report that we are done with the initial scan. */
    bool all_for_now;

    struct gensio_mdns_watch *w;

    struct gensio_mdns_watch_data *data;
};

struct gensio_mdns_watch_resolver;
struct gensio_mdns_result {
    struct gensio_link link;
    struct gensio_mdns_watch_resolver *resolver;
    struct gensio_mdns_callback cbdata;
    AvahiAddress addr;
    uint16_t port;
};

static void
result_free(struct gensio_os_funcs *o, struct gensio_mdns_result *e)
{
    if (e->cbdata.data)
	gensio_mdns_free_watch_data(o, e->cbdata.data);
    o->free(o, e);
}

struct gensio_mdns_watch_resolver {
    struct gensio_link link;
    struct gensio_mdns_watch_browser *b;
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char *name;
    char *type;
    char *domain;
    AvahiServiceResolver *resolver;
    struct gensio_list results;
};

struct gensio_mdns_watch_browser {
    struct gensio_link link;
    struct gensio_mdns_watch *w;
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char *type;
    char *domain;
    AvahiServiceBrowser *browser;
    struct gensio_list resolvers;
};

struct gensio_mdns_watch {
    struct gensio_link link;

    struct gensio_mdns *m;
    AvahiServiceTypeBrowser *browser;
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char *name;
    char *type;
    char *domain;
    char *host;

    bool removed;

    unsigned int service_calls_pending;

    gensio_mdns_watch_cb cb;
    void *userdata;

    struct gensio_mdns_callback callback_data;

    gensio_mdns_watch_done remove_done;
    void *remove_done_data;

    struct gensio_list browsers;
};

static void
enqueue_callback(struct gensio_mdns *m, struct gensio_mdns_callback *c)
{
    if (!c->in_queue) {
	gensio_list_add_tail(&m->callbacks, &c->link);
	c->in_queue = true;
	gensio_mdns_ref(m);
    }
    if (!m->runner_pending) {
	m->runner_pending = true;
	gensio_mdns_ref(m);
	m->o->run(m->runner);
    }
}

static void
resolver_free(struct gensio_os_funcs *o, struct gensio_mdns_watch_resolver *r)
{
    if (r->resolver)
	avahi_service_resolver_free(r->resolver);
    if (r->name)
	o->free(o, r->name);
    if (r->type)
	o->free(o, r->type);
    if (r->domain)
	o->free(o, r->domain);
    o->free(o, r);
}

/* Will be called with the lock held. */
static void
mdns_service_resolver_callback(AvahiServiceResolver *ar,
			       AvahiIfIndex interface,
			       AvahiProtocol protocol,
			       AvahiResolverEvent event,
			       const char *name,
			       const char *type,
			       const char *domain,
			       const char *host,
			       const AvahiAddress *a,
			       uint16_t port,
			       AvahiStringList *txt,
			       AvahiLookupResultFlags flags,
			       void *userdata)
{
    struct gensio_mdns_watch_resolver *r = userdata;
    struct gensio_mdns_watch_browser *b = r->b;
    struct gensio_mdns_watch *w = b->w;
    struct gensio_mdns *m = w->m;
    struct gensio_mdns_callback *c = NULL;
    struct gensio_os_funcs *o = m->o;
    struct gensio_mdns_result *e;
    enum gensio_mdns_data_state state;
    AvahiStringList *str;
    int nettype, netsize, rv;

    switch (event) {
    case AVAHI_RESOLVER_FOUND:
	state = GENSIO_MDNS_NEW_DATA;
	break;

    case AVAHI_RESOLVER_FAILURE:
	gensio_mdns_log(m, GENSIO_LOG_ERR,
			"Error from resolver: %s",
			avahi_strerror(avahi_client_errno(m->ac)));
    default:
	return;
    }

    switch(a->proto) {
    case AVAHI_PROTO_INET:
	nettype = GENSIO_NETTYPE_IPV4;
	netsize = sizeof(a->data.ipv4);
	break;
	
    case AVAHI_PROTO_INET6:
	nettype = GENSIO_NETTYPE_IPV6;
	netsize = sizeof(a->data.ipv6);
	break;
	
    default:
	return;
    }

    if (!mdns_str_cmp(w->host, host))
	return;

    e = o->zalloc(o, sizeof(*e));
    if (!e)
	goto out_nomem;
    e->resolver = r;
    e->addr = *a;
    e->port = port;

    c = &e->cbdata;
    c->w = w;
    c->data = o->zalloc(o, sizeof(*(c->data)));
    if (!c->data)
	goto out_nomem;
    c->data->result = e;
    c->data->state = state;
    c->data->ipdomain = nettype;
    if (dupstr(o, name, &c->data->name))
	goto out_nomem;
    if (dupstr(o, type, &c->data->type))
	goto out_nomem;
    if (dupstr(o, domain, &c->data->domain))
	goto out_nomem;
    if (dupstr(o, host, &c->data->host))
	goto out_nomem;

    if (gensio_addr_create(o, nettype, &a->data, netsize, port, &c->data->addr))
	goto out_nomem;

    if (txt) {
	gensiods args = 0, argc = 0;

	for (str = txt; str; str = str->next) {
	    rv = gensio_argv_append(o, &c->data->txt, (char *) str->text,
				    &args, &argc, true);
	    if (rv)
		goto out_nomem;
	}
	rv = gensio_argv_append(o, &c->data->txt, NULL, &args, &argc, false);
	if (rv)
	    goto out_nomem;
    }

    gensio_list_add_tail(&r->results, &e->link);
    enqueue_callback(m, c);
    
    return;

 out_nomem:
    if (c && c->data)
	gensio_mdns_free_watch_data(o, c->data);
}

static void
browser_finish_one(struct gensio_mdns_watch *w)
{
    struct gensio_mdns *m = w->m;

    assert(w->service_calls_pending > 0);
    w->service_calls_pending--;
    if (w->service_calls_pending == 0) {
	w->callback_data.all_for_now = true;
	enqueue_callback(m, &w->callback_data);
    }
}

static void
browser_free(struct gensio_os_funcs *o, struct gensio_mdns_watch_browser *b)
{
    if (b->browser)
	avahi_service_browser_free(b->browser);
    if (b->type)
	o->free(o, b->type);
    if (b->domain)
	o->free(o, b->domain);
    o->free(o, b);
}

static void
resolver_remove(struct gensio_mdns_watch_resolver *r)
{
    struct gensio_mdns_watch_browser *b = r->b;
    struct gensio_mdns_watch *w = b->w;
    struct gensio_mdns *m = w->m;
    struct gensio_os_funcs *o = m->o;
    struct gensio_link *l, *l2;

    gensio_list_for_each_safe(&r->results, l, l2) {
	struct gensio_mdns_result *e =
	    gensio_container_of(l, struct gensio_mdns_result, link);

	gensio_list_rm(&r->results, &e->link);
	if (e->cbdata.in_queue) {
	    if (e->cbdata.data->state == GENSIO_MDNS_NEW_DATA) {
		/* In queue but not reported, just remove it. */
		gensio_list_rm(&m->callbacks, &e->cbdata.link);
		gensio_mdns_deref(m);
		result_free(o, e);
	    }
	    /* Otherwise already scheduled for removal. */
	} else {
	    /* Report removal */
	    e->cbdata.data->state = GENSIO_MDNS_DATA_GONE;
	    enqueue_callback(m, &e->cbdata);
	}
    }
    gensio_list_rm(&b->resolvers, &r->link);
    resolver_free(o, r);
}

static void
mdns_service_browser_callback(AvahiServiceBrowser *ab,
			      AvahiIfIndex interface,
			      AvahiProtocol protocol,
			      AvahiBrowserEvent event,
			      const char *name,
			      const char *type,
			      const char *domain,
			      AvahiLookupResultFlags flags,
			      void *userdata)
{
    struct gensio_mdns_watch_browser *b = userdata;
    struct gensio_mdns_watch *w = b->w;
    struct gensio_mdns *m = w->m;
    struct gensio_os_funcs *o = m->o;
    struct gensio_link *l;
    struct gensio_mdns_watch_resolver *r = NULL;

    switch (event) {
    case AVAHI_BROWSER_NEW:
    case AVAHI_BROWSER_REMOVE:
	/* Handle this one below. */
	break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
	browser_finish_one(w);
	return;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
    default:
	return;

    case AVAHI_BROWSER_FAILURE:
	gensio_mdns_log(m, GENSIO_LOG_ERR,
			"Error from browser: %s",
			avahi_strerror(avahi_client_errno(m->ac)));
	return;
    }

    /* See if it aready exists. */
    gensio_list_for_each(&b->resolvers, l) {
	r = gensio_container_of(l, struct gensio_mdns_watch_resolver, link);

	if (r->interface == interface && r->protocol == protocol &&
		strcmp(r->name, name) == 0 &&
		strcmp(r->type, type) == 0 &&
		strcmp(r->domain, domain) == 0)
	    break;
	else
	    r = NULL;
    }

    if (event == AVAHI_BROWSER_REMOVE) {
	/* If we have the resolver, remove it. */
	if (r)
	    resolver_remove(r);
	return;
    }

    if (r)
	return; /* We already have it. */

    if (w->interface != -1 && interface != w->interface)
	return;
    if (w->protocol != AVAHI_PROTO_UNSPEC && protocol != w->protocol)
	return;
    if (!mdns_str_cmp(w->name, name))
	return;
    if (!mdns_str_cmp(w->type, type))
	return;
    if (!mdns_str_cmp(w->domain, domain))
	return;

    r = o->zalloc(o, sizeof(*r));
    if (!r)
	goto out_err;

    gensio_list_init(&r->results);
    r->b = b;
    r->interface = interface;
    r->protocol = protocol;

    if (dupstr(o, name, &r->name))
	goto out_err;
    if (dupstr(o, type, &r->type))
	goto out_err;
    if (dupstr(o, domain, &r->domain))
	goto out_err;

    gensio_list_add_tail(&b->resolvers, &r->link);
    r->resolver = avahi_service_resolver_new(m->ac, interface, protocol,
					     name, type, domain, w->protocol,
					     0, mdns_service_resolver_callback,
					     r);
    if (!r->resolver) {
	gensio_list_rm(&b->resolvers, &r->link);
	goto out_err;
    }
    return;

 out_err:
    gensio_mdns_log(m, GENSIO_LOG_ERR,
		    "Out of memory allocating browser");
    if (r)
	resolver_free(o, r);
}

/* Will be called with the lock held. */
static void
mdns_service_type_callback(AvahiServiceTypeBrowser *ab,
			   AvahiIfIndex interface,
			   AvahiProtocol protocol,
			   AvahiBrowserEvent event,
			   const char *type,
			   const char *domain,
			   AvahiLookupResultFlags flags,
			   void *userdata)
{
    struct gensio_mdns_watch *w = userdata;
    struct gensio_mdns *m = w->m;
    struct gensio_os_funcs *o = m->o;
    struct gensio_link *l;
    struct gensio_mdns_watch_browser *b = NULL;

    switch (event) {
    case AVAHI_BROWSER_NEW:
    case AVAHI_BROWSER_REMOVE:
	/* Handle this one below. */
	break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
	browser_finish_one(w);
	return;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
    default:
	return;

    case AVAHI_BROWSER_FAILURE:
	gensio_mdns_log(m, GENSIO_LOG_ERR,
			"Error from type browser: %s",
			avahi_strerror(avahi_client_errno(m->ac)));
	return;
    }

    /* If we have the resolver set, remove it. */
    gensio_list_for_each(&w->browsers, l) {
	b = gensio_container_of(l, struct gensio_mdns_watch_browser, link);

	if (b->interface == interface && b->protocol == protocol &&
		strcmp(b->type, type) == 0 &&
		strcmp(b->domain, domain) == 0)
	    break;
	else
	    b = NULL;
    }

    if (event == AVAHI_BROWSER_REMOVE) {
	if (b) {
	    struct gensio_link *l, *l2;

	    gensio_list_for_each_safe(&b->resolvers, l, l2) {
		struct gensio_mdns_watch_resolver *r =
		    gensio_container_of(l, struct gensio_mdns_watch_resolver,
					link);

		resolver_remove(r);
	    }
	    gensio_list_rm(&w->browsers, &b->link);
	    browser_free(o, b);
	}
	return;
    }
    if (b)
	return; /* We already have it. */

    if (w->interface != -1 && interface != w->interface)
	return;
    if (w->protocol != AVAHI_PROTO_UNSPEC && protocol != w->protocol)
	return;
    if (!mdns_str_cmp(w->type, type))
	return;
    if (!mdns_str_cmp(w->domain, domain))
	return;

    b = o->zalloc(o, sizeof(*b));

    if (!b)
	goto out_err;

    gensio_list_init(&b->resolvers);
    b->w = w;
    b->interface = interface;
    b->protocol = protocol;

    if (dupstr(o, type, &b->type))
	goto out_err;
    if (dupstr(o, domain, &b->domain))
	goto out_err;

    gensio_list_add_tail(&w->browsers, &b->link);
    w->service_calls_pending++;
    b->browser = avahi_service_browser_new(m->ac, interface, protocol,
					   type, domain,
					   0, mdns_service_browser_callback,
					   b);
    if (!b->browser) {
	gensio_list_rm(&w->browsers, &b->link);
	w->service_calls_pending--;
	goto out_err;
    }
    return;

 out_err:
    gensio_mdns_log(m, GENSIO_LOG_ERR,
		    "Out of memory allocating service type browser");
    if (b)
	browser_free(o, b);
}

static void
watch_free(struct gensio_os_funcs *o, struct gensio_mdns_watch *w)
{
    if (w->host)
	o->free(o, w->host);
    if (w->domain)
	o->free(o, w->domain);
    if (w->type)
	o->free(o, w->type);
    if (w->name)
	o->free(o, w->name);
    o->free(o, w);
}

static void
avahi_add_watch(struct gensio_mdns_watch *w)
{
    struct gensio_mdns *m = w->m;

    w->browser = avahi_service_type_browser_new(m->ac, w->interface,
						w->protocol, w->domain, 0,
						mdns_service_type_callback, w);
    if (w->browser)
	w->service_calls_pending++;
}

int				    
gensio_mdns_add_watch(struct gensio_mdns *m,
		      int interface, int ipdomain,
		      const char *name, const char *type,
		      const char *domain, const char *host,
		      gensio_mdns_watch_cb callback, void *userdata,
		      struct gensio_mdns_watch **rwatch)
{
    struct gensio_mdns_watch *w;
    struct gensio_os_funcs *o = m->o;
    AvahiProtocol protocol;

    switch(ipdomain) {
    case GENSIO_NETTYPE_IPV4: protocol = AVAHI_PROTO_INET; break;
    case GENSIO_NETTYPE_IPV6: protocol = AVAHI_PROTO_INET6; break;
    case GENSIO_NETTYPE_UNSPEC: protocol = AVAHI_PROTO_UNSPEC; break;
    default:
	return GE_INVAL;
    }

    if (interface < 0)
	interface = AVAHI_IF_UNSPEC;

    w = o->zalloc(o, sizeof(*w));
    if (!w)
	return GE_NOMEM;

    w->m = m;
    w->cb = callback;
    w->callback_data.w = w;
    w->userdata = userdata;
    w->interface = interface;
    w->protocol = protocol;
    gensio_list_init(&w->browsers);

    if (dupstr(o, name, &w->name))
	goto out_err;
    if (dupstr(o, type, &w->type))
	goto out_err;
    if (dupstr(o, domain, &w->domain))
	goto out_err;
    if (dupstr(o, host, &w->host))
	goto out_err;

    gensio_avahi_lock(m->ap);
    if (m->state == AVAHI_CLIENT_S_RUNNING) {
	avahi_add_watch(w);
	gensio_list_add_tail(&m->watches, &w->link);
    } else if (w->browser)
	gensio_list_add_tail(&m->watches, &w->link);
    gensio_avahi_unlock(m->ap);
    if (m->state == AVAHI_CLIENT_S_RUNNING && !w->browser)
	goto out_err;

    *rwatch = w;
    return 0;

 out_err:
    watch_free(o, w);
    return GE_NOMEM;
}

static int
i_gensio_mdns_remove_watch(struct gensio_mdns_watch *w,
			   gensio_mdns_watch_done done, void *userdata)
{
    struct gensio_mdns *m = w->m;
    struct gensio_os_funcs *o = m->o;
    struct gensio_link *l, *l2;
    struct gensio_link *li, *li2;
    struct gensio_link *lj, *lj2;
    struct gensio_mdns_watch_resolver *r;
    struct gensio_mdns_watch_browser *b;
    struct gensio_mdns_result *e;

    w->removed = true;
    w->remove_done = done;
    w->remove_done_data = userdata;
    gensio_list_rm(&m->watches, &w->link);

    gensio_list_for_each_safe(&w->browsers, l, l2) {
	b = gensio_container_of(l, struct gensio_mdns_watch_browser, link);

	gensio_list_for_each_safe(&b->resolvers, li, li2) {
	    r = gensio_container_of(li, struct gensio_mdns_watch_resolver,
				    link);
	    gensio_list_for_each_safe(&r->results, lj, lj2) {
		e = gensio_container_of(lj, struct gensio_mdns_result, link);
		if (e->cbdata.in_queue)
		    gensio_list_rm(&m->callbacks, &e->cbdata.link);
		gensio_list_rm(&r->results, &e->link);
		result_free(o, e);
	    }
	    gensio_list_rm(&b->resolvers, &r->link);
	    resolver_free(o, r);
	}
	gensio_list_rm(&w->browsers, &b->link);
	browser_free(o, b);
    }
    w->callback_data.remove = true;
    enqueue_callback(m, &w->callback_data);

    return 0;
}

int
gensio_mdns_remove_watch(struct gensio_mdns_watch *w,
			 gensio_mdns_watch_done done, void *userdata)
{
    struct gensio_mdns *m = w->m;
    int err;

    gensio_avahi_lock(m->ap);
    if (w->removed)
	err = GE_INUSE;
    else
	err = i_gensio_mdns_remove_watch(w, done, userdata);
    gensio_avahi_unlock(m->ap);

    return err;
}

/* Lock should already be held when calling this. */
static void
mdns_client_callback(AvahiClient *ac, AvahiClientState state, void *userdata)
{
    struct gensio_mdns *m = userdata;
    struct gensio_link *l;
    struct gensio_mdns_service *s;
    struct gensio_mdns_watch *w;

    if (m->state == state)
	return;
    m->state = state;

    if (state == AVAHI_CLIENT_S_RUNNING) {
	gensio_list_for_each(&m->services, l) {
	    s = gensio_container_of(l, struct gensio_mdns_service, link);

	    avahi_add_service(m, s);
	}
	gensio_list_for_each(&m->watches, l) {
	    w = gensio_container_of(l, struct gensio_mdns_watch, link);

	    avahi_add_watch(w);
	}
    }
    /* FIXME - handle other states. */
}

static void mdns_runner(struct gensio_runner *runner, void *userdata)
{
    struct gensio_mdns *m = userdata;
    struct gensio_os_funcs *o = m->o;
    struct gensio_link *l;
    struct gensio_mdns_callback *c;
    struct gensio_mdns_watch *w;

    gensio_avahi_lock(m->ap);
    while (!gensio_list_empty(&m->callbacks)) {
	l = gensio_list_first(&m->callbacks);
	c = gensio_container_of(l, struct gensio_mdns_callback, link);
	w = c->w;
	gensio_list_rm(&m->callbacks, &c->link);
	c->in_queue = false;
	gensio_mdns_deref(m);

	if (c->remove) {
	    if (w->remove_done) {
		gensio_avahi_unlock(m->ap);
		w->remove_done(w, w->remove_done_data);
		gensio_avahi_lock(m->ap);
	    }
	    watch_free(o, w);
	} else {
	    if (c->data) {
		struct gensio_mdns_watch_data *d = c->data;

		if (!m->freed && !w->removed) {
		    gensio_avahi_unlock(m->ap);
		    w->cb(w, d->state, d->interface, d->ipdomain, d->name,
			  d->type, d->domain, d->host, d->addr, d->txt,
			  w->userdata);
		    gensio_avahi_lock(m->ap);
		}
		if (d->state == GENSIO_MDNS_DATA_GONE)
		    result_free(o, d->result);
	    } else if (c->all_for_now) {
		c->all_for_now = false;
		gensio_avahi_unlock(m->ap);
		w->cb(w, GENSIO_MDNS_ALL_FOR_NOW, 0, 0, NULL,
		      NULL, NULL, NULL, NULL, NULL, w->userdata);
		gensio_avahi_lock(m->ap);
	    }
	}
    }
    m->runner_pending = false;
    gensio_mdns_deref_and_unlock(m);
}

int
gensio_alloc_mdns(struct gensio_os_funcs *o, struct gensio_mdns **new_m)
{
    struct gensio_mdns *m;

    m = o->zalloc(o, sizeof(*m));
    if (!m)
	return GE_NOMEM;

    m->o = o;
    m->refcount = 1;

    m->ap = alloc_gensio_avahi_poll(o);
    if (!m->ap) {
	o->free(o, m);
	return GE_NOMEM;
    }

    m->runner = o->alloc_runner(o, mdns_runner, m);
    if (!m->runner) {
	gensio_avahi_poll_free(m->ap, NULL, NULL);
	o->free(o, m);
	return GE_NOMEM;
    }	

    gensio_list_init(&m->services);
    gensio_list_init(&m->watches);
    gensio_list_init(&m->callbacks);

    gensio_avahi_lock(m->ap);
    m->ac = avahi_client_new(m->ap, AVAHI_CLIENT_NO_FAIL,
			     mdns_client_callback, m, NULL);
    gensio_avahi_unlock(m->ap);
    if (!m->ac) {
	gensio_avahi_poll_free(m->ap, NULL, NULL);
	o->free_runner(m->runner);
	o->free(o, m);
	return GE_NOMEM;
    }

    *new_m = m;
    return 0;
}

int
gensio_free_mdns(struct gensio_mdns *m, gensio_mdns_done done, void *userdata)
{
    struct gensio_os_funcs *o = m->o;
    struct gensio_link *l, *l2;
    struct gensio_mdns_service *s;
    struct gensio_mdns_watch *w;
    int err = 0;

    gensio_avahi_lock(m->ap);
    if (m->freed) {
	err = GE_INUSE;
	goto out_unlock;
    }
    m->freed = true;
    m->free_done = done;
    m->free_userdata = userdata;

    gensio_list_for_each_safe(&m->callbacks, l, l2) {
	struct gensio_mdns_callback *c =
	    gensio_container_of(l, struct gensio_mdns_callback, link);

	gensio_list_rm(&m->callbacks, &c->link);
	if (c->data && c->data->state == GENSIO_MDNS_DATA_GONE)
	    result_free(o, c->data->result);
    }

    gensio_list_for_each_safe(&m->services, l, l2) {
	s = gensio_container_of(l, struct gensio_mdns_service, link);
	i_gensio_mdns_remove_service(s);
    }

    gensio_list_for_each_safe(&m->watches, l, l2) {
	w = gensio_container_of(l, struct gensio_mdns_watch, link);
	i_gensio_mdns_remove_watch(w, NULL, NULL);
    }
    if (m->refcount == 1) {
	if (!m->runner_pending) {
	    m->runner_pending = true;
	    o->run(m->runner);
	}
    } else {
	gensio_mdns_deref(m);
    }
 out_unlock:
    gensio_avahi_unlock(m->ap);
    return err;
}

#else

int
gensio_alloc_mdns(struct gensio_os_funcs *o, struct gensio_mdns **m)
{
    return GE_NOTSUP;
}

int
gensio_free_mdns(struct gensio_mdns *m,
		 gensio_mdns_done done, void *userdata)
{
    return GE_NOTSUP;
}


int
gensio_mdns_add_service(struct gensio_mdns *m,
			int interface, int ipdomain,
			const char *name, const char *type,
			const char *domain, const char *host,
			int port, const char *txt[],
			struct gensio_mdns_service **rservice)
{
    return GE_NOTSUP;
}

int
gensio_mdns_remove_service(struct gensio_mdns_service *s)
{
    return GE_NOTSUP;
}

int
gensio_mdns_add_watch(struct gensio_mdns *m,
		      int interface, int ipdomain,
		      const char *name, const char *type,
		      const char *domain, const char *host,
		      gensio_mdns_watch_cb callback, void *userdata,
		      struct gensio_mdns_watch **rwatch)
{
    return GE_NOTSUP;
}

int
gensio_mdns_remove_watch(struct gensio_mdns_watch *w,
			 gensio_mdns_watch_done done, void *userdata)
{
    return GE_NOTSUP;
}

#endif
