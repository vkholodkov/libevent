
#include <event-config.h>

#ifdef WIN32
#include <winsock2.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef _EVENT_HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <aio.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/util.h>

#include "regress.h"

#define TEST_BUFFER_SIZE 128
#define NUM_INVOKATIONS 4

static int called = 0;

static void
read_cb(struct event *event, void *buf, size_t size, off_t offset, int result, int error, void *arg)
{
	if(result == TEST_BUFFER_SIZE && error == 0)
		called++;
}

void
test_aio(void)
{
	struct event ev;
	struct event ev1;
	struct event_base *base;
	int success = 0;
	char buf[TEST_BUFFER_SIZE];
	char buf1[TEST_BUFFER_SIZE];
	char buf2[TEST_BUFFER_SIZE];
	char buf3[TEST_BUFFER_SIZE];
	int fd;

	called = 0;

	/* Initalize the event library */
	base = event_base_new();

	printf("Testing AIO completion events: ");

	/* Initalize one event */
	event_aio_assign(&ev, base, read_cb, &ev);
	event_aio_assign(&ev1, base, read_cb, &ev1);

	fd = open("./regress_aio.c",O_RDONLY);

	if(fd < 0) {
		printf("FAILED (could not open source file)\n");
		exit(1);
	}

	event_aio_read(&ev, fd, buf, TEST_BUFFER_SIZE, 0, 0);
	event_aio_read(&ev1, fd, buf1, TEST_BUFFER_SIZE, TEST_BUFFER_SIZE, 0);

	event_base_loop(base,EVLOOP_NONBLOCK);

	event_aio_read(&ev, fd, buf2, TEST_BUFFER_SIZE, 2*TEST_BUFFER_SIZE, 0);
	event_aio_read(&ev1, fd, buf3, TEST_BUFFER_SIZE, 3*TEST_BUFFER_SIZE, 0);

	event_base_loop(base,EVLOOP_NONBLOCK);

	event_base_free(base);

	close(fd);

	if(called == NUM_INVOKATIONS)
		success = 1;

	if (success) {
		puts("OK");
	} else {
		printf("FAILED (called=%d)\n", called);
		exit(1);
	}
}
