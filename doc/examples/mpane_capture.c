#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include <fcntl.h> /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define CAPTURE_BUFFERS 4

struct buffer
{
	unsigned int idx;
	unsigned int size;
	void *mem;
};

static char *dev_name;
static int fd = -1;

struct buffer buffers[CAPTURE_BUFFERS];
static unsigned int buffers_nb;
static int frame_count = 0;
const char *pattern = "frame-#.bin";
unsigned int sequence = 0;

static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
	int r;

	do
	{
		r = ioctl(fh, request, arg);
	} while (-1 == r && EINTR == errno);

	return r;
}

static void process_image(struct v4l2_buffer *buf)
{
	unsigned int pattern_size;
	static int file = -1;
	char *filename;
	const char *p;
	bool append;
	int ret;

	pattern_size = strlen(pattern);
	filename = malloc(pattern_size + 12);
	if (filename == NULL)
		return;

	p = strchr(pattern, '#');
	if (p != NULL)
	{
		sprintf(filename, "%.*s%06u%s", (int)(p - pattern), pattern,
			sequence, p + 1);
		append = false;
	}
	else
	{
		strcpy(filename, pattern);
		append = true;
	}

	file = open(filename, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC),
		    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	free(filename);
	if (file == -1)
		return;

	void *data = buffers[buf->index].mem;
	unsigned int length = buf->m.planes[0].bytesused;

	ret = write(file, data, length);
	if (ret < 0)
	{
		errno_exit("Write error");
	}
	else if (ret != (int)length)
	{
		fprintf(stderr, "write error: only %d bytes written instead of %u\n", ret, length);
		exit(EXIT_FAILURE);
	}

	sequence++;

	close(file);
}

static int read_frame(void)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[1];
	int ret;

	/* Dequeue a buffer */
	CLEAR(buf);
	CLEAR(planes);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.length = 1;
	buf.m.planes = planes;

	ret = xioctl(fd, VIDIOC_DQBUF, &buf);
	if (ret < 0)
	{
		if (errno != EIO)
		{
			fprintf(stderr, "Unable to dequeue buffer: %s (%d).\n",
				strerror(errno), errno);
			errno_exit("VIDIOC_DQBUF");
		}
	}

	assert(buf.index < buffers_nb);
	process_image(&buf);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
		errno_exit("VIDIOC_QBUF");

	return 1;
}

static void mainloop(void)
{
	unsigned int count;

	count = frame_count;

	while (count-- > 0)
	{
		fprintf(stderr, "Read frame %d \r", count);
		read_frame();
	}
}

static void stop_capturing(void)
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
		errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
	unsigned int i;
	enum v4l2_buf_type type;

	/* Prepare capture : queue all buffers */
	for (i = 0; i < buffers_nb; ++i)
	{
		struct v4l2_buffer buf;
		struct v4l2_plane planes[1];

		CLEAR(buf);
		CLEAR(planes);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = 1;

		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");
	}

	/* Initialize stream */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
		errno_exit("VIDIOC_STREAMON");

	fprintf(stderr, "Start streaming \n");
}

static void uninit_device(void)
{
	unsigned int i;

	for (i = 0; i < buffers_nb; ++i)
	{
		if (-1 == munmap(buffers[i].mem, buffers[i].size))
			errno_exit("munmap");
	}
}

static void init_device(void)
{
	struct v4l2_capability cap;
	unsigned int caps;
	unsigned int i;
	bool has_video;
	bool has_capture;
	bool has_output;
	bool has_mplane;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[1];

	/* Device Capabilities */
	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
	{
		if (EINVAL == errno)
		{
			fprintf(stderr, "%s is no V4L2 device\n", dev_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
		   ? cap.device_caps
		   : cap.capabilities;

	has_video = caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE |
			    V4L2_CAP_VIDEO_CAPTURE |
			    V4L2_CAP_VIDEO_OUTPUT_MPLANE |
			    V4L2_CAP_VIDEO_OUTPUT);
	has_capture = caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE |
			      V4L2_CAP_VIDEO_CAPTURE);
	has_output = caps & (V4L2_CAP_VIDEO_OUTPUT_MPLANE |
			     V4L2_CAP_VIDEO_OUTPUT);
	has_mplane = caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE |
			     V4L2_CAP_VIDEO_OUTPUT_MPLANE);

	fprintf(stderr, "Device `%s' on `%s' (driver '%s') supports%s%s%s %s mplanes.\n",
		cap.card, cap.bus_info, cap.driver,
		has_video ? " video," : "",
		has_capture ? " capture," : "",
		has_output ? " output," : "",
		has_mplane ? "with" : "without");

	if (!has_capture)
	{
		fprintf(stderr, "%s is no video capture device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	/* Capture is capabilities dependant. Here Multi-Planar API is used. */
	if (!has_mplane)
	{
		fprintf(stderr, "This example targets Multi-Planar API, %s supports only Single-Planar API. Aborting.\n", dev_name);
		exit(EXIT_FAILURE);
	}	

	/* Setup resolution and format */
	/* They must comply with those configured on mediacontroller pipeline */
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
		errno_exit("VIDIOC_G_FMT");
	fprintf(stderr, "Current resolution (%dx%d)\n", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
	assert(fmt.fmt.pix_mp.num_planes == 1);
	
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SGBRG8;
	fmt.fmt.pix_mp.width = 480;
	fmt.fmt.pix_mp.height = 640;
	fprintf(stderr, "Setup resolution (%dx%d)\n", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_exit("VIDIOC_S_FMT");
	
	/* Request Buffers */
	CLEAR(req);
	req.count = CAPTURE_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
	{
		if (EINVAL == errno)
		{
			fprintf(stderr, "%s does not support memory mapping \n", dev_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			errno_exit("VIDIOC_REQBUFS");
		}
	}
	if (req.count != CAPTURE_BUFFERS)
	{
		fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
		exit(EXIT_FAILURE);
	}
	buffers_nb = req.count;
	fprintf(stderr, "%d buffer requested with success \n", buffers_nb);

	/* Map the Buffers (Use Multi-Planar API but with 1 plane) */
	for (i = 0; i < buffers_nb; ++i)
	{
		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = 1; /* length in struct v4l2_buffer in multi-planar API stores the size of planes array. */
		buf.m.planes = planes;

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");

		buffers[i].idx = i;
		buffers[i].size = buf.m.planes[0].length;
		buffers[i].mem = mmap(NULL /* start anywhere */,
				      buffers[i].size,
				      PROT_READ | PROT_WRITE /* required */,
				      MAP_SHARED /* recommended */,
				      fd,
				      buf.m.planes[0].m.mem_offset);

		if (buffers[i].mem == MAP_FAILED)
		{
			errno_exit("mmap");
		}

		fprintf(stderr, "Buffer %u (size=%d) mapped at address %p.\n",
			buffers[i].idx,
			buffers[i].size,
			buffers[i].mem);
	}
}

static void close_device(void)
{
	if (-1 == close(fd))
		errno_exit("close");

	fd = -1;
}

static void open_device(void)
{
	struct stat st;

	if (-1 == stat(dev_name, &st))
	{
		fprintf(stderr, "Cannot identify '%s': %d, %s\n", dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (!S_ISCHR(st.st_mode))
	{
		fprintf(stderr, "%s is no device \n", dev_name);
		exit(EXIT_FAILURE);
	}

	fd = open(dev_name, O_RDWR /* required */, 0);

	if (-1 == fd)
	{
		fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char **argv)
{
	// avoid warning
	(void) argc;
	(void) argv;

	dev_name = "/dev/video0";
	frame_count = 10;

	open_device();
	init_device();
	start_capturing();
	mainloop();
	stop_capturing();
	uninit_device();
	close_device();
	return 0;
}
