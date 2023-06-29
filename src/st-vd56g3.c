// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for VD56G3 (Mono) and VD66GY (RGB) global shutter sensors
 *
 * Copyright (C) STMicroelectronics SA 2019
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <asm/unaligned.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Backward compatibility */
#include <linux/version.h>
#if KERNEL_VERSION(5, 18, 0) > LINUX_VERSION_CODE
#define MIPI_CSI2_DT_RAW8	0x2a
#define MIPI_CSI2_DT_RAW10	0x2b
#else
#include <media/mipi-csi2.h>
#endif

#if KERNEL_VERSION(5, 15, 0) > LINUX_VERSION_CODE
#define HZ_PER_MHZ		1000000UL
#define MEGA			1000000UL
#else
#include <linux/units.h>
#endif

#if KERNEL_VERSION(5, 9, 0) > LINUX_VERSION_CODE
int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	if (err != -EPROBE_DEFER)
		dev_err(dev, "error %d: %pV", err, &vaf);
	else
		dev_dbg(dev, "error %d: %pV", err, &vaf);

	va_end(args);

	return err;
}
#endif

#if KERNEL_VERSION(4, 5, 0) > LINUX_VERSION_CODE
int pm_runtime_get_if_in_use(struct device *dev)
{
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&dev->power.lock, flags);
	retval = dev->power.disable_depth > 0 ? -EINVAL :
		dev->power.runtime_status == RPM_ACTIVE
			&& atomic_inc_not_zero(&dev->power.usage_count);
	spin_unlock_irqrestore(&dev->power.lock, flags);
	return retval;
}
#endif

#define VD56G3_REG_SIZE_SHIFT		16
#define VD56G3_REG_ADDR_MASK		0xffff
#define VD56G3_REG_8BIT(n)		((1 << VD56G3_REG_SIZE_SHIFT) | (n))
#define VD56G3_REG_16BIT(n)		((2 << VD56G3_REG_SIZE_SHIFT) | (n))
#define VD56G3_REG_32BIT(n)		((4 << VD56G3_REG_SIZE_SHIFT) | (n))

/* Register Map */
#define VD56G3_REG_MODEL_ID				VD56G3_REG_16BIT(0x0000)
#define VD56G3_MODEL_ID					0x5603
#define VD56G3_REG_REVISION				VD56G3_REG_16BIT(0x0002)
#define VD56G3_REG_OPTICAL_REVISION			VD56G3_REG_8BIT(0x001a)
#define VD56G3_REG_FWPATCH_REVISION			VD56G3_REG_16BIT(0x001e)
// TODO : replace 3 next registers by VTPATCH_ID to be aligned with UM
#define VD56G3_REG_VTIMING_RD_REVISION			VD56G3_REG_8BIT(0x0020)
#define VD56G3_REG_VTIMING_GR_REVISION			VD56G3_REG_8BIT(0x0024)
#define VD56G3_REG_VTIMING_GT_REVISION			VD56G3_REG_8BIT(0x0026)
#define VD56G3_REG_SYSTEM_FSM				VD56G3_REG_8BIT(0x0028)
#define VD56G3_SYSTEM_FSM_READY_TO_BOOT			0x01
#define VD56G3_SYSTEM_FSM_SW_STBY			0x02
#define VD56G3_SYSTEM_FSM_STREAMING			0x03
#define VD56G3_REG_TEMPERATURE				VD56G3_REG_16BIT(0x004c)
#define VD56G3_REG_APPLIED_COARSE_EXPOSURE		VD56G3_REG_16BIT(0x0064)
#define VD56G3_REG_APPLIED_ANALOG_GAIN			VD56G3_REG_8BIT(0x0068)
#define VD56G3_REG_APPLIED_DIGITAL_GAIN			VD56G3_REG_16BIT(0x006A)
#define VD56G3_REG_BOOT					VD56G3_REG_8BIT(0x0200)
#define VD56G3_CMD_ACK					0
#define VD56G3_CMD_BOOT					1
#define VD56G3_CMD_PATCH_SETUP				2
#define VD56G3_REG_STBY					VD56G3_REG_8BIT(0x0201)
#define VD56G3_CMD_START_STREAM				1
#define VD56G3_CMD_THSENS_READ				4
#define VD56G3_REG_STREAMING				VD56G3_REG_8BIT(0x0202)
#define VD56G3_CMD_STOP_STREAM				1
#define VD56G3_REG_VTPATCHING				VD56G3_REG_8BIT(0x0203)
#define VD56G3_CMD_START_VTRAM_UPDATE			1
#define VD56G3_CMD_END_VTRAM_UPDATE			2
#define VD56G3_REG_EXT_CLOCK				VD56G3_REG_32BIT(0x0220)
#define VD56G3_REG_CLK_PLL_PREDIV			VD56G3_REG_8BIT(0x0224)
#define VD56G3_REG_CLK_SYS_PLL_MULT			VD56G3_REG_8BIT(0x0226)
#define VD56G3_REG_ORIENTATION				VD56G3_REG_8BIT(0x0302)
#define VD56G3_REG_VT_CTRL				VD56G3_REG_8BIT(0x0309)
#define VD56G3_REG_FORMAT_CTRL				VD56G3_REG_8BIT(0x030a)
#define VD56G3_REG_OIF_CTRL				VD56G3_REG_16BIT(0x030c)
#define VD56G3_REG_OIF_IMG_CTRL				VD56G3_REG_8BIT(0x030f)
#define VD56G3_REG_OIF_CSI_BITRATE			VD56G3_REG_16BIT(0x0312)
#define VD56G3_REG_DUSTER_CTRL				VD56G3_REG_8BIT(0x0318)
#define VD56G3_DUSTER_DISABLE				0
#define VD56G3_DUSTER_ENABLE_DEF_MODULES		0x13
#define VD56G3_REG_DARKCAL_CTRL				VD56G3_REG_8BIT(0x0340)
#define VD56G3_DARKCAL_ENABLE				1
#define VD56G3_DARKCAL_DISABLE_DARKAVG			2
#define VD56G3_REG_PATGEN_CTRL				VD56G3_REG_16BIT(0x0400)
#define VD56G3_REG_DARKCAL_PEDESTAL			VD56G3_REG_8BIT(0x0415)
#define VD56G3_REG_AE_COLDSTART_COARSE_EXPOSURE		VD56G3_REG_16BIT(0x042A)
#define VD56G3_REG_AE_COLDSTART_ANALOG_GAIN		VD56G3_REG_8BIT(0x042C)
#define VD56G3_REG_AE_COLDSTART_DIGITAL_GAIN		VD56G3_REG_16BIT(0x042E)
#define VD56G3_REG_AE_COMPILER_CONTROL			VD56G3_REG_8BIT(0x0430)
#define VD56G3_REG_AE_COMPENSATION			VD56G3_REG_16BIT(0x043A)
#define VD56G3_REG_AE_TARGET_PERCENTAGE			VD56G3_REG_16BIT(0x043C)
#define VD56G3_REG_AE_STEP_PROPORTION			VD56G3_REG_16BIT(0x043E)
#define VD56G3_REG_AE_LEAK_PROPORTION			VD56G3_REG_16BIT(0x0440)
#define VD56G3_REG_EXP_MODE				VD56G3_REG_8BIT(0x044c)
#define VD56G3_EXP_MODE_AUTO				0
#define VD56G3_EXP_MODE_FREEZE				1
#define VD56G3_EXP_MODE_MANUAL				2
#define VD56G3_REG_MANUAL_ANALOG_GAIN			VD56G3_REG_8BIT(0x044d)
#define VD56G3_REG_MANUAL_COARSE_EXPOSURE		VD56G3_REG_16BIT(0x044e)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH0		VD56G3_REG_16BIT(0x0450)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH1		VD56G3_REG_16BIT(0x0452)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH2		VD56G3_REG_16BIT(0x0454)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH3		VD56G3_REG_16BIT(0x0456)
#define VD56G3_REG_FRAME_LENGTH				VD56G3_REG_16BIT(0x0458)
#define VD56G3_REG_OUT_ROI_X_START			VD56G3_REG_16BIT(0x045e)
#define VD56G3_REG_OUT_ROI_X_END			VD56G3_REG_16BIT(0x0460)
#define VD56G3_REG_OUT_ROI_Y_START			VD56G3_REG_16BIT(0x0462)
#define VD56G3_REG_OUT_ROI_Y_END			VD56G3_REG_16BIT(0x0464)
#define VD56G3_REG_GPIO_0_CTRL				VD56G3_REG_8BIT(0x0467)
#define VD56G3_GPIOX_FSYNC_OUT				0x00
#define VD56G3_GPIOX_GPIO_IN				0x01
#define VD56G3_GPIOX_STROBE_MODE			0x02
#define VD56G3_GPIOX_VT_SLAVE_MODE			0x0a
#define VD56G3_REG_READOUT_CTRL				VD56G3_REG_8BIT(0x047e)

/*
 * The VD56G3 pixel array is organized as follows:
 *
 * +--------------------------------+
 * |                                | \
 * |   +------------------------+   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   |   Default resolution   |   |  | Native height (1364)
 * |   |      1120 x 1360       |   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   |                        |   |  |
 * |   +------------------------+   |  |
 * |                                | /
 * +--------------------------------+
 *   <----------------------------->
 *                     \-------------------  Native width (1124)
 *
 * The native resolution is 1124x1364.
 * The recommanded/default resolution is 1120x1360 (multiple of 16).
 */
#define VD56G3_NATIVE_WIDTH				1124
#define VD56G3_NATIVE_HEIGHT				1364
#define VD56G3_DEFAULT_WIDTH				1120
#define VD56G3_DEFAULT_HEIGHT				1360

/* PLL settings */
#define VD56G3_TARGET_PLL				804000000UL
#define VD56G3_VT_CLOCK_DIV				5

/* Line length and Frame length (valid for 10bits ADC only) */
#define VD56G3_LINE_LENGTH_MIN				1236				// 1236 for 10bits ADC (TODO: ensure 9bits is never used)
#define VD56G3_FRAME_LENGTH_MIN				(VD56G3_NATIVE_HEIGHT+69)	// Min Frame Length, Min Vblank, highest FPS
#define VD56G3_FRAME_LENGTH_DEF_60FPS			2168				// (1/60)/(line_length/pixel_clk) // TODO : check line_length and pixel_clk at runtime

/* Exposure settings */
#define VD56G3_EXPOSURE_OFFSET				(68 + 7)			// EXP_COARSE_INTG_MARGIN + 7
#define VD56G3_EXPOSURE_DEFAULT				1420

/* Output Interface settings */
#define VD56G3_MAX_CSI_DATA_LANES			2
#define VD56G3_LINK_FREQ_DEF_1LANE			750000000UL
#define VD56G3_LINK_FREQ_DEF_2LANES			402000000UL

/* GPIOs */
#define VD56G3_NB_GPIOS					8

#define VD56G3_WRITE_MULTIPLE_CHUNK_MAX			16				// TODO : unecessary

/* parse-SNIP: Custom-CIDs*/
#define V4L2_CID_TEMPERATURE			(V4L2_CID_USER_BASE | 0x1020)
#define V4L2_CID_AE_TARGET_PERCENTAGE		(V4L2_CID_USER_BASE | 0x1021)
#define V4L2_CID_AE_STEP_PROPORTION		(V4L2_CID_USER_BASE | 0x1022)
#define V4L2_CID_AE_LEAK_PROPORTION		(V4L2_CID_USER_BASE | 0x1023)
#define V4L2_CID_DARKCAL_PEDESTAL		(V4L2_CID_USER_BASE | 0x1024)
#define V4L2_CID_SLAVE_MODE			(V4L2_CID_USER_BASE | 0x1025)
#if KERNEL_VERSION(4, 13, 0) > LINUX_VERSION_CODE
#define V4L2_CID_DIGITAL_GAIN			(V4L2_CID_USER_BASE | 0x1026)
#endif
/* parse-SNAP: */

#include "st-vd56g3_patch_cut2.c"
#include "st-vd56g3_vtpatch.c"

/* regulator supplies */
static const char *const vd56g3_supply_names[] = {
	"VCORE",
	"VDDIO",
	"VANA",
};

#define VD56G3_NUM_SUPPLIES		ARRAY_SIZE(vd56g3_supply_names)


/* -----------------------------------------------------------------------------
 * Modes and formats
 */

enum vd56g3_bin_mode {
	VG56G3_BIN_MODE_NORMAL,
	VG56G3_BIN_MODE_DIGITAL_X2,
	VG56G3_BIN_MODE_DIGITAL_X4,
};

struct vd56g3_mode {
	u32 width;
	u32 height;
	enum vd56g3_bin_mode bin_mode;
	struct v4l2_rect crop;
};

/**
 * DOC: Supported Modes
 *
 * The vd56g3 driver supports 8 modes described below :
 *
 * ======= ======== ============ ====================
 *  Width   Height   Binning	  Comment
 * ======= ======== ============ ====================
 *   1124     1364   No Binning   Native resolution
 *   1120     1360   No Binning   Default resolution
 *   1024     1280   No Binning
 *   1024      768   No Binning
 *    768     1024   No Binning
 *    720     1280   No Binning
 *    640      480   No Binning
 *    480      640   Binning x2
 *    320      240   Binning x2
 * ======= ======== ============ ====================
 *
 * Each mode defaults to 60FPS. In addition, the framerate could be adjusted in
 * a continuous manner up to 88FPS (making use of the ``V4L2_CID_VBLANK``
 * control).
 */

static const struct vd56g3_mode vd56g3_supported_modes[] = {
	{
		.width = VD56G3_NATIVE_WIDTH,
		.height = VD56G3_NATIVE_HEIGHT,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 0,
			.top = 0,
			.width = VD56G3_NATIVE_WIDTH,
			.height = VD56G3_NATIVE_HEIGHT,
		},
	},
	{
		.width = VD56G3_DEFAULT_WIDTH,
		.height = VD56G3_DEFAULT_HEIGHT,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 2,
			.top = 2,
			.width = VD56G3_DEFAULT_WIDTH,
			.height = VD56G3_DEFAULT_HEIGHT,
		},
	},
	{
		.width = 1024,
		.height = 1280,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 50,
			.top = 42,
			.width = 1024,
			.height = 1280,
		},
	},
	{
		.width = 1024,
		.height = 768,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 50,
			.top = 298,
			.width = 1024,
			.height = 768,
		},
	},
	{
		.width = 768,
		.height = 1024,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 178,
			.top = 170,
			.width = 768,
			.height = 1024,
		},
	},
	{
		.width = 720,
		.height = 1280,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 202,
			.top = 42,
			.width = 720,
			.height = 1280,
		},
	},
	{
		.width = 640,
		.height = 480,
		.bin_mode = VG56G3_BIN_MODE_NORMAL,
		.crop = {
			.left = 242,
			.top = 442,
			.width = 640,
			.height = 480,
		},
	},
	{
		.width = 480,
		.height = 640,
		.bin_mode = VG56G3_BIN_MODE_DIGITAL_X2,
		.crop = {
			.left = 82,
			.top = 42,
			.width = 960,
			.height = 1280,
		},
	},
	{
		.width = 320,
		.height = 240,
		.bin_mode = VG56G3_BIN_MODE_DIGITAL_X2,
		.crop = {
			.left = 242,
			.top = 442,
			.width = 640,
			.height = 480,
		},
	},
};

/* Sensor support 8bits and 10bits output in both variants
 *  - Monochrome
 *  - RGB (with all H/V flip variations)
 */
static const unsigned int vd56g3_mbus_codes[2][5] = {
	{
		MEDIA_BUS_FMT_Y8_1X8,
		MEDIA_BUS_FMT_SGRBG8_1X8,
		MEDIA_BUS_FMT_SRGGB8_1X8,
		MEDIA_BUS_FMT_SBGGR8_1X8,
		MEDIA_BUS_FMT_SGBRG8_1X8,
	},
	{
		MEDIA_BUS_FMT_Y10_1X10,
		MEDIA_BUS_FMT_SGRBG10_1X10,
		MEDIA_BUS_FMT_SRGGB10_1X10,
		MEDIA_BUS_FMT_SBGGR10_1X10,
		MEDIA_BUS_FMT_SGBRG10_1X10,
	},
};

enum vd56g3_expo_state {
	VD56G3_EXPO_AUTO,
	VD56G3_EXPO_AUTO_FREEZE,
	VD56G3_EXPO_MANUAL
};

struct vd56g3 {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regulator_bulk_data supplies[VD56G3_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct clk *xclk;
	u32 ext_clock;
	u32 pll_prediv;
	u32 pll_mult;
	u32 pixel_clock;
	u16 oif_ctrl;
	int nb_of_lane;
	u32 gpios[VD56G3_NB_GPIOS];
	bool ext_vt_sync;
	unsigned long ext_leds_mask;
	bool is_rgb;
	/* lock to protect all members below */
	struct mutex lock;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct {
		struct v4l2_ctrl *hflip_ctrl;
		struct v4l2_ctrl *vflip_ctrl;
	};
	struct v4l2_ctrl *patgen_ctrl;
	struct {
		struct v4l2_ctrl *ae_ctrl;
		struct v4l2_ctrl *expo_ctrl;
		struct v4l2_ctrl *again_ctrl;
		struct v4l2_ctrl *dgain_ctrl;
	};
	struct v4l2_ctrl *ae_lock_ctrl;
	struct v4l2_ctrl *ae_bias_ctrl;
	struct v4l2_ctrl *ae_target_ctrl;
	struct v4l2_ctrl *ae_step_prop_ctrl;
	struct v4l2_ctrl *ae_leak_prop_ctrl;
	struct v4l2_ctrl *slave_ctrl;
	struct v4l2_ctrl *led_ctrl;
	bool streaming;
	const struct vd56g3_mode *current_mode;
	u32 mbus_code;
};

static inline struct vd56g3 *to_vd56g3(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vd56g3, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct vd56g3, ctrl_handler)->sd;
}

/* ----------------------------------------------------------------------------
 * HW access
 */

static int get_chunk_size(struct vd56g3 *sensor)
{
	int max_write_len = VD56G3_WRITE_MULTIPLE_CHUNK_MAX;
	struct i2c_adapter *adapter = sensor->i2c_client->adapter;

	if (adapter->quirks && adapter->quirks->max_write_len)
		max_write_len = adapter->quirks->max_write_len - 2;

	max_write_len = min(max_write_len, VD56G3_WRITE_MULTIPLE_CHUNK_MAX);

	return max(max_write_len, 1);
}

static int vd56g3_read_multiple(struct vd56g3 *sensor, u32 reg,
				unsigned int len)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	u8 buf[2];
	u8 val[sizeof(u32)] = { 0 };
	int ret;

	if (len > sizeof(u32))
		return -EINVAL;
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = len;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_dbg(&client->dev, "%s: %x i2c_transfer, reg: %x => %d\n",
			__func__, client->addr, reg, ret);
		return ret;
	}

	return get_unaligned_le32(val);
}

static inline int vd56g3_read(struct vd56g3 *sensor, u32 reg)
{
	return vd56g3_read_multiple(sensor, reg & VD56G3_REG_ADDR_MASK,
				    (reg >> VD56G3_REG_SIZE_SHIFT) & 7);
}

static int vd56g3_write_multiple(struct vd56g3 *sensor, u32 reg, const u8 *data,
				 unsigned int len, int *err)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[VD56G3_WRITE_MULTIPLE_CHUNK_MAX + 2];
	unsigned int i;
	int ret;

	if (err && *err)
		return *err;

	if (len > VD56G3_WRITE_MULTIPLE_CHUNK_MAX)
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
		if (err)
			*err = ret;
		return ret;
	}

	return 0;
}

static inline int vd56g3_write(struct vd56g3 *sensor, u32 reg, u32 val,
			       int *err)
{
	return vd56g3_write_multiple(sensor, reg & VD56G3_REG_ADDR_MASK,
				     (u8 *)&val,
				     (reg >> VD56G3_REG_SIZE_SHIFT) & 7, err);
}

static int vd56g3_write_array(struct vd56g3 *sensor, u32 reg, unsigned int nb,
			      const u8 *array)
{
	const unsigned int chunk_size = get_chunk_size(sensor);
	int ret;
	unsigned int sz;

	while (nb) {
		sz = min(nb, chunk_size);
		ret = vd56g3_write_multiple(sensor, reg, array, sz, NULL);
		if (ret < 0)
			return ret;
		nb -= sz;
		reg += sz;
		array += sz;
	}

	return 0;
}

static int vd56g3_poll_reg(struct vd56g3 *sensor, u32 reg, u8 poll_val)
{
	const unsigned int loop_delay_ms = 10;
	const unsigned int timeout_ms = 500;
	int ret;
#if KERNEL_VERSION(5, 7, 0) > LINUX_VERSION_CODE
	int loop_nb = timeout_ms / loop_delay_ms;

	while (--loop_nb) {
		ret = vd56g3_read(sensor, reg);
		if (ret < 0)
			return ret;
		if (ret == poll_val)
			return 0;
		msleep(loop_delay_ms);
	}
	return -ETIMEDOUT;
#else
	return read_poll_timeout(vd56g3_read, ret,
				 ((ret < 0) || (ret == poll_val)),
				 loop_delay_ms * 1000, timeout_ms * 1000, false,
				 sensor, reg);
#endif
}

static int vd56g3_wait_state(struct vd56g3 *sensor, int state)
{
	return vd56g3_poll_reg(sensor, VD56G3_REG_SYSTEM_FSM, state);
}

/* ----------------------------------------------------------------------------
 * Controls: definitions, helpers and handlers
 */

static const char *const vd56g3_test_pattern_menu[] = {
	"Disabled",
	"Solid",
	"Colorbar",
	"Gradbar",
	"Hgrey",
	"Vgrey",
	"Dgrey",
	"PN28"
};

static const s64 vd56g3_ev_bias_qmenu[] = { -4000, -3500, -3000, -2500, -2000,
					    -1500, -1000, -500,	 0,	500,
					    1000,  1500,  2000,	 2500,	3000,
					    3500,  4000 };

static const s64 vd56g3_link_freq_1lane[] = { VD56G3_LINK_FREQ_DEF_1LANE };

static const s64 vd56g3_link_freq_2lanes[] = { VD56G3_LINK_FREQ_DEF_2LANES };

static u8 vd56g3_get_bpp(__u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
		return 8;
	case MEDIA_BUS_FMT_Y10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return 10;
	default:
		/* Should never happen */
		WARN(1, "Unsupported code %d. default to 8 bpp", code);
	}

	return 8;
}

static u8 vd56g3_get_datatype(__u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
		return MIPI_CSI2_DT_RAW8;
	case MEDIA_BUS_FMT_Y10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return MIPI_CSI2_DT_RAW10;
	default:
		/* Should never happen */
		WARN(1, "Unsupported code %d. default to MIPI_CSI2_DT_RAW8",
		     code);
	}

	return MIPI_CSI2_DT_RAW8;
}

static int vd56g3_get_temp_stream_enable(struct vd56g3 *sensor, int *temp)
{
	int temperature;

	temperature = vd56g3_read(sensor, VD56G3_REG_TEMPERATURE);
	if (temperature < 0)
		return temperature;

	*temp = temperature;

	return 0;
}

static int vd56g3_get_temp_stream_disable(struct vd56g3 *sensor, int *temp)
{
	int ret;

	/* request temperature read */
	ret = vd56g3_write(sensor, VD56G3_REG_STBY, VD56G3_CMD_THSENS_READ,
			   NULL);
	if (ret)
		return ret;
	ret = vd56g3_poll_reg(sensor, VD56G3_REG_STBY, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	return vd56g3_get_temp_stream_enable(sensor, temp);
}

static int vd56g3_get_temp(struct vd56g3 *sensor, int *temp)
{
	*temp = 0;
	if (sensor->streaming)
		return vd56g3_get_temp_stream_enable(sensor, temp);
	else
		return vd56g3_get_temp_stream_disable(sensor, temp);
}

static int vd56g3_get_expo_cluster(struct vd56g3 *sensor, bool force_cur_val)
{
	int exposure;
	int again;
	int dgain;

	/* When 'force_cur_val' is enabled, save the ctrl value in 'cur.val'
	 * instead of the normal 'val', this is used during poweroff to cache
	 * volatile ctrls and enable coldstart.
	 */
	exposure = vd56g3_read(sensor, VD56G3_REG_APPLIED_COARSE_EXPOSURE);
	if (exposure < 0)
		return exposure;
	if (force_cur_val)
		sensor->expo_ctrl->cur.val = exposure;
	else
		sensor->expo_ctrl->val = exposure;

	again = vd56g3_read(sensor, VD56G3_REG_APPLIED_ANALOG_GAIN);
	if (again < 0)
		return again;
	if (force_cur_val)
		sensor->again_ctrl->cur.val = again;
	else
		sensor->again_ctrl->val = again;

	dgain = vd56g3_read(sensor, VD56G3_REG_APPLIED_DIGITAL_GAIN);
	if (dgain < 0)
		return dgain;
	if (force_cur_val)
		sensor->dgain_ctrl->cur.val = dgain;
	else
		sensor->dgain_ctrl->val = dgain;

	return 0;
}

static int vd56g3_update_patgen(struct vd56g3 *sensor, u32 patgen_index)
{
	u32 pattern = patgen_index <= 3 ? patgen_index : patgen_index + 12;
	u16 patgen = pattern << 4;
	u8 duster = VD56G3_DUSTER_ENABLE_DEF_MODULES;
	u8 darkcal = VD56G3_DARKCAL_ENABLE;
	int ret = 0;

	if (patgen_index) {
		patgen |= 1;
		duster = VD56G3_DUSTER_DISABLE;
		darkcal = VD56G3_DARKCAL_DISABLE_DARKAVG;
	}

	vd56g3_write(sensor, VD56G3_REG_DUSTER_CTRL, duster, &ret);
	vd56g3_write(sensor, VD56G3_REG_DARKCAL_CTRL, darkcal, &ret);
	vd56g3_write(sensor, VD56G3_REG_PATGEN_CTRL, patgen, &ret);

	return ret;
}

static int vd56g3_update_expo_cluster(struct vd56g3 *sensor, bool is_auto)
{
	int ret = 0;
	enum vd56g3_expo_state expo_state = is_auto ? VD56G3_EXP_MODE_AUTO :
						      VD56G3_EXP_MODE_MANUAL;

	if (sensor->ae_ctrl->is_new)
		vd56g3_write(sensor, VD56G3_REG_EXP_MODE, expo_state, &ret);

	// In Auto expo, set coldstart parameters
	if (is_auto && sensor->ae_ctrl->is_new) {
		vd56g3_write(sensor, VD56G3_REG_AE_COLDSTART_COARSE_EXPOSURE,
			     sensor->expo_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_AE_COLDSTART_ANALOG_GAIN,
			     sensor->again_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_AE_COLDSTART_DIGITAL_GAIN,
			     sensor->dgain_ctrl->val, &ret);
	}

	// In Manual expo, set exposure, analog and digital gains
	if (!is_auto && sensor->expo_ctrl->is_new)
		vd56g3_write(sensor, VD56G3_REG_MANUAL_COARSE_EXPOSURE,
			     sensor->expo_ctrl->val, &ret);

	if (!is_auto && sensor->again_ctrl->is_new)
		vd56g3_write(sensor, VD56G3_REG_MANUAL_ANALOG_GAIN,
			     sensor->again_ctrl->val, &ret);

	if (!is_auto && sensor->dgain_ctrl->is_new) {
		vd56g3_write(sensor, VD56G3_REG_MANUAL_DIGITAL_GAIN_CH0,
			     sensor->dgain_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_MANUAL_DIGITAL_GAIN_CH1,
			     sensor->dgain_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_MANUAL_DIGITAL_GAIN_CH2,
			     sensor->dgain_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_MANUAL_DIGITAL_GAIN_CH3,
			     sensor->dgain_ctrl->val, &ret);
	}

	return ret;
}

static int vd56g3_lock_exposure(struct vd56g3 *sensor, u32 lock_val)
{
	bool ae_lock = lock_val & V4L2_LOCK_EXPOSURE;
	enum vd56g3_expo_state expo_state = ae_lock ? VD56G3_EXP_MODE_FREEZE :
						      VD56G3_EXP_MODE_AUTO;
	int ret = 0;

	if (sensor->ae_ctrl->val == V4L2_EXPOSURE_AUTO)
		vd56g3_write(sensor, VD56G3_REG_EXP_MODE, expo_state, &ret);

	return ret;
}

static int vd56g3_write_gpiox(struct vd56g3 *sensor, unsigned long gpio_mask)
{
	unsigned long io;
	u32 gpio_val;
	int ret = 0;

	for_each_set_bit(io, &gpio_mask, VD56G3_NB_GPIOS) {
		gpio_val = sensor->gpios[io];

		if ((gpio_val == VD56G3_GPIOX_VT_SLAVE_MODE) &&
		    !sensor->slave_ctrl->val)
			gpio_val = VD56G3_GPIOX_GPIO_IN;

		if ((gpio_val == VD56G3_GPIOX_STROBE_MODE) &&
		    sensor->led_ctrl->val == V4L2_FLASH_LED_MODE_NONE)
			gpio_val = VD56G3_GPIOX_GPIO_IN;

		vd56g3_write(sensor, VD56G3_REG_GPIO_0_CTRL + io, gpio_val,
			     &ret);
	}

	return ret;
}

static int vd56g3_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int temperature;
	int ret = 0;

	/* Interact with HW only when it is powered ON */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_TEMPERATURE:
		ret = vd56g3_get_temp(sensor, &temperature);
		if (ret)
			break;
		ctrl->val = temperature;
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		ret = vd56g3_get_expo_cluster(sensor, false);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_mark_last_busy(&client->dev);
	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static int vd56g3_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	unsigned int frame_length;
	unsigned int expo_max;
	bool is_auto;
	int ret;

	if (ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)
		return 0;

	/* Update controls state, range, etc. whatever the state of the HW*/
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		frame_length = sensor->current_mode->crop.height + ctrl->val;
		expo_max = frame_length - VD56G3_EXPOSURE_OFFSET;
		__v4l2_ctrl_modify_range(sensor->expo_ctrl, 0, expo_max, 1,
					 VD56G3_EXPOSURE_DEFAULT);
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		is_auto = (ctrl->val == V4L2_EXPOSURE_AUTO);
#if KERNEL_VERSION(4, 20, 0) > LINUX_VERSION_CODE
		mutex_unlock(&sensor->lock);
		v4l2_ctrl_grab(sensor->ae_lock_ctrl, !is_auto);
		v4l2_ctrl_grab(sensor->ae_bias_ctrl, !is_auto);
		v4l2_ctrl_grab(sensor->ae_target_ctrl, !is_auto);
		v4l2_ctrl_grab(sensor->ae_step_prop_ctrl, !is_auto);
		v4l2_ctrl_grab(sensor->ae_leak_prop_ctrl, !is_auto);
		mutex_lock(&sensor->lock);
#else
		__v4l2_ctrl_grab(sensor->ae_lock_ctrl, !is_auto);
		__v4l2_ctrl_grab(sensor->ae_bias_ctrl, !is_auto);
		__v4l2_ctrl_grab(sensor->ae_target_ctrl, !is_auto);
		__v4l2_ctrl_grab(sensor->ae_step_prop_ctrl, !is_auto);
		__v4l2_ctrl_grab(sensor->ae_leak_prop_ctrl, !is_auto);
#endif
		break;
	default:
		break;
	}

	/* Interact with HW only when it is powered ON */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ret = vd56g3_write(sensor, VD56G3_REG_ORIENTATION,
				   sensor->hflip_ctrl->val |
					   (sensor->vflip_ctrl->val << 1),
				   NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = vd56g3_update_patgen(sensor, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		ret = vd56g3_update_expo_cluster(sensor, is_auto);
		break;
	case V4L2_CID_3A_LOCK:
		ret = vd56g3_lock_exposure(sensor, ctrl->val);
		break;
	case V4L2_CID_AUTO_EXPOSURE_BIAS:
		ret = vd56g3_write(
			sensor, VD56G3_REG_AE_COMPENSATION,
			DIV_ROUND_CLOSEST((int)vd56g3_ev_bias_qmenu[ctrl->val] *
						  256,
					  1000),
			NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = vd56g3_write(sensor, VD56G3_REG_FRAME_LENGTH,
				   frame_length, NULL);
		break;
	case V4L2_CID_SLAVE_MODE:
		ret = vd56g3_write_gpiox(sensor, BIT(0));
		ret = vd56g3_write(sensor, VD56G3_REG_VT_CTRL, ctrl->val, &ret);
		break;
	case V4L2_CID_FLASH_LED_MODE:
		ret = vd56g3_write_gpiox(sensor, sensor->ext_leds_mask);
		break;
	case V4L2_CID_AE_TARGET_PERCENTAGE:
		ret = vd56g3_write(sensor, VD56G3_REG_AE_TARGET_PERCENTAGE,
				   ctrl->val, NULL);
		break;
	case V4L2_CID_AE_STEP_PROPORTION:
		ret = vd56g3_write(sensor, VD56G3_REG_AE_STEP_PROPORTION,
				   ctrl->val, NULL);
		break;
	case V4L2_CID_AE_LEAK_PROPORTION:
		ret = vd56g3_write(sensor, VD56G3_REG_AE_LEAK_PROPORTION,
				   ctrl->val, NULL);
		break;
	case V4L2_CID_DARKCAL_PEDESTAL:
		ret = vd56g3_write(sensor, VD56G3_REG_DARKCAL_PEDESTAL,
				   ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_mark_last_busy(&client->dev);
	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops vd56g3_ctrl_ops = {
	.g_volatile_ctrl = vd56g3_g_volatile_ctrl,
	.s_ctrl = vd56g3_s_ctrl,
};

/**
 * DOC: Temperature Control
 *
 * Return sensor temperature (in Celsius)
 *
 * :id:     ``V4L2_CID_TEMPERATURE``
 * :type:   ``V4L2_CTRL_TYPE_INTEGER``
 */
static const struct v4l2_ctrl_config vd56g3_temp_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_TEMPERATURE,
	.name		= "Temperature in celsius",
	.type		= V4L2_CTRL_TYPE_INTEGER,
	.min		= -1024,
	.max		= 1023,
	.step		= 1,
};

/**
 * DOC: AE - Light level target (%)
 *
 * The AE algorithm targets a level of luminance as a percent of the saturation
 * level.
 *
 * Lower value means darker image, Higher value means brighter image (100% means
 * saturated image).
 *
 * :id:     ``V4L2_CID_AE_TARGET_PERCENTAGE``
 * :type:   ``V4L2_CTRL_TYPE_INTEGER``
 * :min:    0
 * :max:    100
 * :def:    30
 */
static const struct v4l2_ctrl_config vd56g3_ae_target_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_AE_TARGET_PERCENTAGE,
	.name		= "AE - Light level target (%)",
	.type		= V4L2_CTRL_TYPE_INTEGER,
	.min		= 0,
	.max		= 100,
	.def		= 30,
	.step		= 1
};

/**
 * DOC: AE - Convergence step proportion
 *
 * The AE convergence loop can be fined tuned.
 *
 * This parameter controls convergence speed. It is the damping factor for the
 * exposure variation. The step proportion range is [0.0 .. 1.0] :
 *
 *    - 0.0, the exposure is frozen
 *    - 0.5, operation is at half speed (default)
 *    - 1.0, operation is at full speed (not recommended)
 *
 * The current V4L2 control is expressed in **Fixed Point 8.8**
 *
 * :id:     ``V4L2_CID_AE_STEP_PROPORTION``
 * :type:   ``V4L2_CTRL_TYPE_INTEGER``
 * :min:    0.0 (0x000)
 * :max:    1.0 (0x100)
 * :def:    0.54 (0x08c)
 */
static const struct v4l2_ctrl_config vd56g3_ae_step_prop_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_AE_STEP_PROPORTION,
	.name		= "AE - Convg. step proportion",
	.type		= V4L2_CTRL_TYPE_INTEGER,
	.min		= 0x000,
	.max		= 0x100,
	.def		= 0x08c,
	.step		= 1
};

/**
 * DOC: AE - Convergence leak proportion
 *
 * The AE convergence loop can be fined tuned.
 *
 * This parameter controls reactivity.
 * The convergence algorithm contains a leaky integrator that average the image
 * statistics over time and smoothes the luminance transition.
 *
 * The leak proportion range is [0.0 .. 1.0] :
 *
 *    - 0.0, statistics are frozen at the previous value
 *    - 1.0, statistics are forced to the current frame statistic
 *
 * The current V4L2 control is expressed in **Fixed Point 1.15**
 *
 * :id:     ``V4L2_CID_AE_LEAK_PROPORTION``
 * :type:   ``V4L2_CTRL_TYPE_INTEGER``
 * :min:    0.0  (0x0000)
 * :max:    1.0  (0x8000)
 * :def:    0.35 (0x2ccc)
 */
static const struct v4l2_ctrl_config vd56g3_ae_leak_prop_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_AE_LEAK_PROPORTION,
	.name		= "AE - Convg. leak proportion",
	.type		= V4L2_CTRL_TYPE_INTEGER,
	.min		= 0x0000,
	.max		= 0x8000,
	.def		= 0x2ccc,
	.step		= 1
};

/**
 * DOC: Dark Calibration Pedestal
 *
 * The device embeds an automatic dark calibration mechanism.
 * This controls allows to set the dark calibration target.
 *
 * :id:     ``V4L2_CID_DARKCAL_PEDESTAL``
 * :type:   ``V4L2_CTRL_TYPE_INTEGER``
 * :min:    0
 * :max:    255
 * :def:    64
 *
 */
static const struct v4l2_ctrl_config vd56g3_darkcal_pedestal_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_DARKCAL_PEDESTAL,
	.name		= "Dark Calibration Pedestal",
	.type		= V4L2_CTRL_TYPE_INTEGER,
	.min		= 0,
	.max		= 255,
	.step		= 1,
	.def		= 0x40,
};

/**
 * DOC: VT Slave Mode Control
 *
 * When the 'st,in-sync' property of the device tree is enabled on gpio0,
 * this control allows to enable/disable the Slave Mode of the sensor
 *
 * :id:     ``V4L2_CID_SLAVE``
 * :type:   ``V4L2_CTRL_TYPE_BOOLEAN``
 * :min:    False
 * :max:    True
 * :def:    True
 *
 */
static const struct v4l2_ctrl_config vd56g3_slave_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_SLAVE_MODE,
	.name		= "VT Slave Mode",
	.type		= V4L2_CTRL_TYPE_BOOLEAN,
	.min		= 0,
	.max		= 1,
	.step		= 1,
	.def		= 1,
};

#if KERNEL_VERSION(4, 13, 0) > LINUX_VERSION_CODE
/**
 * DOC: Digital Gain Control
 *
 * Digital gain [1.00, 8.00] is coded as a Fixed Point 5.8
 *
 * :id:     ``V4L2_CID_DIGITAL_GAIN``
 * :type:   ``V4L2_CTRL_TYPE_INTEGER``
 * :min:    0x100
 * :max:    0x800
 * :def:    0x100
 */
static const struct v4l2_ctrl_config vd56g3_dgain_ctrl = {
	.ops		= &vd56g3_ctrl_ops,
	.id		= V4L2_CID_DIGITAL_GAIN,
	.name		= "Digital Gain",
	.type		= V4L2_CTRL_TYPE_INTEGER,
	.min		= 0x100,
	.max		= 0x800,
	.step		= 1,
	.def		= 0x100,
};
#endif

static void vd56g3_update_controls(struct vd56g3 *sensor)
{
	unsigned int hblank =
		VD56G3_LINE_LENGTH_MIN - sensor->current_mode->crop.width;
	unsigned int vblank_min =
		VD56G3_FRAME_LENGTH_MIN - sensor->current_mode->crop.height;
	unsigned int vblank = VD56G3_FRAME_LENGTH_DEF_60FPS -
			      sensor->current_mode->crop.height;
	unsigned int vblank_max = 0xffff - sensor->current_mode->crop.height;
	unsigned int frame_length = sensor->current_mode->crop.height + vblank;
	unsigned int expo_max = frame_length - VD56G3_EXPOSURE_OFFSET;

	/* Update blanking and exposure (ranges + values) */
	__v4l2_ctrl_modify_range(sensor->hblank_ctrl, hblank, hblank, 1,
				 hblank);
	__v4l2_ctrl_modify_range(sensor->vblank_ctrl, vblank_min, vblank_max, 1,
				 vblank);
	__v4l2_ctrl_s_ctrl(sensor->vblank_ctrl, vblank);
	__v4l2_ctrl_modify_range(sensor->expo_ctrl, 0, expo_max, 1,
				 VD56G3_EXPOSURE_DEFAULT);
	__v4l2_ctrl_s_ctrl(sensor->expo_ctrl, VD56G3_EXPOSURE_DEFAULT);
}

static int vd56g3_init_controls(struct vd56g3 *sensor)
{
	const struct v4l2_ctrl_ops *ops = &vd56g3_ctrl_ops;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 25);

	/* we can use our own mutex for the ctrl lock */
	hdl->lock = &sensor->lock;

	/* Horizontal & vertival Flips modify bayer code with RGB variant*/
	sensor->hflip_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
#if KERNEL_VERSION(4, 12, 0) > LINUX_VERSION_CODE
#else
	if (sensor->hflip_ctrl)
		sensor->hflip_ctrl->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
#endif
	sensor->vflip_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
#if KERNEL_VERSION(4, 12, 0) > LINUX_VERSION_CODE
#else
	if (sensor->vflip_ctrl)
		sensor->vflip_ctrl->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
#endif

	sensor->patgen_ctrl = v4l2_ctrl_new_std_menu_items(
		hdl, ops, V4L2_CID_TEST_PATTERN,
		ARRAY_SIZE(vd56g3_test_pattern_menu) - 1, 0, 0,
		vd56g3_test_pattern_menu);

	ctrl = v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(vd56g3_link_freq_1lane) - 1, 0,
				      (sensor->nb_of_lane == 2) ?
					      vd56g3_link_freq_2lanes :
					      vd56g3_link_freq_1lane);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE,
				 sensor->pixel_clock, sensor->pixel_clock, 1,
				 sensor->pixel_clock);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->ae_ctrl = v4l2_ctrl_new_std_menu(hdl, ops,
						 V4L2_CID_EXPOSURE_AUTO,
						 V4L2_EXPOSURE_MANUAL, 0,
						 V4L2_EXPOSURE_AUTO);

	sensor->ae_lock_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_3A_LOCK, 0, 7, 0, 0);

	sensor->ae_bias_ctrl = v4l2_ctrl_new_int_menu(
		hdl, ops, V4L2_CID_AUTO_EXPOSURE_BIAS,
		ARRAY_SIZE(vd56g3_ev_bias_qmenu) - 1,
		(ARRAY_SIZE(vd56g3_ev_bias_qmenu) + 1) / 2 - 1,
		vd56g3_ev_bias_qmenu);

	/*
	 * Analog gain [1, 8] is computed with the following logic :
	 * 32/(32 − again_reg), with again_reg in the range [0:28]
	 * Digital gain [1.00, 8.00] is coded as a Fixed Point 5.8
	 */
	sensor->again_ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_ANALOGUE_GAIN,
					       0, 28, 1, 0);
#if KERNEL_VERSION(4, 13, 0) > LINUX_VERSION_CODE
	sensor->dgain_ctrl =
		v4l2_ctrl_new_custom(hdl, &vd56g3_dgain_ctrl, NULL);
#else
	sensor->dgain_ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_DIGITAL_GAIN,
					       0x100, 0x800, 1, 0x100);
#endif

	/*
	 * Set the exposure, horizontal and vertical blanking ctrls
	 * to hardcoded values, they will be updated in vd56g3_ctrl_update().
	 */
	sensor->expo_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE, 1, 1, 1, 1);
	sensor->hblank_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HBLANK, 1, 1, 1, 1);
	if (sensor->hblank_ctrl)
		sensor->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->vblank_ctrl =
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VBLANK, 1, 1, 1, 1);

	/* Custom controls : temperature, custom AE ctrls, pedestal */
	ctrl = v4l2_ctrl_new_custom(hdl, &vd56g3_temp_ctrl, NULL);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE |
			       V4L2_CTRL_FLAG_READ_ONLY;
	sensor->ae_target_ctrl =
		v4l2_ctrl_new_custom(hdl, &vd56g3_ae_target_ctrl, NULL);
	sensor->ae_step_prop_ctrl =
		v4l2_ctrl_new_custom(hdl, &vd56g3_ae_step_prop_ctrl, NULL);
	sensor->ae_leak_prop_ctrl =
		v4l2_ctrl_new_custom(hdl, &vd56g3_ae_leak_prop_ctrl, NULL);
	v4l2_ctrl_new_custom(hdl, &vd56g3_darkcal_pedestal_ctrl, NULL);

	/* Addition controls based on device tree properties */
	if (sensor->ext_vt_sync)
		sensor->slave_ctrl =
			v4l2_ctrl_new_custom(hdl, &vd56g3_slave_ctrl, NULL);
	if (sensor->ext_leds_mask)
		sensor->led_ctrl = v4l2_ctrl_new_std_menu(
			hdl, ops, V4L2_CID_FLASH_LED_MODE,
			V4L2_FLASH_LED_MODE_FLASH, 0,
			V4L2_FLASH_LED_MODE_FLASH);

	if (hdl->error) {
		ret = hdl->error;
		goto free_ctrls;
	}

	v4l2_ctrl_cluster(2, &sensor->hflip_ctrl);
	v4l2_ctrl_auto_cluster(4, &sensor->ae_ctrl, V4L2_EXPOSURE_MANUAL, true);

	sensor->sd.ctrl_handler = hdl;
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

/* ----------------------------------------------------------------------------
 * Videos ops
 */

static int vd56g3_stream_on(struct vd56g3 *sensor)
{
	const struct v4l2_rect *crop = &sensor->current_mode->crop;
	int ret = 0;
	unsigned int csi_mbps = ((sensor->nb_of_lane == 2) ?
					 VD56G3_LINK_FREQ_DEF_2LANES :
					 VD56G3_LINK_FREQ_DEF_1LANE) * 2 / MEGA;
	/* configure clocks */
	vd56g3_write(sensor, VD56G3_REG_EXT_CLOCK, sensor->ext_clock, &ret);
	vd56g3_write(sensor, VD56G3_REG_CLK_PLL_PREDIV, sensor->pll_prediv,
		     &ret);
	vd56g3_write(sensor, VD56G3_REG_CLK_SYS_PLL_MULT, sensor->pll_mult,
		     &ret);

	/* configure output */
	vd56g3_write(sensor, VD56G3_REG_FORMAT_CTRL,
		     vd56g3_get_bpp(sensor->mbus_code), &ret);
	vd56g3_write(sensor, VD56G3_REG_OIF_CTRL, sensor->oif_ctrl, &ret);
	vd56g3_write(sensor, VD56G3_REG_OIF_CSI_BITRATE, csi_mbps, &ret);
	vd56g3_write(sensor, VD56G3_REG_OIF_IMG_CTRL,
		     vd56g3_get_datatype(sensor->mbus_code), &ret);

	/* configure size and bin mode */
	vd56g3_write(sensor, VD56G3_REG_READOUT_CTRL,
		     sensor->current_mode->bin_mode, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_X_START, crop->left, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_X_END,
		     crop->left + crop->width - 1, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_Y_START, crop->top, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_Y_END,
		     crop->top + crop->height - 1, &ret);
	if (ret)
		return ret;

	/* Setup default GPIO values; could be overriden by V4L2 ctrl setup */
	ret = vd56g3_write_gpiox(sensor, GENMASK(VD56G3_NB_GPIOS - 1, 0));
	if (ret)
		return ret;

	/* Apply settings from V4L2 ctrls */
#if KERNEL_VERSION(4, 13, 0) > LINUX_VERSION_CODE
	mutex_unlock(&sensor->lock);
	ret = v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
	mutex_lock(&sensor->lock);
#else
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
#endif
	if (ret)
		return ret;

	/* start streaming */
	ret = vd56g3_write(sensor, VD56G3_REG_STBY, VD56G3_CMD_START_STREAM,
			   NULL);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, VD56G3_REG_STBY, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_STREAMING);
	if (ret)
		return ret;

	return 0;
}

static int vd56g3_stream_off(struct vd56g3 *sensor)
{
	int ret;

	/* Retrieve Expo cluster to enable coldstart of AE*/
	ret = vd56g3_get_expo_cluster(sensor, true);
	if (ret)
		return ret;

	ret = vd56g3_write(sensor, VD56G3_REG_STREAMING, VD56G3_CMD_STOP_STREAM,
			   NULL);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, VD56G3_REG_STREAMING, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY);
	if (ret)
		return ret;

	return 0;
}

static int vd56g3_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);

	if (enable) {
#if KERNEL_VERSION(5, 10, 0) > LINUX_VERSION_CODE
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock;
		}
#else
		ret = pm_runtime_resume_and_get(&client->dev);
		if (ret < 0)
			goto unlock;
#endif
		ret = vd56g3_stream_on(sensor);
		if (ret) {
			dev_err(&client->dev, "Failed to start streaming\n");
			pm_runtime_put_sync(&client->dev);
		}
	} else {
		vd56g3_stream_off(sensor);
		pm_runtime_mark_last_busy(&client->dev);
		pm_runtime_put_autosuspend(&client->dev);
	}

unlock:
	mutex_unlock(&sensor->lock);

	if (!ret) {
		sensor->streaming = enable;

		/* some controls are locked during streaming */
		v4l2_ctrl_grab(sensor->hflip_ctrl, enable);
		v4l2_ctrl_grab(sensor->vflip_ctrl, enable);
		v4l2_ctrl_grab(sensor->patgen_ctrl, enable);
		if (sensor->ext_vt_sync)
			v4l2_ctrl_grab(sensor->slave_ctrl, enable);
	}

	return ret;
}

static const struct v4l2_subdev_video_ops vd56g3_video_ops = {
	.s_stream = vd56g3_s_stream,
};

/* ----------------------------------------------------------------------------
 * Pad ops
 */

/* Media bus code is dependant of :
 *      - 8bits or 10bits output
 *      - variant : Mono or RGB
 *      - H/V flips parameters in case of RGB
 */
static u32 vd56g3_get_mbus_code(struct vd56g3 *sensor, u32 code)
{
	unsigned int i_bpp;
	unsigned int j;

	for (i_bpp = 0; i_bpp < ARRAY_SIZE(vd56g3_mbus_codes); i_bpp++) {
		for (j = 0; j < ARRAY_SIZE(vd56g3_mbus_codes[i_bpp]); j++) {
			if (vd56g3_mbus_codes[i_bpp][j] == code)
				goto endloops;
		}
	}

endloops:
	if (i_bpp >= ARRAY_SIZE(vd56g3_mbus_codes))
		i_bpp = 0;

	if (!sensor->is_rgb)
		j = 0;
	else
		j = 1 + (sensor->hflip_ctrl->val ? 1 : 0) +
		    (sensor->vflip_ctrl->val ? 2 : 0);

	return vd56g3_mbus_codes[i_bpp][j];
}

static int vd56g3_enum_mbus_code(struct v4l2_subdev *sd,
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
				 struct v4l2_subdev_pad_config *cfg,
#else
				 struct v4l2_subdev_state *sd_state,
#endif
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct vd56g3 *sensor = to_vd56g3(sd);

	if (code->index >= ARRAY_SIZE(vd56g3_mbus_codes))
		return -EINVAL;

	code->code =
		vd56g3_get_mbus_code(sensor, vd56g3_mbus_codes[code->index][0]);

	return 0;
}

static int vd56g3_enum_frame_size(struct v4l2_subdev *sd,
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
				  struct v4l2_subdev_pad_config *cfg,
#else
				  struct v4l2_subdev_state *sd_state,
#endif
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = sensor->i2c_client;

	dev_dbg(&client->dev, "%s for index %d", __func__, fse->index);
	if (fse->pad != 0)
		return -EINVAL;
	if (fse->index >= ARRAY_SIZE(vd56g3_supported_modes))
		return -EINVAL;

	fse->min_width = vd56g3_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = vd56g3_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void vd56g3_update_pad_format(struct vd56g3 *sensor,
				     const struct vd56g3_mode *mode,
				     u32 mbus_code,
				     struct v4l2_mbus_framefmt *mbus_fmt)
{
	mbus_fmt->width = mode->width;
	mbus_fmt->height = mode->height;
	mbus_fmt->code = vd56g3_get_mbus_code(sensor, mbus_code);
	mbus_fmt->colorspace = V4L2_COLORSPACE_RAW;
	mbus_fmt->field = V4L2_FIELD_NONE;
	mbus_fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	mbus_fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	mbus_fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int vd56g3_get_fmt(struct v4l2_subdev *sd,
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
			  struct v4l2_subdev_pad_config *cfg,
#else
			  struct v4l2_subdev_state *sd_state,
#endif
			  struct v4l2_subdev_format *sd_fmt)
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = sensor->i2c_client;
	struct v4l2_mbus_framefmt *pad_fmt;

	mutex_lock(&sensor->lock);

	if (sd_fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
		pad_fmt = v4l2_subdev_get_try_format(&sensor->sd, cfg,
						     sd_fmt->pad);
#elif KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
		pad_fmt = v4l2_subdev_get_try_format(&sensor->sd, sd_state,
						     sd_fmt->pad);
#else
		pad_fmt = v4l2_subdev_get_pad_format(&sensor->sd, sd_state,
						     sd_fmt->pad);
#endif
		pad_fmt->code = vd56g3_get_mbus_code(sensor, pad_fmt->code);
		sd_fmt->format = *pad_fmt;
	} else {
		vd56g3_update_pad_format(sensor, sensor->current_mode,
					 sensor->mbus_code, &sd_fmt->format);
	}

	mutex_unlock(&sensor->lock);

	dev_dbg(&client->dev, "%s[%d] <- [pad%d]:%dx%d", __func__,
		sd_fmt->which, sd_fmt->pad, sd_fmt->format.width,
		sd_fmt->format.height);

	return 0;
}

static const struct vd56g3_mode *
vd56g3_find_nearest_size(struct v4l2_mbus_framefmt *fmt)
{
	const struct vd56g3_mode *mode = vd56g3_supported_modes;
	unsigned int i;

	/* select size */
	for (i = 0; i < ARRAY_SIZE(vd56g3_supported_modes); i++) {
		if (mode->width <= fmt->width && mode->height <= fmt->height)
			break;
		mode++;
	}
	if (i == ARRAY_SIZE(vd56g3_supported_modes))
		mode--;

	return mode;
}

static int vd56g3_set_fmt(struct v4l2_subdev *sd,
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
			  struct v4l2_subdev_pad_config *cfg,
#else
			  struct v4l2_subdev_state *sd_state,
#endif
			  struct v4l2_subdev_format *sd_fmt)
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = sensor->i2c_client;
	const struct vd56g3_mode *new_mode;
	struct v4l2_mbus_framefmt *pad_fmt;
	int ret = 0;

	dev_dbg(&client->dev, "%s[%d] -> [pad%d]:%dx%d", __func__,
		sd_fmt->which, sd_fmt->pad, sd_fmt->format.width,
		sd_fmt->format.height);

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	/* find best format */
	new_mode = vd56g3_find_nearest_size(&sd_fmt->format);
	vd56g3_update_pad_format(sensor, new_mode, sd_fmt->format.code,
				 &sd_fmt->format);

	if (sd_fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
		pad_fmt = v4l2_subdev_get_try_format(sd, cfg, sd_fmt->pad);
#elif KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
		pad_fmt = v4l2_subdev_get_try_format(sd, sd_state, sd_fmt->pad);
#else
		pad_fmt = v4l2_subdev_get_pad_format(sd, sd_state, sd_fmt->pad);
#endif
		*pad_fmt = sd_fmt->format;
	} else if (sensor->current_mode != new_mode ||
		   sensor->mbus_code != sd_fmt->format.code) {
		// This nested 'if' only avoid to reset ctrls while format
		// hasn't changed (userspace pb, we shouldn't interfere ?)
		sensor->current_mode = new_mode;
		sensor->mbus_code = sd_fmt->format.code;

		vd56g3_update_controls(sensor);
	}

out:
	mutex_unlock(&sensor->lock);

	return ret;
}

static int vd56g3_get_selection(struct v4l2_subdev *sd,
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
				struct v4l2_subdev_pad_config *cfg,
#else
				struct v4l2_subdev_state *sd_state,
#endif
				struct v4l2_subdev_selection *sel)
{
	struct vd56g3 *sensor = to_vd56g3(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = sensor->current_mode->crop;
		break;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = VD56G3_NATIVE_WIDTH;
		sel->r.height = VD56G3_NATIVE_HEIGHT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

#if KERNEL_VERSION(4, 7, 0) > LINUX_VERSION_CODE
#else
static int vd56g3_init_cfg(struct v4l2_subdev *sd,
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
			   struct v4l2_subdev_pad_config *cfg)
#else
			   struct v4l2_subdev_state *sd_state)
#endif
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct v4l2_mbus_framefmt *pad_fmt;

#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
	pad_fmt = v4l2_subdev_get_try_format(sd, cfg, 0);
#elif KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	pad_fmt = v4l2_subdev_get_try_format(sd, sd_state, 0);
#else
	pad_fmt = v4l2_subdev_get_pad_format(sd, sd_state, 0);
#endif

	/* Default resolution mode / raw8 */
	vd56g3_update_pad_format(sensor, &vd56g3_supported_modes[1],
				 vd56g3_mbus_codes[0][0], pad_fmt);
	return 0;
}
#endif

static const struct v4l2_subdev_core_ops vd56g3_core_ops = {};

static const struct v4l2_subdev_pad_ops vd56g3_pad_ops = {
#if KERNEL_VERSION(4, 7, 0) > LINUX_VERSION_CODE
#else
	.init_cfg = vd56g3_init_cfg,
#endif
	.enum_mbus_code = vd56g3_enum_mbus_code,
	.enum_frame_size = vd56g3_enum_frame_size,
	.get_fmt = vd56g3_get_fmt,
	.set_fmt = vd56g3_set_fmt,
	.get_selection = vd56g3_get_selection,
};

static const struct v4l2_subdev_ops vd56g3_subdev_ops = {
	.core = &vd56g3_core_ops,
	.video = &vd56g3_video_ops,
	.pad = &vd56g3_pad_ops,
};

static const struct media_entity_operations vd56g3_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* ----------------------------------------------------------------------------
 * Boot section (includes FMW and VT Patch)
 */

static int vd56g3_patch(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	const u8 *patch = patch_cut2;
	int patch_size = sizeof(patch_cut2);
	u8 patch_major;
	u8 patch_minor;
	int cur_patch_rev;
	int ret;

	patch_major = patch[3];
	patch_minor = patch[2];

	ret = vd56g3_write_array(sensor, 0x2000, patch_size, patch);
	if (ret)
		return ret;

	ret = vd56g3_write(sensor, VD56G3_REG_BOOT, VD56G3_CMD_PATCH_SETUP,
			   NULL);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, VD56G3_REG_BOOT, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	cur_patch_rev = vd56g3_read(sensor, VD56G3_REG_FWPATCH_REVISION);
	if (cur_patch_rev < 0)
		return cur_patch_rev;

	if (cur_patch_rev != (patch_major << 8) + patch_minor) {
		dev_err(&client->dev,
			"bad patch version expected %d.%d got %d.%d",
			patch_major, patch_minor, cur_patch_rev >> 8,
			cur_patch_rev & 0xff);
		return -ENODEV;
	}
	dev_info(&client->dev, "patch %d.%d applied", cur_patch_rev >> 8,
		 cur_patch_rev & 0xff);

	return 0;
}

static int vd56g3_boot(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = vd56g3_write(sensor, VD56G3_REG_BOOT, VD56G3_CMD_BOOT, NULL);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, VD56G3_REG_BOOT, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY);
	if (ret)
		return ret;

	dev_info(&client->dev, "sensor boot successfully");

	return 0;
}

static int vd56g3_vtpatch(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int i;
	int vtpatch_offset = 0;
	int cur_vtpatch_rd_rev, cur_vtpatch_gr_rev, cur_vtpatch_gt_rev;
	int ret = 0;

	ret = vd56g3_write(sensor, VD56G3_REG_VTPATCHING,
			   VD56G3_CMD_START_VTRAM_UPDATE, NULL);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, VD56G3_REG_VTPATCHING, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY);
	if (ret)
		return ret;

	for (i = 0; i < vtpatch_area_nb; i++) {
		ret = vd56g3_write_array(sensor, vtpatch_desc[i].offset,
					 vtpatch_desc[i].size,
					 vtpatch + vtpatch_offset);
		if (ret)
			return ret;

		vtpatch_offset += vtpatch_desc[i].size;
	}
	// TODO replace with correct register names
	vd56g3_write(sensor, VD56G3_REG_8BIT(0xd9f8), VT_REVISION, &ret);
	vd56g3_write(sensor, VD56G3_REG_8BIT(0xaffc), VT_REVISION, &ret);
	vd56g3_write(sensor, VD56G3_REG_8BIT(0xbbb4), VT_REVISION, &ret);
	vd56g3_write(sensor, VD56G3_REG_8BIT(0xb898), VT_REVISION, &ret);

	vd56g3_write(sensor, VD56G3_REG_VTPATCHING, VD56G3_CMD_END_VTRAM_UPDATE,
		     &ret);
	if (ret)
		return ret;

	ret = vd56g3_poll_reg(sensor, VD56G3_REG_VTPATCHING, VD56G3_CMD_ACK);
	if (ret)
		return ret;

	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY);
	if (ret)
		return ret;

	cur_vtpatch_rd_rev =
		vd56g3_read(sensor, VD56G3_REG_VTIMING_RD_REVISION);
	if (cur_vtpatch_rd_rev < 0)
		return cur_vtpatch_rd_rev;
	cur_vtpatch_gr_rev =
		vd56g3_read(sensor, VD56G3_REG_VTIMING_GR_REVISION);
	if (cur_vtpatch_gr_rev < 0)
		return cur_vtpatch_gr_rev;
	cur_vtpatch_gt_rev =
		vd56g3_read(sensor, VD56G3_REG_VTIMING_GT_REVISION);
	if (cur_vtpatch_gt_rev < 0)
		return cur_vtpatch_gt_rev;

	if (cur_vtpatch_rd_rev != VT_REVISION ||
	    cur_vtpatch_gr_rev != VT_REVISION ||
	    cur_vtpatch_gt_rev != VT_REVISION) {
		dev_err(&client->dev,
			"bad vtpatch version, expected %d got rd:%d, gr:%d gt:%d",
			VT_REVISION, cur_vtpatch_rd_rev, cur_vtpatch_gr_rev,
			cur_vtpatch_gt_rev);
		return -ENODEV;
	}
	dev_info(&client->dev, "VT patch %d applied", VT_REVISION);

	return 0;
}

/* ----------------------------------------------------------------------------
 * Power management
 */

static int vd56g3_power_on(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = regulator_bulk_enable(VD56G3_NUM_SUPPLIES, sensor->supplies);
	if (ret) {
		dev_err(&client->dev, "Failed to enable regulators %d", ret);
		return ret;
	}

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(&client->dev, "Failed to enable clock %d", ret);
		goto disable_reg;
	}

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_READY_TO_BOOT);
	if (ret) {
		dev_err(&client->dev, "Sensor reset failed %d\n", ret);
		goto disable_clock;
	}

	return 0;

disable_clock:
	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
disable_reg:
	regulator_bulk_disable(VD56G3_NUM_SUPPLIES, sensor->supplies);

	return ret;
}

static int vd56g3_power_patch(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = vd56g3_power_on(sensor);
	if (ret) {
		dev_err(&client->dev, "Failed to power on %d", ret);
		return ret;
	}

	ret = vd56g3_patch(sensor);
	if (ret) {
		dev_err(&client->dev, "sensor patch failed %d", ret);
		return ret;
	}

	ret = vd56g3_boot(sensor);
	if (ret) {
		dev_err(&client->dev, "sensor boot failed %d", ret);
		return ret;
	}

	ret = vd56g3_vtpatch(sensor);
	if (ret) {
		dev_err(&client->dev, "sensor VT patch failed %d", ret);
		return ret;
	}

	return 0;
}

static int vd56g3_power_off(struct vd56g3 *sensor)
{
	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	regulator_bulk_disable(VD56G3_NUM_SUPPLIES, sensor->supplies);
	return 0;
}

static int vd56g3_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct vd56g3 *vd56g3 = to_vd56g3(sd);

	return vd56g3_power_patch(vd56g3);
}

static int vd56g3_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct vd56g3 *vd56g3 = to_vd56g3(sd);

	return vd56g3_power_off(vd56g3);
}

static const struct dev_pm_ops vd56g3_pm_ops = { SET_RUNTIME_PM_OPS(
	vd56g3_runtime_suspend, vd56g3_runtime_resume, NULL) };

/* ----------------------------------------------------------------------------
 * Probe and initialization
 */

static int vd56g3_check_csi_conf(struct vd56g3 *sensor,
				 struct fwnode_handle *endpoint)
{
	struct i2c_client *client = sensor->i2c_client;
#if KERNEL_VERSION(4, 20, 0) > LINUX_VERSION_CODE
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2 };
#else
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
#endif
	u32 phy_data_lanes[VD56G3_MAX_CSI_DATA_LANES] = { ~0, ~0 };
	int n_lanes;
	int p, l;
	int ret = 0;

#if KERNEL_VERSION(4, 20, 0) > LINUX_VERSION_CODE
	struct v4l2_fwnode_endpoint *ep_ptr =
		v4l2_fwnode_endpoint_alloc_parse(endpoint);
	if (IS_ERR(ep_ptr))
		return -EINVAL;
	ep = (*ep_ptr);
#else
	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	if (ret)
		return -EINVAL;
#endif

	// Check lanes number
	n_lanes = ep.bus.mipi_csi2.num_data_lanes;
	if (n_lanes != 1 && n_lanes != 2) {
		dev_err(&client->dev, "Invalid data lane number %d\n", n_lanes);
		ret = -EINVAL;
		goto done;
	}
	sensor->nb_of_lane = n_lanes;

	// Clock lane must be first
	if (ep.bus.mipi_csi2.clock_lane != 0) {
		dev_err(&client->dev, "Clk lane must be mapped to lane 0\n");
		ret = -EINVAL;
		goto done;
	}

	// Prepare Output Interface conf based on lane settings
	// logical to physical lane conversion (+ pad remaining slots)
	for (l = 0; l < n_lanes; l++)
		phy_data_lanes[ep.bus.mipi_csi2.data_lanes[l] - 1] = l;
	for (p = 0; p < VD56G3_MAX_CSI_DATA_LANES; p++) {
		if (phy_data_lanes[p] != ~0)
			continue;
		phy_data_lanes[p] = l;
		l++;
	}
	sensor->oif_ctrl = n_lanes |
			   (ep.bus.mipi_csi2.lane_polarities[0] << 3) |
			   ((phy_data_lanes[0]) << 4) |
			   (ep.bus.mipi_csi2.lane_polarities[1] << 6) |
			   ((phy_data_lanes[1]) << 7) |
			   (ep.bus.mipi_csi2.lane_polarities[2] << 9);

	// Check link frequency
	if (!ep.nr_of_link_frequencies) {
		dev_err(&client->dev, "link-frequency not found in DT\n");
		ret = -EINVAL;
		goto done;
	}
	if (ep.nr_of_link_frequencies != 1 ||
	    (ep.link_frequencies[0] != ((n_lanes == 2) ?
						VD56G3_LINK_FREQ_DEF_2LANES :
						VD56G3_LINK_FREQ_DEF_1LANE))) {
		dev_err(&client->dev, "Link frequency not supported: %lld\n",
			ep.link_frequencies[0]);
		ret = -EINVAL;
		goto done;
	}

done:
#if KERNEL_VERSION(4, 20, 0) > LINUX_VERSION_CODE
	v4l2_fwnode_endpoint_free(ep_ptr);
#else
	v4l2_fwnode_endpoint_free(&ep);
#endif

	return ret;
}

static int vd56g3_parse_dt_gpios_array(struct vd56g3 *sensor, char *prop_name,
				       u32 *array, int *nb)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device_node *np = client->dev.of_node;
	unsigned int i;

	*nb = of_property_read_variable_u32_array(np, prop_name, array, 0,
						  VD56G3_NB_GPIOS);

	if (*nb == -EINVAL) {
		*nb = 0;
		return *nb;
	} else if (*nb < 0) {
		dev_err(&client->dev, "Failed to read %s prop\n", prop_name);
		return *nb;
	}

	for (i = 0; i < *nb; i++) {
		if (array[i] >= VD56G3_NB_GPIOS) {
			dev_err(&client->dev, "Invalid GPIO : %d\n", array[i]);
			return -EINVAL;
		}
	}

	return 0;
}

static int vd56g3_parse_dt_gpios(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device_node *np = client->dev.of_node;
	u32 led_gpios[VD56G3_NB_GPIOS];
	int nb_gpios_leds;
	u32 out_sync_gpios[VD56G3_NB_GPIOS];
	int nb_gpios_out;
	u32 in_sync_gpio;
	int ret;
	unsigned int i;

	/* Initialize GPIOs to default */
	for (i = 0; i < VD56G3_NB_GPIOS; i++)
		sensor->gpios[i] = VD56G3_GPIOX_GPIO_IN;
	sensor->ext_leds_mask = 0;

	/* Take into account optional 'st,leds' output for GPIOs */
	ret = vd56g3_parse_dt_gpios_array(sensor, "st,leds", led_gpios,
					  &nb_gpios_leds);
	if (ret)
		return ret;

	for (i = 0; i < nb_gpios_leds; i++) {
		sensor->gpios[led_gpios[i]] = VD56G3_GPIOX_STROBE_MODE;
		set_bit(led_gpios[i], &sensor->ext_leds_mask);
	}

	/* Take into account optional 'st,out-sync' output for GPIOs */
	ret = vd56g3_parse_dt_gpios_array(sensor, "st,out-sync", out_sync_gpios,
					  &nb_gpios_out);
	if (ret)
		return ret;

	for (i = 0; i < nb_gpios_out; i++) {
		if (sensor->gpios[out_sync_gpios[i]] != VD56G3_GPIOX_GPIO_IN) {
			dev_err(&client->dev, "Multiple use of GPIO %d\n",
				out_sync_gpios[i]);
			return -EINVAL;
		}
		sensor->gpios[out_sync_gpios[i]] = VD56G3_GPIOX_FSYNC_OUT;
	}

	/* Take into account optional 'st,in-sync' input for GPIO0 */
	ret = of_property_read_u32(np, "st,in-sync", &in_sync_gpio);
	if (ret < 0 && ret != -EINVAL) {
		dev_err(&client->dev, "Failed to read st,in-sync prop\n");
		return ret;
	} else if (ret == -EINVAL) {
		sensor->ext_vt_sync = false;
	} else {
		if (in_sync_gpio != 0) {
			dev_err(&client->dev, "in-sync GPIO must be gpio0\n");
			return -EINVAL;
		}
		if (sensor->gpios[in_sync_gpio] != VD56G3_GPIOX_GPIO_IN) {
			dev_err(&client->dev, "Multiple use of GPIO %d\n",
				in_sync_gpio);
			return -EINVAL;
		}
		sensor->gpios[in_sync_gpio] = VD56G3_GPIOX_VT_SLAVE_MODE;
		sensor->ext_vt_sync = true;
	}

	return 0;
}

static int vd56g3_parse_dt(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	int ret;

#if KERNEL_VERSION(5, 2, 0) > LINUX_VERSION_CODE
	endpoint = fwnode_graph_get_next_endpoint(
		of_fwnode_handle(dev->of_node), NULL);
#else
	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
#endif
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = vd56g3_check_csi_conf(sensor, endpoint);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;

	ret = vd56g3_parse_dt_gpios(sensor);
	if (ret)
		return ret;

	return 0;
}

static int vd56g3_get_regulators(struct vd56g3 *sensor)
{
	int i;

	for (i = 0; i < VD56G3_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = vd56g3_supply_names[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       VD56G3_NUM_SUPPLIES, sensor->supplies);
}

static int vd56g3_prepare_clock_tree(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	const unsigned int predivs[] = { 1, 2, 4 };
	u32 pll_out;
	int i;

	/* External clock must be in [6Mhz-27Mhz] */
	if (sensor->ext_clock < 6 * HZ_PER_MHZ ||
	    sensor->ext_clock > 27 * HZ_PER_MHZ) {
		dev_err(&client->dev,
			"Only 6Mhz-27Mhz clock range supported. provide %lu MHz\n",
			sensor->ext_clock / HZ_PER_MHZ);
		return -EINVAL;
	}

	/* PLL input should be in [6Mhz-12Mhz[ */
	for (i = 0; i < ARRAY_SIZE(predivs); i++) {
		sensor->pll_prediv = predivs[i];
		if (sensor->ext_clock / sensor->pll_prediv < 12 * HZ_PER_MHZ)
			break;
	}

	/* PLL output clock must be as close as possible to 804Mhz */
	sensor->pll_mult = (VD56G3_TARGET_PLL * sensor->pll_prediv +
			    sensor->ext_clock / 2) /
			   sensor->ext_clock;

	/* Pixel Clock = PLL Output Clock / VD56G3_VT_CLOCK_DIV ≈ 160.8Mhz */
	pll_out = sensor->ext_clock * sensor->pll_mult / sensor->pll_prediv;
	sensor->pixel_clock = pll_out / VD56G3_VT_CLOCK_DIV;

	dev_dbg(&client->dev, "Ext Clock = %d Hz", sensor->ext_clock);
	dev_dbg(&client->dev, "PLL prediv = %d", sensor->pll_prediv);
	dev_dbg(&client->dev, "PLL mult = %d", sensor->pll_mult);
	dev_dbg(&client->dev, "PLL Output = %dHz", pll_out);
	dev_dbg(&client->dev, "Pixel Clock = %dHz", sensor->pixel_clock);

	return 0;
}

static int vd56g3_detect(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int id = 0;
	int device_revision = 0;
	int optical_revision = 0;

	id = vd56g3_read(sensor, VD56G3_REG_MODEL_ID);
	if (id < 0)
		return id;

	if (id != VD56G3_MODEL_ID) {
		dev_warn(&client->dev, "Unsupported sensor id %x", id);
		return -ENODEV;
	}

	device_revision = vd56g3_read(sensor, VD56G3_REG_REVISION);
	if (device_revision < 0)
		return device_revision;

	if ((device_revision >> 8) != 0x20) {
		dev_warn(&client->dev, "Unsupported Cut version %x",
			 device_revision);
		return -ENODEV;
	}

	optical_revision = vd56g3_read(sensor, VD56G3_REG_OPTICAL_REVISION);
	if (optical_revision < 0)
		return optical_revision;

	sensor->is_rgb = ((optical_revision & 1) == 1);

	return 0;
}

static int vd56g3_subdev_init(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	mutex_init(&sensor->lock);
	sensor->streaming = false;
	/* Default resolution mode / raw8 */
	sensor->current_mode = &vd56g3_supported_modes[1];
	sensor->mbus_code = vd56g3_mbus_codes[0][0];

	v4l2_i2c_subdev_init(&sensor->sd, client, &vd56g3_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.ops = &vd56g3_subdev_entity_ops;
#if KERNEL_VERSION(4, 5, 0) > LINUX_VERSION_CODE
	sensor->sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_init(&sensor->sd.entity, 1, &sensor->pad, 0);
#else
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
#endif

	if (ret) {
		dev_err(&client->dev, "Failed to init media entity : %d", ret);
		return ret;
	}

	ret = vd56g3_init_controls(sensor);
	if (ret) {
		dev_err(&client->dev, "Controls initialization failed %d", ret);
		goto err_media;
	}

	vd56g3_update_controls(sensor);

	return 0;

err_media:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static void vd56g3_subdev_cleanup(struct vd56g3 *sensor)
{
	v4l2_async_unregister_subdev(&sensor->sd);
	mutex_destroy(&sensor->lock);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(sensor->sd.ctrl_handler);
}

#if KERNEL_VERSION(4, 10, 0) > LINUX_VERSION_CODE
static int vd56g3_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#else
static int vd56g3_probe(struct i2c_client *client)
#endif
{
	struct device *dev = &client->dev;
	struct vd56g3 *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	ret = vd56g3_parse_dt(sensor);
	if (ret) {
		dev_err(&client->dev, "Failed to parse Device Tree : %d", ret);
		return ret;
	}

	/* Get (and check) ressources : power regs, ext clock, reset gpio */
	ret = vd56g3_get_regulators(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulators.");

	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "Failed to get xclk.");
	sensor->ext_clock = clk_get_rate(sensor->xclk);
	ret = vd56g3_prepare_clock_tree(sensor);
	if (ret)
		return ret;

	sensor->reset_gpio =
		devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "Failed to get reset gpio.");

	/* Power ON */
	ret = vd56g3_power_on(sensor);
	if (ret) {
		dev_err(&client->dev, "Sensor power on failed : %d", ret);
		return ret;
	}

	/* Enable PM runtime with autosuspend (sensor being ON, set active) */
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 4000);
	pm_runtime_use_autosuspend(dev);

	/* Check HW model/version */
	ret = vd56g3_boot(sensor);
	if (ret) {
		dev_err(&client->dev, "Sensor boot failed : %d", ret);
		goto err_power_off;
	}

	ret = vd56g3_detect(sensor);
	if (ret) {
		dev_err(&client->dev, "Sensor detect failed : %d", ret);
		goto err_power_off;
	}

	/* Initialize, then register V4L2 subdev. */
	ret = vd56g3_subdev_init(sensor);
	if (ret) {
		dev_err(&client->dev, "V4l2 init failed : %d", ret);
		goto err_power_off;
	}

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		dev_err(&client->dev, "async subdev register failed %d", ret);
		goto err_subdev;
	}

	/* Sensor could now be powered off (after the autosuspend delay) */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(&client->dev, "vd56g3 probe successfully");

	return 0;

err_subdev:
	vd56g3_subdev_cleanup(sensor);
err_power_off:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	vd56g3_power_off(sensor);
	return ret;
}

#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
static int vd56g3_remove(struct i2c_client *client)
#else
static void vd56g3_remove(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vd56g3 *sensor = to_vd56g3(sd);

	vd56g3_subdev_cleanup(sensor);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		vd56g3_power_off(sensor);
	pm_runtime_set_suspended(&client->dev);
#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
	return 0;
#endif
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
		.pm = &vd56g3_pm_ops,
	},
#if KERNEL_VERSION(4, 10, 0) > LINUX_VERSION_CODE
	.probe = vd56g3_probe,
#elif KERNEL_VERSION(6, 3, 0) > LINUX_VERSION_CODE
	.probe_new = vd56g3_probe,
#else
	.probe = vd56g3_probe,
#endif
	.remove = vd56g3_remove,
};

module_i2c_driver(vd56g3_i2c_driver);

MODULE_AUTHOR("Mickael Guene <mickael.guene@st.com>");
MODULE_AUTHOR("Sylvain Petinot <sylvain.petinot@foss.st.com>");
MODULE_DESCRIPTION("VD56G3 camera subdev driver");
MODULE_LICENSE("GPL");
