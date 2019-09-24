// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for VD56G3 global shutter sensor
 *
 * Copyright (C) STMicroelectronics SA 2019
 * Authors: Mickael Guene <mickael.guene@st.com>
 *          for STMicroelectronics.
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define WRITE_MULTIPLE_CHUNK_MAX			16

#define DEVICE_MODEL_ID_REG				0x0000
#define VD56G3_MODEL_ID					0x5603
#define DEVICE_FWPATCH_REVISION				0x001e
#define DEVICE_BOOT					0x0200
#define CMD_BOOT					1
#define CMD_PATCH_SETUP					2

#include "st-vd56g3_patch.c"

/* regulator supplies */
static const char * const vd56g3_supply_name[] = {
	"VCORE",
	"VDDIO",
	"VANA",
};
#define VD56G3_NUM_SUPPLIES		ARRAY_SIZE(vd56g3_supply_name)

static const s64 link_freq[] = {
	402000000ULL
};

struct vd56g3_mode_info {
	u32 width;
	u32 height;
	int bin_mode;
};

static const u32 vd56g3_supported_codes[] = {
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGBRG10_1X10
};

const int vd56g3_sensor_frame_rates[] = { 15 };

static const struct vd56g3_mode_info vd56g3_mode_data[] = {
	{1124, 1364, 0},
};

struct vd56g3_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regulator_bulk_data supplies[VD56G3_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct clk *xclk;
	u32 clk_freq;
	u16 oif_ctrl;
	int nb_of_lane;
	int data_rate_in_mbps;
	/* lock to protect all members below */
	struct mutex lock;
	struct v4l2_ctrl_handler ctrl_handler;
	bool streaming;
	struct v4l2_mbus_framefmt fmt;
	const struct vd56g3_mode_info *current_mode;
	struct v4l2_fract frame_interval;
};

/* helpers */
static inline struct vd56g3_dev *to_vd56g3_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vd56g3_dev, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct vd56g3_dev,
		ctrl_handler)->sd;
}

static u8 get_bpp_by_code(__u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SGBRG8_1X8:
		return 8;
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return 10;
	}

	WARN(1, "Unsupported code %d. default to 8 bpp", code);

	return 8;
}

static s32 get_pixel_rate(struct vd56g3_dev *sensor)
{
	return div64_u64((u64)sensor->data_rate_in_mbps * sensor->nb_of_lane,
			 get_bpp_by_code(sensor->fmt.code));
}

static int get_chunk_size(struct vd56g3_dev *sensor)
{
	int max_write_len = WRITE_MULTIPLE_CHUNK_MAX;
	struct i2c_adapter *adapter = sensor->i2c_client->adapter;

	if (adapter->quirks && adapter->quirks->max_write_len)
		max_write_len = adapter->quirks->max_write_len - 2;

	max_write_len = min(max_write_len, WRITE_MULTIPLE_CHUNK_MAX);

	return max(max_write_len, 1);
}

static int vd56g3_read_reg(struct vd56g3_dev *sensor, u16 reg, u8 *val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = 1;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_dbg(&client->dev, "%s: %x i2c_transfer, reg: %x => %d\n",
			__func__, client->addr, reg, ret);
		return ret;
	}

	return 0;
}

static int vd56g3_read_reg16(struct vd56g3_dev *sensor, u16 reg, u16 *val)
{
	u8 hi, lo;
	int ret;

	ret = vd56g3_read_reg(sensor, reg, &lo);
	if (ret)
		return ret;
	ret = vd56g3_read_reg(sensor, reg + 1, &hi);
	if (ret)
		return ret;
	*val = ((u16)hi << 8) | (u16)lo;

	return 0;
}

static int vd56g3_write_reg(struct vd56g3_dev *sensor, u16 reg, u8 val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_dbg(&client->dev, "%s: i2c_transfer, reg: %x => %d\n",
			__func__, reg, ret);
		return ret;
	}

	return 0;
}

static int vd56g3_write_multiple(struct vd56g3_dev *sensor, u16 reg,
				 const u8 *data, int len)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[WRITE_MULTIPLE_CHUNK_MAX + 2];
	int i;
	int ret;

	if (len > WRITE_MULTIPLE_CHUNK_MAX)
		return -EINVAL;
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	for (i = 0; i < len; i++)
		buf[i + 2] = data[i];

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = len + 2;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_dbg(&client->dev, "%s: i2c_transfer, reg: %x => %d\n",
			__func__, reg, ret);
		return ret;
	}

	return 0;
}

static int vd56g3_write_array(struct vd56g3_dev *sensor, u16 reg, int nb,
			      const u8 *array)
{
	const int chunk_size = get_chunk_size(sensor);
	int ret;
	int sz;

	while (nb) {
		sz = min(nb, chunk_size);
		ret = vd56g3_write_multiple(sensor, reg, array, sz);
		if (ret < 0)
			return ret;
		nb -= sz;
		reg += sz;
		array += sz;
	}

	return 0;
}

static int vd56g3_poll_reg(struct vd56g3_dev *sensor, u16 reg, u8 poll_val)
{
	struct i2c_client *client = sensor->i2c_client;
	const int loop_delay_ms = 10;
	const int timeout_ms = 500;
	int loop_nb = timeout_ms / loop_delay_ms;
	u8 val;
	int ret;

	while (--loop_nb) {
		ret = vd56g3_read_reg(sensor, reg, &val);
		if (ret)
			return ret;
		dev_dbg(&client->dev,  "%s: got %d / waiting %d => ret = %d",
			__func__, val, poll_val, ret);
		if (val == poll_val)
			break;
		msleep(loop_delay_ms);
	}
	if (!loop_nb)
		return -ETIMEDOUT;

	return ret;
}

static int vd56g3_get_regulators(struct vd56g3_dev *sensor)
{
	int i;

	for (i = 0; i < VD56G3_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = vd56g3_supply_name[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       VD56G3_NUM_SUPPLIES,
				       sensor->supplies);
}

static void vd56g3_apply_reset(struct vd56g3_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;

	/* FIXME : update when doc is clear */
	dev_dbg(&client->dev, "%s", __func__);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(5000, 10000);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	usleep_range(5000, 10000);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(40000, 100000);
}

static int vd56g3_detect(struct vd56g3_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	u16 id = 0;
	int ret;

	ret = vd56g3_read_reg16(sensor, DEVICE_MODEL_ID_REG, &id);
	if (ret)
		return ret;

	if (id != VD56G3_MODEL_ID) {
		dev_warn(&client->dev, "Unsupported sensor id %x", id);
		return -ENODEV;
	}

	return 0;
}

static int vd56g3_try_fmt_internal(struct v4l2_subdev *sd,
				   struct v4l2_mbus_framefmt *fmt,
				   const struct vd56g3_mode_info **new_mode)
{
	const struct vd56g3_mode_info *mode = vd56g3_mode_data;
	unsigned int index;
	unsigned i;

	/* select code */
	for (index = 0; index < ARRAY_SIZE(vd56g3_supported_codes); index++) {
		if (vd56g3_supported_codes[index] == fmt->code)
			break;
	}
	if (index == ARRAY_SIZE(vd56g3_supported_codes))
		index = 0;

	/* select size */
	for (i = 0; i < ARRAY_SIZE(vd56g3_mode_data); i++) {
		if (mode->width <= fmt->width && mode->height <= fmt->height)
			break;
		mode++;
	}
	if (i == ARRAY_SIZE(vd56g3_mode_data))
		mode--;

	*new_mode = mode;
	fmt->code = vd56g3_supported_codes[index];
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->field = V4L2_FIELD_NONE;

	return 0;
}

static int vd56g3_stream_enable(struct vd56g3_dev *sensor)
{
	return -EINVAL;
}

static int vd56g3_stream_disable(struct vd56g3_dev *sensor)
{
	return -EINVAL;
}

static int vd56g3_rx_from_ep(struct vd56g3_dev *sensor,
			     struct fwnode_handle *endpoint)
{
	/* FIXME : use ep to setup rx */

	sensor->oif_ctrl = 2 | /* two lanes */
			   (0 < 3) | /* no swap for clk */
			   (0 < 4) | /* logic 0 map to physical 0 */
			   (0 < 6) | /* no swap for lane 0 */
			   (2 < 8) | /* logic 1 map to physical 1 */
			   (0 < 9);  /* no swap for lane 1 */

	sensor->nb_of_lane = 2;
	/* FIXME : compute in configure */
	sensor->data_rate_in_mbps = 804;

	return 0;
}

static int vd56g3_patch(struct vd56g3_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	u16 patch;
	int ret;

	ret = vd56g3_write_array(sensor, 0x2000, sizeof(array_0x2000),
				 array_0x2000);
	if (ret)
		return ret;

	ret = vd56g3_write_reg(sensor, DEVICE_BOOT, CMD_PATCH_SETUP);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, DEVICE_BOOT, 0);
	if (ret)
		return ret;

	ret = vd56g3_read_reg16(sensor, DEVICE_FWPATCH_REVISION, &patch);
	if (ret)
		return ret;

	if (patch != (DEVICE_FWPATCH_REVISION_MAJOR << 8) +
	    DEVICE_FWPATCH_REVISION_MINOR) {
		dev_err(&client->dev, "bad patch version expected %d.%d got %d.%d",
			DEVICE_FWPATCH_REVISION_MAJOR,
			DEVICE_FWPATCH_REVISION_MINOR,
			patch >> 8, patch & 0xff);
		return -ENODEV;
	}
	dev_info(&client->dev, "patch %d.%d applied", patch >> 8, patch & 0xff);

	return 0;
}

/* implement v4l2_subdev_video_ops */
static int vd56g3_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	struct i2c_client *client = sensor->i2c_client;
	int ret = 0;

	mutex_lock(&sensor->lock);
	dev_dbg(&client->dev, "%s : requested %d / current = %d", __func__,
		   enable, sensor->streaming);
	if (sensor->streaming == enable)
		goto out;

	ret = enable ? vd56g3_stream_enable(sensor) :
		       vd56g3_stream_disable(sensor);
	if (!ret)
		sensor->streaming = enable;

out:
	dev_dbg(&client->dev, "%s current now = %d / %d", __func__,
		   sensor->streaming, ret);
	mutex_unlock(&sensor->lock);

	return ret;
}

static int vd56g3_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);

	mutex_lock(&sensor->lock);
	fi->interval = sensor->frame_interval;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int vd56g3_s_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	struct i2c_client *client = sensor->i2c_client;
	u64 req_int, err, min_err = ~0ULL;
	u64 test_int;
	unsigned int i;
	int ret;

	if (fi->pad != 0)
		return -EINVAL;

	if (fi->interval.denominator == 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	dev_dbg(&client->dev, "%s request %d/%d\n", __func__,
		fi->interval.numerator, fi->interval.denominator);
	/* find nearest period */
	req_int = div64_u64((u64)(fi->interval.numerator * 10000),
			    fi->interval.denominator);
	for (i = 0; i < ARRAY_SIZE(vd56g3_sensor_frame_rates); i++) {
		test_int = div64_u64((u64)10000, vd56g3_sensor_frame_rates[i]);
		err = abs(test_int - req_int);
		if (err < min_err) {
			fi->interval.numerator = 1;
			fi->interval.denominator = vd56g3_sensor_frame_rates[i];
			min_err = err;
		}
	}
	sensor->frame_interval = fi->interval;
	dev_dbg(&client->dev, "%s set     %d/%d\n", __func__,
		fi->interval.numerator, fi->interval.denominator);

	ret = 0;
out:
	mutex_unlock(&sensor->lock);

	return ret;
}

/* implement v4l2_subdev_pad_ops */
static int vd56g3_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	struct i2c_client *client = sensor->i2c_client;

	dev_dbg(&client->dev, "%s probe index %d", __func__, code->index);
	if (code->index >= ARRAY_SIZE(vd56g3_supported_codes))
		return -EINVAL;

	code->code = vd56g3_supported_codes[code->index];

	return 0;
}

static int vd56g3_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	struct i2c_client *client = sensor->i2c_client;
	struct v4l2_mbus_framefmt *fmt;

	dev_dbg(&client->dev, "%s probe %d", __func__, format->pad);
	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_get_try_format(&sensor->sd, cfg,
						 format->pad);
	else
		fmt = &sensor->fmt;

	format->format = *fmt;

	mutex_unlock(&sensor->lock);

	return 0;
}

static int vd56g3_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	struct i2c_client *client = sensor->i2c_client;
	const struct vd56g3_mode_info *new_mode;
	struct v4l2_mbus_framefmt *fmt;
	int ret;

	if (format->pad != 0)
		return -EINVAL;

	dev_dbg(&client->dev, "%s %dx%d", __func__, format->format.width,
		format->format.height);

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	/* find best format */
	ret = vd56g3_try_fmt_internal(sd, &format->format, &new_mode);
	if (ret)
		goto out;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt = v4l2_subdev_get_try_format(sd, cfg, 0);
	} else {
		fmt = &sensor->fmt;
		sensor->current_mode = new_mode;
	}
	*fmt = format->format;

out:
	mutex_unlock(&sensor->lock);

	return ret;
}

static int vd56g3_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	struct i2c_client *client = sensor->i2c_client;

	dev_dbg(&client->dev, "%s for index %d", __func__, fse->index);
	if (fse->pad != 0)
		return -EINVAL;
	if (fse->index >= ARRAY_SIZE(vd56g3_mode_data))
		return -EINVAL;

	fse->min_width = vd56g3_mode_data[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = vd56g3_mode_data[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int vd56g3_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_interval_enum
				      *fie)
{
	const struct vd56g3_mode_info *mode = vd56g3_mode_data;
	unsigned int i;

	if (fie->pad != 0)
		return -EINVAL;
	if (fie->index >= ARRAY_SIZE(vd56g3_sensor_frame_rates))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(vd56g3_mode_data); i++) {
		if (mode->width <= fie->width && mode->height <= fie->height)
			break;
		mode++;
	}
	if (i == ARRAY_SIZE(vd56g3_mode_data))
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = vd56g3_sensor_frame_rates[fie->index];

	return 0;
}

static const struct v4l2_subdev_core_ops vd56g3_core_ops = {
};

static const struct v4l2_subdev_video_ops vd56g3_video_ops = {
	.s_stream = vd56g3_s_stream,
	.g_frame_interval = vd56g3_g_frame_interval,
	.s_frame_interval = vd56g3_s_frame_interval,
};

static const struct v4l2_subdev_pad_ops vd56g3_pad_ops = {
	.enum_mbus_code = vd56g3_enum_mbus_code,
	.get_fmt = vd56g3_get_fmt,
	.set_fmt = vd56g3_set_fmt,
	.enum_frame_size = vd56g3_enum_frame_size,
	.enum_frame_interval = vd56g3_enum_frame_interval,
};

static const struct v4l2_subdev_ops vd56g3_subdev_ops = {
	.core = &vd56g3_core_ops,
	.video = &vd56g3_video_ops,
	.pad = &vd56g3_pad_ops,
};

static const struct media_entity_operations vd56g3_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* controls */
static int vd56g3_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_PIXEL_RATE:
		ret = __v4l2_ctrl_s_ctrl_int64(ctrl, get_pixel_rate(sensor));
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int vd56g3_s_ctrl(struct v4l2_ctrl *ctrl)
{
	int ret;

	switch (ctrl->id) {
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops vd56g3_ctrl_ops = {
	.g_volatile_ctrl = vd56g3_g_volatile_ctrl,
	.s_ctrl = vd56g3_s_ctrl,
};

static int vd56g3_init_controls(struct vd56g3_dev *sensor)
{
	const struct v4l2_ctrl_ops *ops = &vd56g3_ctrl_ops;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 16);
	/* we can use our own mutex for the ctrl lock */
	hdl->lock = &sensor->lock;
	/* add V4L2_CID_PIXEL_RATE */
	ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE, 1, INT_MAX, 1,
				 get_pixel_rate(sensor));
	ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(link_freq) - 1, 0, link_freq);
	ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (hdl->error) {
		ret = hdl->error;
		goto free_ctrls;
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static int vd56g3_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct vd56g3_dev *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;
	sensor->streaming = false;
	sensor->fmt.code = MEDIA_BUS_FMT_SGBRG8_1X8;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_SRGB;
	sensor->frame_interval.numerator = 1;
	sensor->frame_interval.denominator = 15;

	endpoint = fwnode_graph_get_next_endpoint(
		of_fwnode_handle(dev->of_node), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}
	ret = vd56g3_rx_from_ep(sensor, endpoint);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Failed to parse endpoint %d\n", ret);
		return ret;
	}

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(sensor->xclk);
	}
	sensor->clk_freq = clk_get_rate(sensor->xclk);
	if (sensor->clk_freq < 6000000 || sensor->clk_freq > 27000000) {
		dev_err(dev, "Only 6Mhz-27Mhz clock range supported. provide %d Hz\n",
			sensor->clk_freq);
		return -EINVAL;
	}

	v4l2_i2c_subdev_init(&sensor->sd, client, &vd56g3_subdev_ops);
	sensor->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.ops = &vd56g3_subdev_entity_ops;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret) {
		dev_err(&client->dev, "pads init failed %d", ret);
		return ret;
	}

	/* request optional reset pin */
	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);

	ret = vd56g3_get_regulators(sensor);
	if (ret) {
		dev_err(&client->dev, "failed to get regulators %d", ret);
		goto entity_cleanup;
	}

	ret = regulator_bulk_enable(VD56G3_NUM_SUPPLIES, sensor->supplies);
	if (ret) {
		dev_err(&client->dev, "failed to enable regulators %d", ret);
		goto entity_cleanup;
	}

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(&client->dev, "failed to enable clock %d", ret);
		goto entity_cleanup;
	}

	mutex_init(&sensor->lock);

	/* apply reset sequence */
	if (sensor->reset_gpio)
		vd56g3_apply_reset(sensor);

	ret = vd56g3_detect(sensor);
	if (ret) {
		dev_err(&client->dev, "sensor detect failed %d", ret);
		goto disable_clock;
	}

	ret = vd56g3_patch(sensor);
	if (ret) {
		dev_err(&client->dev, "sensor patch failed %d", ret);
		goto disable_clock;
	}

	/* FIXME : configure */

	ret = vd56g3_init_controls(sensor);
	if (ret) {
		dev_err(&client->dev, "controls initialization failed %d", ret);
		goto disable_clock;
	}

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		dev_err(&client->dev, "async subdev register failed %d", ret);
		goto disable_clock;
	}

	dev_info(&client->dev, "vd56g3 probe successfully");

	return 0;

disable_clock:
	clk_disable_unprepare(sensor->xclk);
entity_cleanup:
	mutex_destroy(&sensor->lock);
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static int vd56g3_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vd56g3_dev *sensor = to_vd56g3_dev(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	clk_disable_unprepare(sensor->xclk);
	mutex_destroy(&sensor->lock);
	media_entity_cleanup(&sensor->sd.entity);
	regulator_bulk_disable(VD56G3_NUM_SUPPLIES, sensor->supplies);

	return 0;
}

static const struct of_device_id vd56g3_dt_ids[] = {
	{ .compatible = "st,st-vd56g3" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, vd56g3_dt_ids);

static struct i2c_driver vd56g3_i2c_driver = {
	.driver = {
		.name  = "st-vd56g3",
		.of_match_table = vd56g3_dt_ids,
	},
	.probe_new = vd56g3_probe,
	.remove = vd56g3_remove,
};

module_i2c_driver(vd56g3_i2c_driver);

MODULE_AUTHOR("Mickael Guene <mickael.guene@st.com>");
MODULE_DESCRIPTION("vd56g3 Camera Subdev Driver");
MODULE_LICENSE("GPL v2");