/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "min_heap.h"
#include "evsignal.h"
#include "mm-internal.h"

/* map union members back */

/* mutually exclusive */
#define ev_signal_next	_ev.ev_signal.ev_signal_next
#define ev_io_next	_ev.ev_io.ev_io_next
#define ev_aio_next	_ev.ev_aio.ev_aio_next
#define ev_submitted_next	_ev.ev_aio.ev_submitted_next

/* used only by signals */
#define ev_ncalls	_ev.ev_signal.ev_ncalls
#define ev_pncalls	_ev.ev_signal.ev_pncalls

struct eventop {
	const char *name;
	void *(*init)(struct event_base *);
	int (*add)(struct event_base *, evutil_socket_t fd, short old, short events);
	int (*del)(struct event_base *, evutil_socket_t fd, short old, short events);
	int (*dispatch)(struct event_base *, struct timeval *);
	void (*dealloc)(struct event_base *);
	/* set if we need to reinitialize the event base */
	int need_reinit;
	enum event_method_feature features;
};

#ifdef WIN32
#define EVMAP_USE_HT
#endif

#ifdef EVMAP_USE_HT
#include "ht-internal.h"
struct event_map_entry;
HT_HEAD(event_io_map, event_map_entry);
#else
#define event_io_map event_signal_map
#endif

/* Used to map signal numbers to a list of events.  If EVMAP_USE_HT is not
   defined, this is also used as event_io_map, to map fds to a list of events.
*/
struct event_signal_map {
	void **entries;
	int nentries;
};

struct eventaioop {
	const char *name;
	void *(*init)(struct event_base *);
	void (*dealloc)(struct event_base *, void*);
	void (*prepare_read)(struct event *, int fd, void *buf, size_t length, off_t offset, int pri);
	void (*prepare_write)(struct event *, int fd, void *buf, size_t length, off_t offset, int pri);
	void (*submit)(struct event_base *);
	void (*cancel)(struct event_base *, struct event *ev);
	int need_direct_notification;
};

struct event_base {
	const struct eventop *evsel;
	const struct eventaioop *evaiosel;
	void *evbase;

	/* signal handling info */
	const struct eventop *evsigsel;
	void *evsigbase;

	struct evsig_info sig;

	void *evaiobase;

	int event_count;		/* counts number of total events */
	int event_count_active;	/* counts number of active events */

	int event_gotterm;		/* Set to terminate loop */
	int event_break;		/* Set to terminate loop immediately */

	/* active event management */
	struct event_list **activequeues;
	int nactivequeues;

	/* for mapping io activity to events */
	struct event_io_map io;

	/* for mapping signal activity to events */
	struct event_signal_map sigmap;

	/* AIO queues */
	struct event_list aioqueue;
	struct event_list submittedqueue;

	struct event_list eventqueue;
	struct timeval event_tv;

	struct min_heap timeheap;

	struct timeval tv_cache;
	
	/* threading support */
	unsigned long th_owner_id;
	unsigned long (*th_get_id)(void);
	void *th_base_lock;

	/* allocate/free locks */
	void *(*th_alloc)(void);
	void (*th_free)(void *lock);

	/* lock or unlock a lock */
	void (*th_lock)(int mode, void *lock);
	int th_notify_fd[2];
	struct event th_notify;
};

struct event_config_entry {
	TAILQ_ENTRY(event_config_entry) (next);

	const char *avoid_method;
};

struct event_config {
	TAILQ_HEAD(event_configq, event_config_entry) entries;

	enum event_method_feature require_features;
};

/* Internal use only: Functions that might be missing from <sys/queue.h> */
#ifndef HAVE_TAILQFOREACH
#define	TAILQ_FIRST(head)		((head)->tqh_first)
#define	TAILQ_END(head)			NULL
#define	TAILQ_NEXT(elm, field)		((elm)->field.tqe_next)
#define TAILQ_FOREACH(var, head, field)					\
	for((var) = TAILQ_FIRST(head);					\
	    (var) != TAILQ_END(head);					\
	    (var) = TAILQ_NEXT(var, field))
#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
	(elm)->field.tqe_next = (listelm);				\
	*(listelm)->field.tqe_prev = (elm);				\
	(listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)
#endif /* TAILQ_FOREACH */

int _evsig_set_handler(struct event_base *base, int evsignal,
			  void (*fn)(int));
int _evsig_restore_handler(struct event_base *base, int evsignal);

int event_aio_get_events_to_submit(struct event_base *base, struct event**, int maxnent);

void event_aio_set_submitted(struct event*);

void event_aio_set_cancelled(struct event*);

void event_aio_set_ready(struct event*, int result, int error);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_INTERNAL_H_ */
