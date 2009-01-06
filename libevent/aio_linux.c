#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_LIBAIO_H
#include <libaio.h>
#endif

#ifdef HAVE_SYS_EVENTFD_H
#include <sys/eventfd.h>
#endif

//#error "Linux native AIO support is broken now"


#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#include <assert.h>

#include "event2/event.h"
#include "event2/event_struct.h"
#include "event-internal.h"
#include "log.h"
#include "event-internal.h"


#ifndef  IOCB_FLAG_RESFD
# define IOCB_FLAG_RESFD		(1 << 0)
#endif

#define EVENT_LINUX_AIO_MAX_NENT 64

struct aio_linux_ctx {
	io_context_t ctx_id;
	size_t max_nent;
	int notify_fd;
	struct event notify_event;
	unsigned int notify_event_added:1;
	struct iocb **iocbs;
	struct event **events;
	struct io_event *io_events;
};

static void *aio_linux_init(struct event_base *base);
static void aio_linux_dealloc(struct event_base *base, void*);
static void aio_linux_prepare_read(struct event *ev, int fd, void *buf, size_t length, off_t offset, int pri);
static void aio_linux_prepare_write(struct event *ev, int fd, void *buf, size_t length, off_t offset, int pri);
static void aio_linux_submit(struct event_base *base);

static void aio_linux_process_one(struct event *ev, struct io_event *io_event);
static void aio_linux_process(struct event_base *base, uint64_t _hint);

static void aio_linux_cancel(struct event_base *base, struct event *ev);

const struct eventaioop aio_linux_ops = {
	"Linux AIO",
	aio_linux_init,
	aio_linux_dealloc,
	aio_linux_prepare_read,
	aio_linux_prepare_write,
	aio_linux_submit,
	aio_linux_cancel,
	0
};

static void
aio_linux_callback(int fd, short event, void *_base)
{
	uint64_t u = 0;
	int result;

	result = read(fd, &u, sizeof(uint64_t));

	assert (result == sizeof(uint64_t) || result == 0);

	aio_linux_process((struct event_base*)_base, u);
}

static void*
aio_linux_init(struct event_base *base)
{
	struct aio_linux_ctx *ctx;

	ctx = mm_calloc(1, sizeof(struct aio_linux_ctx) +
			sizeof(struct iocb*)   * EVENT_LINUX_AIO_MAX_NENT + 
			sizeof(struct event*)  * EVENT_LINUX_AIO_MAX_NENT +
			sizeof(struct io_event)* EVENT_LINUX_AIO_MAX_NENT);
	if(ctx == NULL)
		goto out;

	ctx->notify_fd = eventfd(0, 0); 
	if(ctx->notify_fd < 0)
		goto out_ctx;

	ctx->max_nent = EVENT_LINUX_AIO_MAX_NENT;
	ctx->iocbs     = (struct iocb **)(ctx + 1);
	ctx->events    = (struct event **)(ctx->iocbs + ctx->max_nent);
	ctx->io_events = (struct io_event *)(ctx->events + ctx->max_nent);

	event_assign(&ctx->notify_event, base, ctx->notify_fd, 
		     EV_READ|EV_PERSIST, aio_linux_callback, base);
	
	ctx->notify_event.ev_flags |= EVLIST_INTERNAL;

	if(io_setup(ctx->max_nent, &ctx->ctx_id) < 0)
		goto out_close;

	return (ctx);


out_close:
	close(ctx->notify_fd);

out_ctx:
	free(ctx);
out:
	return (NULL);
}

static void
aio_linux_dealloc(struct event_base *base, void *_evaiobase)
{
	struct aio_linux_ctx *ctx = (struct aio_linux_ctx *)_evaiobase;

	if(ctx->notify_event_added != 0)
		event_del(&ctx->notify_event);

	io_destroy(ctx->ctx_id);

	close(ctx->notify_fd);
}

static void
aio_linux_prepare_read(struct event *ev, int fd, void *buf, size_t length, off_t offset, int pri)
{
	io_prep_pread(&ev->_ev.ev_aio.iocb, fd, buf, length, offset);
	ev->_ev.ev_aio.iocb.aio_reqprio = pri;
	ev->_ev.ev_aio.iocb.data = ev;
	
#if 0
	memset(&ev->_ev.ev_aio.iocb, 0, sizeof(ev->_ev.ev_aio.iocb));
	ev->_ev.ev_aio.iocb->aio_fildes = fd;
	ev->_ev.ev_aio.iocb->aio_lio_opcode = IO_CMD_PREAD;
	ev->_ev.ev_aio.iocb->aio_reqprio = pri;
	ev->_ev.ev_aio.iocb->u.c.buf = buf;
	ev->_ev.ev_aio.iocb->u.c.nbytes = length;
	ev->_ev.ev_aio.iocb->u.c.offset = offset;
#endif
}

static void
aio_linux_prepare_write(struct event *ev, int fd, void *buf, size_t length, off_t offset, int pri)
{
	io_prep_pwrite(&ev->_ev.ev_aio.iocb, fd, buf, length, offset);
	ev->_ev.ev_aio.iocb.aio_reqprio = pri;
	ev->_ev.ev_aio.iocb.data = ev;

#if 0
	memset(ev->_ev.ev_aio.iocb, 0, sizeof(ev->_ev.ev_aio.iocb));
	ev->_ev.ev_aio.iocb->aio_fildes = fd;
	ev->_ev.ev_aio.iocb->aio_lio_opcode = IO_CMD_PWRITE;
	ev->_ev.ev_aio.iocb->aio_flags = IO_FLAG_RESFD;
	ev->_ev.ev_aio.iocb->aio_reqprio = pri;
	ev->_ev.ev_aio.iocb->u.c.buf = buf;
	ev->_ev.ev_aio.iocb->u.c.nbytes = length;
	ev->_ev.ev_aio.iocb->u.c.offset = offset;
#endif
}

static void
aio_linux_submit(struct event_base *base)
{
	struct aio_linux_ctx *ctx = (struct aio_linux_ctx *)base->evaiobase;
	int result, iosubmit_error;
	int nent;
	struct event *ev;
	int i;

	// Submit as many events as possible
	do{
		nent = 0;

		for (ev = TAILQ_FIRST(&base->aioqueue); ev && nent < ctx->max_nent;
		     ev = TAILQ_NEXT(ev, ev_aio_next))
		{
			ev->_ev.ev_aio.iocb.u.c.flags = IOCB_FLAG_RESFD;
			ev->_ev.ev_aio.iocb.u.c.resfd = ctx->notify_fd;

			ctx->iocbs[nent] = &ev->_ev.ev_aio.iocb;
			ctx->events[nent] = ev;
			nent++;
		}

		if(nent > 0) {
			if (ctx->notify_event_added == 0) {
				event_add(&ctx->notify_event, NULL);
				ctx->notify_event_added = 1;
			}

			result = io_submit(ctx->ctx_id, nent, ctx->iocbs);

			iosubmit_error = errno;

			if(result != nent) {
				assert(0);

#if 0
				for(i = 0; i < nent; i++) {
					ev = events[i];

					if(error == EINPROGRESS) {
						TAILQ_REMOVE(&base->aioqueue, ev, ev_aio_next);	
						TAILQ_INSERT_TAIL(&base->submittedqueue, ev, ev_submitted_next);
					}else{
						ev->_ev.ev_aio.error = error;
						ev->_ev.ev_aio.result = aio_return(&ev->_ev.ev_aio.aiocb);
						
						event_active(ev, EV_AIO, 1);
					}
				}
				if(iosubmit_error == EAGAIN)
					break;
#endif

			}else{
				for(i = 0; i < nent; i++) {
					ev = ctx->events[i];
					ev->ev_flags |= EVLIST_AIO_SUBMITTED;
					TAILQ_REMOVE(&base->aioqueue, ev, ev_aio_next);	
					TAILQ_INSERT_TAIL(&base->submittedqueue, ev, ev_submitted_next);
				}
			}
		}
	}while(nent > 0 && result > 0);
}

static void
aio_linux_process_one(struct event *ev, struct io_event *io_event)
{
	ev->ev_flags &= ~EVLIST_AIO_SUBMITTED;

	if(io_event->res < 0) {
		ev->_ev.ev_aio.result = -1;	
		ev->ev_res = ev->_ev.ev_aio.error = io_event->res;	
	}else{
		ev->_ev.ev_aio.result = io_event->res;	
		ev->ev_res = ev->_ev.ev_aio.error = 0;	
	}

	// in aio_linux_submit we removed ev from the aioqueue. 
	// we can "just" submit the aio request and we are done. 

	event_active(ev, EV_AIO, 1);
}

static void
aio_linux_process(struct event_base *base, uint64_t _hint)
{
	struct aio_linux_ctx *ctx = (struct aio_linux_ctx *)base->evaiobase;
	int result = 0;
	struct timespec timeout = {0, 0};
	struct event *ev;
	int i;

	// Retrieve as many events as possible to prevent starvation
	do{
		_hint -= (unsigned)result;
		result = io_getevents(ctx->ctx_id, _hint < ctx->max_nent ? _hint : ctx->max_nent,
				      ctx->max_nent, ctx->io_events, &timeout);

		for(i = 0; i < result; i++) {
			aio_linux_process_one((struct event*)ctx->io_events[i].data,
					      ctx->io_events + i);
		}

	}while(result == ctx->max_nent);
}

static void
aio_linux_cancel(struct event_base *base, struct event *ev)
{
	struct aio_linux_ctx *ctx = (struct aio_linux_ctx *)base->evaiobase;

	if(ev->ev_flags & EVLIST_AIO_SUBMITTED)
	{
		if(io_cancel(ctx->ctx_id, &ev->_ev.ev_aio.iocb, &ctx->io_events[0]) == 0)
			aio_linux_process_one(ev, &ctx->io_events[0]);
	}else{
		TAILQ_REMOVE(&base->aioqueue, ev, ev_aio_next);	
	}
}

