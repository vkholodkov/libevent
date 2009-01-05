
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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

#ifdef HAVE_AIO_H
#include <aio.h>
#endif

#include "event2/event.h"
#include "event2/event_struct.h"
#include "event-internal.h"
#include "log.h"
#include "event-internal.h"

#if !defined AIO_LISTIO_MAX || AIO_LISTIO_MAX > 64
#define EVENT_AIO_LISTIO_MAX 64
#else
#define EVENT_AIO_LISTIO_MAX AIO_LISTIO_MAX
#endif

struct aio_posix_ctx {
	struct event notify_event;
	unsigned int notify_event_added:1;
};

static void *aio_posix_init(struct event_base *base);
static void aio_posix_dealloc(struct event_base *base, void*);
void aio_posix_prepare_read(struct event *, int fd, void *buf, size_t length, off_t offset, int pri);
void aio_posix_prepare_write(struct event *, int fd, void *buf, size_t length, off_t offset, int pri);
static void aio_posix_submit(struct event_base *base);
static void aio_posix_cancel(struct event_base *base, struct event *ev);

const struct eventaioop aio_posix_ops = {
	"POSIX AIO",
	aio_posix_init,
	aio_posix_dealloc,
	aio_posix_prepare_read,
	aio_posix_prepare_write,
	aio_posix_submit,
	aio_posix_cancel,
	0
};

static void
aio_posix_callback(int fd, short event, void *_base)
{
	int error;
	struct event_base *base = (struct event_base*)_base;
	struct event *ev;

	TAILQ_FOREACH(ev, &base->submittedqueue, ev_submitted_next)
	{
		error = aio_error(&ev->_ev.ev_aio.aiocb);
		if(error == EINPROGRESS)
				continue;

		event_aio_set_ready(ev, aio_return(&ev->_ev.ev_aio.aiocb), error);
	}
}

static void
aio_posix_closure(struct event_base *base, struct event *ev)
{
	(*ev->ev_aio_callback)(
		ev
      , (void*)ev->_ev.ev_aio.aiocb.aio_buf
      , ev->_ev.ev_aio.aiocb.aio_nbytes
      , ev->_ev.ev_aio.aiocb.aio_offset
	  , ev->_ev.ev_aio.result
	  , ev->_ev.ev_aio.error
	  , ev->ev_arg
	  );
}

static void*
aio_posix_init(struct event_base *base)
{
	struct aio_posix_ctx *ctx;

	if (!(ctx = mm_calloc(1, sizeof(struct aio_posix_ctx))))
		return (NULL);

	event_assign(&ctx->notify_event, base, SIGIO, EV_SIGNAL|EV_PERSIST, aio_posix_callback, base);

	ctx->notify_event.ev_flags |= EVLIST_INTERNAL;

	ctx->notify_event_added = 0;

	return ctx;
}

static void
aio_posix_dealloc(struct event_base *base, void*_ctx)
{
	struct aio_posix_ctx *ctx = (struct aio_posix_ctx*)_ctx;

	if(ctx->notify_event_added != 0)
		event_del(&ctx->notify_event);
}

void aio_posix_prepare_read(struct event *ev, int fd, void *buf, size_t length, off_t offset, int pri)
{
	ev->_ev.ev_aio.aiocb.aio_fildes = fd;
	ev->_ev.ev_aio.aiocb.aio_offset = offset;
	ev->_ev.ev_aio.aiocb.aio_buf = buf;
	ev->_ev.ev_aio.aiocb.aio_nbytes = length;
	ev->_ev.ev_aio.aiocb.aio_reqprio = pri;
	ev->_ev.ev_aio.aiocb.aio_lio_opcode = LIO_READ;
	ev->_ev.ev_aio.aiocb.aio_sigevent.sigev_notify = SIGEV_NONE;
}

void aio_posix_prepare_write(struct event *ev, int fd, void *buf, size_t length, off_t offset, int pri)
{
	ev->_ev.ev_aio.aiocb.aio_fildes = fd;
	ev->_ev.ev_aio.aiocb.aio_offset = offset;
	ev->_ev.ev_aio.aiocb.aio_buf = buf;
	ev->_ev.ev_aio.aiocb.aio_nbytes = length;
	ev->_ev.ev_aio.aiocb.aio_reqprio = pri;
	ev->_ev.ev_aio.aiocb.aio_lio_opcode = LIO_WRITE;
	ev->_ev.ev_aio.aiocb.aio_sigevent.sigev_notify = SIGEV_NONE;
}

static void
aio_posix_submit(struct event_base *base)
{
	struct aio_posix_ctx *ctx = (struct aio_posix_ctx*)base->evaiobase;
	int result;
	int nent;
	int listio_error, error;
	struct aiocb *iocbs[EVENT_AIO_LISTIO_MAX];
	struct event *events[EVENT_AIO_LISTIO_MAX];
	struct event *ev;
	struct sigevent aio_sigevent;
	int i;

	do{
		nent = event_aio_get_events_to_submit(base, events, EVENT_AIO_LISTIO_MAX);

		if(nent > 0) {
			struct aiocb **iocbp;
			struct event **eventp;

			for(i = nent, eventp = events, iocbp = iocbs; i--; )
			{
				(*eventp)->ev_closure = aio_posix_closure;
				*iocbp++ = &(*eventp++)->_ev.ev_aio.aiocb;
			}

			if (ctx->notify_event_added == 0) {
				event_add(&ctx->notify_event, NULL);
				ctx->notify_event_added = 1;
			}

			aio_sigevent.sigev_notify = SIGEV_SIGNAL;
			aio_sigevent.sigev_signo = SIGIO;
			aio_sigevent.sigev_value.sival_ptr = 0;

			result = lio_listio(LIO_NOWAIT, iocbs, nent, &aio_sigevent);

			listio_error = errno;

			if(result < 0) {
				for(i = 0; i < nent; i++) {
					ev = events[i];

					error = aio_error(&ev->_ev.ev_aio.aiocb);

					if(error == EINPROGRESS) {
						event_aio_set_submitted(ev);
					}else{
						event_aio_set_ready(ev, aio_return(&ev->_ev.ev_aio.aiocb), error);
					}
				}

				if(listio_error == EAGAIN)
					break;
			}else{
				for(i = 0; i < nent; i++) {
					ev = events[i];
					event_aio_set_submitted(ev);
				}
			}
		}
	}while(nent > 0 && result == 0);
}

static void
aio_posix_cancel(struct event_base *base, struct event *ev)
{
    if(ev->ev_flags & EVLIST_AIO_SUBMITTED)
        aio_cancel(ev->ev_fd, &ev->_ev.ev_aio.aiocb);

	event_aio_set_cancelled(ev);
}
