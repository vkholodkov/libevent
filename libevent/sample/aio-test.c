/*
 * Compile with:
 * cc -I/usr/local/include -o aio-test aio-test.c -L/usr/local/lib -levent
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <event.h>

#include <zlib.h>

#define READ_BUFFER_SIZE 4096
#define WRITE_BUFFER_SIZE 4096

#define NEVENTS 128

unsigned char in[READ_BUFFER_SIZE];
unsigned char out[WRITE_BUFFER_SIZE];

struct event events[NEVENTS];
struct event evread, evwrite;

struct event_base *base;

struct stat st;
int readfd, writefd;

size_t read_offset, write_offset;

unsigned char  gzheader[10] = { 0x1f, 0x8b, Z_DEFLATED, 0, 0, 0, 0, 0, 0, 3 };

struct gztrailer {
    uint32_t  crc32;
    uint32_t  zlen;
} ztrailer;

z_stream             zstream;

int trailer_written = 0;
enum io_state { HEADER_WRITTEN, NEED_DEFLATE, NEED_INPUT, DEFALTE_DONE, TRAILER_WRITTEN };

enum io_state state = HEADER_WRITTEN;

static void
file_read(struct event *event, void *buf, size_t size, off_t offset, int result, int error, void *arg)
{
	int ret;
	size_t remaining = (st.st_size - (offset + result));
	size_t to_write;

	if(result < 0) {
		fprintf(stderr, "AIO read failed offset=%li, size=%i, error=%d\n", (long int)offset, size, error);
		event_base_loopexit(base, NULL);
		return;
	}

	zstream.avail_in = result;
	zstream.next_in = in;

	ztrailer.crc32 = crc32(ztrailer.crc32, zstream.next_in, zstream.avail_in);
	ztrailer.zlen += result;

	// Provide output space
	zstream.avail_out = WRITE_BUFFER_SIZE;
	zstream.next_out = out;

	ret = deflate(&zstream, remaining == 0 ? Z_FINISH : Z_NO_FLUSH);

	if(ret == Z_STREAM_ERROR) {
		fprintf(stderr, "Error while deflating stream\n");
		(void)deflateEnd(&zstream);
		event_base_loopexit(base, NULL);
		return;
	}

	to_write = WRITE_BUFFER_SIZE - zstream.avail_out;

	if(zstream.avail_out == 0)
		state = NEED_DEFLATE;
	else
		if(remaining > 0)
			state = NEED_INPUT;
		else
			state = DEFALTE_DONE;

	fprintf(stdout, "read offset=%li, bytes_read=%u, remaining=%u, to_write=%u, avail_in=%u, avail_out=%u\n", (long int)offset, result, remaining, to_write, zstream.avail_in, zstream.avail_out);

	if(to_write > 0)
		event_aio_write(&evwrite, writefd, out, to_write, write_offset, 0);
	else
		event_aio_read(&evread, readfd, in, remaining < READ_BUFFER_SIZE ? remaining : READ_BUFFER_SIZE, offset + result, 0);

	read_offset += result;
}

static void
file_write(struct event *event, void *buf, size_t size, off_t offset, int result, int error, void *arg)
{
	int ret;
	size_t remaining = st.st_size - read_offset;
	size_t to_write = 0;

	if(result < 0) {
		fprintf(stderr, "AIO write failed offset=%li, size=%i, error=%d\n", (long int)offset, size, error);
		event_base_loopexit(base, NULL);
		return;
	}

	fprintf(stdout, "written offset=%li, bytes_written=%u, avail_in=%u, avail_out=%u, remaining=%u, state=%d\n", (long int)offset, result, zstream.avail_in, zstream.avail_out, remaining, state);

	write_offset += result;

	switch(state) {
		case HEADER_WRITTEN:
			event_aio_read(&evread, readfd, in, remaining < READ_BUFFER_SIZE ? remaining : READ_BUFFER_SIZE, read_offset, 0);
			break;
		case NEED_DEFLATE:
			// Provide output space
			zstream.avail_out = WRITE_BUFFER_SIZE;
			zstream.next_out = out;

			// Continue to deflate current chunk
			ret = deflate(&zstream, remaining == 0 ? Z_FINISH : Z_NO_FLUSH);

			if(ret == Z_STREAM_ERROR) {
				fprintf(stderr, "Error while deflating stream\n");
				event_base_loopexit(base, NULL);
				return;
			}

			to_write = WRITE_BUFFER_SIZE - zstream.avail_out;
			
			if(zstream.avail_out == 0)
				state = NEED_DEFLATE;
			else
				if(remaining > 0)
					state = NEED_INPUT;
				else
					state = DEFALTE_DONE;

			event_aio_write(&evwrite, writefd, out, to_write, write_offset, 0);
			break;
		case NEED_INPUT:
			event_aio_read(&evread, readfd, in, remaining < READ_BUFFER_SIZE ? remaining : READ_BUFFER_SIZE, read_offset, 0);
			break;
		case DEFALTE_DONE:
			event_aio_write(&evwrite, writefd, &ztrailer, sizeof(struct gztrailer), write_offset, 0);
			state = TRAILER_WRITTEN;
			break;
		case TRAILER_WRITTEN:
			event_base_loopexit(base, NULL);
			break;
	}
}

int
main (int argc, char **argv)
{
	int result;

	if (argc < 3) {
		fprintf(stderr, "usage aio-test <infile> <outfile>\n");
		exit (1);
	}

	zstream.zalloc = Z_NULL;
	zstream.zfree = Z_NULL;
	zstream.opaque = Z_NULL;

	// Provide output space
	zstream.avail_out = WRITE_BUFFER_SIZE;
	zstream.next_out = out;

	ztrailer.crc32 = crc32(0L, Z_NULL, 0);
	ztrailer.zlen = 0;

	result = deflateInit2(&zstream, (int) 6, Z_DEFLATED,
		-9, 9, Z_DEFAULT_STRATEGY);

	if (result != Z_OK) {
		fprintf(stderr, "deflateInit2: %d\n", result);
		exit (1);
	}
 
	readfd = open (argv[1], O_RDONLY, 0);

	if (readfd == -1) {
		perror("open");
		exit (1);
	}

	result = fstat(readfd, &st);

	if (result == -1) {
		close(readfd);
		perror("fstat");
		exit (1);
	}

	writefd = open (argv[2], O_CREAT|O_TRUNC|O_WRONLY|O_APPEND, 0644);

	if (writefd == -1) {
		close(readfd);
		perror("open");
		exit (1);
	}

	read_offset = 0;
	write_offset = 0;

	event_init();

	base = event_base_new();

	event_aio_assign(&evread, base, file_read, &evread);
	event_aio_assign(&evwrite, base, file_write, &evwrite);

	event_aio_write(&evwrite, writefd, gzheader, sizeof(gzheader), 0, 0);

	event_base_loop(base, 0);

	event_base_free(base);

	(void)deflateEnd(&zstream);

	close(readfd);
	close(writefd);
	
	return (0);
}
