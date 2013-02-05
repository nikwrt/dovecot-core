/* Copyright (c) 2007-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream-private.h"
#include "istream-metawrap.h"

struct metawrap_istream {
	struct istream_private istream;
	metawrap_callback_t *callback;
	void *context;

	uoff_t start_offset;
	bool in_metadata;
};

static int metadata_header_read(struct metawrap_istream *mstream)
{
	char *line, *p;

	while ((line = i_stream_read_next_line(mstream->istream.parent)) != NULL) {
		if (*line == '\0')
			return 1;
		p = strchr(line, ':');
		if (p == NULL) {
			mstream->istream.istream.stream_errno = EINVAL;
			return -1;
		}
		*p++ = '\0';
		mstream->callback(line, p, mstream->context);
	}
	if (mstream->istream.parent->eof) {
		mstream->istream.istream.stream_errno =
			mstream->istream.parent->stream_errno;
		mstream->istream.istream.eof = TRUE;
		return -1;
	}
	i_assert(!mstream->istream.parent->blocking);
	return 0;
}

static ssize_t i_stream_metawrap_read(struct istream_private *stream)
{
	struct metawrap_istream *mstream = (struct metawrap_istream *)stream;
	int ret;

	i_stream_seek(stream->parent, mstream->start_offset +
		      stream->istream.v_offset);

	if (mstream->in_metadata) {
		ret = metadata_header_read(mstream);
		i_assert(stream->istream.v_offset == 0);
		mstream->start_offset = stream->parent->v_offset;
		if (ret <= 0)
			return ret;
		/* this stream is kind of silently skipping over the metadata */
		stream->abs_start_offset += mstream->start_offset;
		mstream->in_metadata = FALSE;
	}
	/* after metadata header it's all just passthrough */
	return i_stream_read_copy_from_parent(&stream->istream);
}

static int i_stream_metawrap_stat(struct istream_private *stream, bool exact)
{
	struct metawrap_istream *mstream = (struct metawrap_istream *)stream;
	const struct stat *st;
	int ret;

	if (i_stream_stat(stream->parent, exact, &st) < 0)
		return -1;
	stream->statbuf = *st;

	if (mstream->in_metadata) {
		ret = i_stream_read(&stream->istream);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			stream->statbuf.st_size = -1;
			return 0;
		}
	}
	i_assert((uoff_t)stream->statbuf.st_size >= mstream->start_offset);
	stream->statbuf.st_size -= mstream->start_offset;
	return 0;
}

struct istream *
i_stream_create_metawrap(struct istream *input,
			 metawrap_callback_t *callback, void *context)
{
	struct metawrap_istream *mstream;

	mstream = i_new(struct metawrap_istream, 1);
	mstream->istream.max_buffer_size = input->real_stream->max_buffer_size;

	mstream->istream.read = i_stream_metawrap_read;
	mstream->istream.stat = i_stream_metawrap_stat;

	mstream->istream.istream.readable_fd = input->readable_fd;
	mstream->istream.istream.blocking = input->blocking;
	mstream->istream.istream.seekable = FALSE;
	mstream->in_metadata = TRUE;
	mstream->callback = callback;
	mstream->context = context;
	return i_stream_create(&mstream->istream, input,
			       i_stream_get_fd(input));
}