#include <fcntl.h>  /* open */
#include <unistd.h> /* close */
#include <stdio.h>  /* printf */
#include <errno.h>  /* errno */
#include <string.h> /* memset */

#include <sys/ioctl.h>	     /* ioctl */
#include <linux/videodev2.h> /* v4l */


static void enumerate_menu(int fd, struct v4l2_queryctrl queryctrl)
{
	struct v4l2_querymenu querymenu;
	memset(&querymenu, 0, sizeof(querymenu));

	printf("  Menu items:\n");
	querymenu.id = queryctrl.id;

	for (querymenu.index = queryctrl.minimum;
	     querymenu.index <= queryctrl.maximum;
	     querymenu.index++)
	{
		if (0 == ioctl(fd, VIDIOC_QUERYMENU, &querymenu))
		{
			printf("  - %s\n", querymenu.name);
		}
	}
}

int enumerate_ctrls(int fd)
{
	struct v4l2_queryctrl queryctrl;
	memset(&queryctrl, 0, sizeof(queryctrl));

	queryctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
	while (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl))
	{
		if (!(queryctrl.flags & V4L2_CTRL_FLAG_DISABLED))
		{
			printf("Control[id:%d] - %s \n", queryctrl.id, queryctrl.name);

			if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
				enumerate_menu(fd, queryctrl);
		}
		queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}

	if (errno != EINVAL)
	{
		perror("VIDIOC_QUERYCTRL");
		return -1;
	}

	return 0;
}

int change_ctrl_boolval(int v4l_fd, int cid, int value)
{
	struct v4l2_queryctrl queryctrl;
	struct v4l2_control control;

	memset(&queryctrl, 0, sizeof(queryctrl));
	queryctrl.id = cid;

	if (-1 == ioctl(v4l_fd, VIDIOC_QUERYCTRL, &queryctrl))
	{
		if (errno != EINVAL)
		{
			perror("VIDIOC_QUERYCTRL");
			return -1;
		}
		else
		{
			printf("Control[id:%d] not supported on this device\n", cid);
			return -1;
		}
	}
	else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
	{
		printf("Control[id:%d] not supported on this device\n", cid);
		return -1;
	}
	else
	{
		/* Ensure control type is integer */
		if (queryctrl.type != V4L2_CTRL_TYPE_BOOLEAN)
		{
			printf("Control type is not boolean but %d \n", queryctrl.type);
			return -1;
		}

		/* Ensure expected value is between min & max */
		/* This step isn't required, but the driver may clamp the value or return ERANGE */
		if (value < queryctrl.minimum || value > queryctrl.maximum)
		{
			printf("Desired value (%d) not in range [%d..%d] \n", value, queryctrl.minimum, queryctrl.maximum);
			return -1;
		}

		memset(&control, 0, sizeof(control));
		control.id = cid;
		control.value = value;
		if (-1 == ioctl(v4l_fd, VIDIOC_S_CTRL, &control))
		{
			perror("VIDIOC_S_CTRL");
			return -1;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int v4l_fd;

	if (argc == 1)
	{
		printf("Usage: %s /dev/v4l-node \n", argv[0]);
		printf("Note that '/dev/v4l-node' can be: \n");
		printf("    - the main video device, for example '/dev/video0'\n");
		printf("    - the camera subdevice or any other v4l-subdevice, for example '/dev/v4l-subdev0'\n");
		return -1;
	}

	/* Open V4L2 node */
	v4l_fd = open(argv[1], O_RDWR);
	if (v4l_fd < 0)
	{
		printf("Can't open device node: %s\n", argv[1]);
		return -1;
	}

	/* Enumerate all controls */
	if (enumerate_ctrls(v4l_fd) < 0)
	{
		printf("Failed to enumerate controls\n");
		goto close_fd;
	}

	/* Change the value of 'V4L2_CID_HFLIP' control (Flip horizontally) */
	if (change_ctrl_boolval(v4l_fd, V4L2_CID_HFLIP, 1) < 0)
	{
		printf("Failed to change control value\n");
		goto close_fd;
	}

close_fd:
	close(v4l_fd);

	return 0;
}
