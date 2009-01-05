
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
#include <sys/event.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#ifndef HAVE_AIO_H
#include <aio.h>
#endif

#include "event2/event.h"
#include "event2/event_struct.h"
#include "event-internal.h"
#include "log.h"
#include "event-internal.h"

#if AIO_LISTIO_MAX > 64
#define EVENT_AIO_LISTIO_MAX 64
#else
#define EVENT_AIO_LISTIO_MAX AIO_LISTIO_MAX
#endif

void aio_posix_prepare_read(struct event *, int fd, void *buf, size_t length, off_t offset, int pri);
void aio_posix_prepare_write(struct event *, int fd, void *buf, size_t length, off_t offset, int pri);
static void aio_posix_kqueue_submit(struct event_base *base);
static void aio_posix_kqueue_cancel(struct event_base *base, struct event *ev);

const struct eventaioop aio_posix_kqueue_ops = {
	"POSIX AIO (via kqueue)",
	NULL,
	NULL,
	aio_posix_prepare_read,
	aio_posix_prepare_write,
	aio_posix_kqueue_submit,
	aio_posix_kqueue_cancel,
	1
};

static void
aio_posix_kqueue_closure(struct event_base *base, struct event *ev)
{
	ev->_ev.ev_aio.error = aio_error(&ev->_ev.ev_aio.aiocb);
	ev->_ev.ev_aio.result = aio_return(&ev->_ev.ev_aio.aiocb);

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

static void
aio_posix_kqueue_submit(struct event_base *base)
{
	int result;
	int listio_error, error;
	int nent;
	struct aiocb *iocbs[EVENT_AIO_LISTIO_MAX];
	struct event *events[EVENT_AIO_LISTIO_MAX];
	struct event *ev;
	int i;

	do{
		nent = event_aio_get_events_to_submit(base, events, EVENT_AIO_LISTIO_MAX);

		if(nent > 0) {
			struct aiocb **iocbp;
			struct event **eventp;

			for(i = nent, eventp = events, iocbp = iocbs; i--; )
			{
				(*eventp)->ev_closure = aio_posix_kqueue_closure;
				*iocbp++ = &(*eventp++)->_ev.ev_aio.aiocb;
			}

			result = lio_listio(LIO_NOWAIT, iocbs, nent, NULL);

			listio_error = errno;

			if(result < 0) {
				/*
				 * Some iocbs were not submitted succefuly,
				 * analyse every of them
				 */
				for(i = 0; i < nent; i++) {
					ev = events[i];

					error = aio_error(&ev->_ev.ev_aio.aiocb);

					if(error == EINPROGRESS) {
						event_aio_set_submitted(ev);
					}else{
						event_aio_set_ready(ev, -1, error);
					}
				}

				if(listio_error == EAGAIN)
					break;
			}else if(result == 0) {
				for(i = 0; i < nent; i++) {
					ev = events[i];
					event_aio_set_submitted(ev);
				}
			}
		}
	}while(nent > 0 && result == 0);
}

static void
aio_posix_kqueue_cancel(struct event_base *base, struct event *ev)
{
    if(ev->ev_flags & EVLIST_AIO_SUBMITTED)
        aio_cancel(ev->ev_fd, &ev->_ev.ev_aio.aiocb);

	event_aio_set_cancelled(ev);
}

