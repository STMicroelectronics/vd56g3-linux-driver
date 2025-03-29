// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for ST VD56G3 (Mono) and VD66GY (RGB) global shutter cameras.
 * Copyright (C) 2019, STMicroelectronics SA
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <asm/unaligned.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Backward compatibility */
#include <linux/version.h>

#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
/*
 * Warning : CCI_REGxy_LE definitions doesn't fit exactly with v4l2-cci.h .
 * In fact endianness is managed directly in vd56g3_read/write() functions.
 */
#include <linux/bitfield.h>
#define CCI_REG_ADDR_MASK		GENMASK(15, 0)
#define CCI_REG_WIDTH_SHIFT		16
#define CCI_REG_ADDR(x)			FIELD_GET(CCI_REG_ADDR_MASK, x)
#define CCI_REG8(x)			((1 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG16_LE(x)			((2 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG32_LE(x)			((4 << CCI_REG_WIDTH_SHIFT) | (x))
#else
#include <media/v4l2-cci.h>
#endif

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

#if KERNEL_VERSION(4, 16, 0) > LINUX_VERSION_CODE
#include <linux/of_device.h>
#endif

#if KERNEL_VERSION(4, 5, 0) > LINUX_VERSION_CODE
int pm_runtime_get_if_in_use(struct device *dev)
{
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&dev->power.lock, flags);
	retval = dev->power.disable_depth > 0 ? -EINVAL :
		dev->power.runtime_status == RPM_ACTIVE &&
			atomic_inc_not_zero(&dev->power.usage_count);
	spin_unlock_irqrestore(&dev->power.lock, flags);
	return retval;
}
#endif

/* Register Map */
#define VD56G3_REG_MODEL_ID				CCI_REG16_LE(0x0000)
#define VD56G3_MODEL_ID					0x5603
#define VD56G3_REG_REVISION				CCI_REG16_LE(0x0002)
#define VD56G3_REVISION_CUT2				0x20
#define VD56G3_REVISION_CUT3				0x31
#define VD56G3_REG_OPTICAL_REVISION			CCI_REG8(0x001a)
#define VD56G3_OPTICAL_REVISION_MONO			0
#define VD56G3_OPTICAL_REVISION_BAYER			1
#define VD56G3_REG_FWPATCH_REVISION			CCI_REG16_LE(0x001e)
#define VD56G3_REG_VTPATCH_ID				CCI_REG8(0x0020)
#define VD56G3_REG_SYSTEM_FSM				CCI_REG8(0x0028)
#define VD56G3_SYSTEM_FSM_READY_TO_BOOT			0x01
#define VD56G3_SYSTEM_FSM_SW_STBY			0x02
#define VD56G3_SYSTEM_FSM_STREAMING			0x03
#define VD56G3_REG_TEMPERATURE				CCI_REG16_LE(0x004c)
#define VD56G3_REG_APPLIED_COARSE_EXPOSURE		CCI_REG16_LE(0x0064)
#define VD56G3_REG_APPLIED_ANALOG_GAIN			CCI_REG8(0x0068)
#define VD56G3_REG_APPLIED_DIGITAL_GAIN			CCI_REG16_LE(0x006a)
#define VD56G3_REG_BOOT					CCI_REG8(0x0200)
#define VD56G3_CMD_ACK					0
#define VD56G3_CMD_BOOT					1
#define VD56G3_CMD_PATCH_SETUP				2
#define VD56G3_REG_STBY					CCI_REG8(0x0201)
#define VD56G3_CMD_START_STREAM				1
#define VD56G3_CMD_THSENS_READ				4
#define VD56G3_REG_STREAMING				CCI_REG8(0x0202)
#define VD56G3_CMD_STOP_STREAM				1
#define VD56G3_REG_VTPATCHING				CCI_REG8(0x0203)
#define VD56G3_CMD_START_VTRAM_UPDATE			1
#define VD56G3_CMD_END_VTRAM_UPDATE			2
#define VD56G3_REG_EXT_CLOCK				CCI_REG32_LE(0x0220)
#define VD56G3_REG_CLK_PLL_PREDIV			CCI_REG8(0x0224)
#define VD56G3_REG_CLK_SYS_PLL_MULT			CCI_REG8(0x0226)
#define VD56G3_REG_ORIENTATION				CCI_REG8(0x0302)
#define VD56G3_REG_VT_CTRL				CCI_REG8(0x0309)
#define VD56G3_REG_FORMAT_CTRL				CCI_REG8(0x030a)
#define VD56G3_REG_OIF_CTRL				CCI_REG16_LE(0x030c)
#define VD56G3_REG_OIF_IMG_CTRL				CCI_REG8(0x030f)
#define VD56G3_REG_OIF_CSI_BITRATE			CCI_REG16_LE(0x0312)
#define VD56G3_REG_DUSTER_CTRL				CCI_REG8(0x0318)
#define VD56G3_DUSTER_DISABLE				0
#define VD56G3_DUSTER_ENABLE_DEF_MODULES		0x13
#define VD56G3_REG_ISL_ENABLE				CCI_REG8(0x0333)
#define VD56G3_REG_DARKCAL_CTRL				CCI_REG8(0x0340)
#define VD56G3_DARKCAL_ENABLE				1
#define VD56G3_DARKCAL_DISABLE_DARKAVG			2
#define VD56G3_REG_PATGEN_CTRL				CCI_REG16_LE(0x0400)
#define VD56G3_PATGEN_ENABLE				1
#define VD56G3_PATGEN_TYPE_SHIFT			4
#define VD56G3_REG_DARKCAL_PEDESTAL			CCI_REG8(0x0415)
#define VD56G3_REG_AE_COLDSTART_COARSE_EXPOSURE		CCI_REG16_LE(0x042a)
#define VD56G3_REG_AE_COLDSTART_ANALOG_GAIN		CCI_REG8(0x042c)
#define VD56G3_REG_AE_COLDSTART_DIGITAL_GAIN		CCI_REG16_LE(0x042e)
#define VD56G3_REG_AE_COMPILER_CONTROL			CCI_REG8(0x0430)
#define VD56G3_REG_AE_ROI_START_H			CCI_REG16_LE(0x0432)
#define VD56G3_REG_AE_ROI_START_V			CCI_REG16_LE(0x0434)
#define VD56G3_REG_AE_ROI_END_H				CCI_REG16_LE(0x0436)
#define VD56G3_REG_AE_ROI_END_V				CCI_REG16_LE(0x0438)
#define VD56G3_REG_AE_COMPENSATION			CCI_REG16_LE(0x043a)
#define VD56G3_REG_AE_TARGET_PERCENTAGE			CCI_REG16_LE(0x043c)
#define VD56G3_REG_AE_STEP_PROPORTION			CCI_REG16_LE(0x043e)
#define VD56G3_REG_AE_LEAK_PROPORTION			CCI_REG16_LE(0x0440)
#define VD56G3_REG_EXP_MODE				CCI_REG8(0x044c)
#define VD56G3_EXP_MODE_AUTO				0
#define VD56G3_EXP_MODE_FREEZE				1
#define VD56G3_EXP_MODE_MANUAL				2
#define VD56G3_REG_MANUAL_ANALOG_GAIN			CCI_REG8(0x044d)
#define VD56G3_REG_MANUAL_COARSE_EXPOSURE		CCI_REG16_LE(0x044e)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH0		CCI_REG16_LE(0x0450)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH1		CCI_REG16_LE(0x0452)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH2		CCI_REG16_LE(0x0454)
#define VD56G3_REG_MANUAL_DIGITAL_GAIN_CH3		CCI_REG16_LE(0x0456)
#define VD56G3_REG_FRAME_LENGTH				CCI_REG16_LE(0x0458)
#define VD56G3_REG_Y_START				CCI_REG16_LE(0x045a)
#define VD56G3_REG_Y_END				CCI_REG16_LE(0x045c)
#define VD56G3_REG_OUT_ROI_X_START			CCI_REG16_LE(0x045e)
#define VD56G3_REG_OUT_ROI_X_END			CCI_REG16_LE(0x0460)
#define VD56G3_REG_OUT_ROI_Y_START			CCI_REG16_LE(0x0462)
#define VD56G3_REG_OUT_ROI_Y_END			CCI_REG16_LE(0x0464)
#define VD56G3_REG_GPIO_0_CTRL				CCI_REG8(0x0467)
#define VD56G3_GPIOX_FSYNC_OUT				0x00
#define VD56G3_GPIOX_GPIO_IN				0x01
#define VD56G3_GPIOX_STROBE_MODE			0x02
#define VD56G3_GPIOX_VT_SLAVE_MODE			0x0a
#define VD56G3_REG_READOUT_CTRL				CCI_REG8(0x047e)
#define READOUT_NORMAL					0x00
#define READOUT_DIGITAL_BINNING_X2			0x01

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
 * The recommended/default resolution is 1120x1360 (multiple of 16).
 */
#define VD56G3_NATIVE_WIDTH				1124
#define VD56G3_NATIVE_HEIGHT				1364
#define VD56G3_DEFAULT_WIDTH				1120
#define VD56G3_DEFAULT_HEIGHT				1360
#define VD56G3_DEFAULT_MODE				1

/* PLL settings */
#define VD56G3_TARGET_PLL				804000000UL
#define VD56G3_VT_CLOCK_DIV				5

/* Line length and Frame length (settings are for standard 10bits ADC mode) */
#define VD56G3_LINE_LENGTH_MIN				1236
#define VD56G3_VBLANK_MIN				110
#define VD56G3_FRAME_LENGTH_DEF_60FPS			2168
#define VD56G3_FRAME_LENGTH_MAX				0xffff

/* Exposure settings */
#define VD56G3_EXPOSURE_MARGIN				75
#define VD56G3_EXPOSURE_MIN				21
#define VD56G3_EXPOSURE_DEFAULT				1420U

/* Output Interface settings */
#define VD56G3_MAX_CSI_DATA_LANES			2
#define VD56G3_LINK_FREQ_DEF_1LANE			750000000UL
#define VD56G3_LINK_FREQ_DEF_2LANES			402000000UL

/* GPIOs */
#define VD56G3_NB_GPIOS					8

/* parse-SNIP: Custom-CIDs */
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

#include "vd56g3_patch_cut2.c"
#include "vd56g3_vtpatch.c"

/* regulator supplies */
static const char *const vd56g3_supply_names[] = {
	"vcore",
	"vddio",
	"vana",
};

/* -----------------------------------------------------------------------------
 * Models (VD56G3: Mono, VD66GY: Bayer RGB), Modes and formats
 */

enum vd56g3_models {
	VD56G3_MODEL_VD56G3,
	VD56G3_MODEL_VD66GY,
};

struct vd56g3_mode {
	u32 width;
	u32 height;
};

/**
 * DOC: Supported Modes
 *
 * The vd56g3 driver supports 9 modes described below :
 *
 * ======= ======== ====================
 *  Width   Height   Comment
 * ======= ======== ====================
 *   1124     1364   Native resolution
 *   1120     1360   Default resolution
 *   1024     1280
 *   1024      768
 *    768     1024
 *    720     1280
 *    640      480
 *    480      640   Enable Binning x2
 *    320      240   Enable Binning x2
 * ======= ======== ====================
 *
 * Each mode defaults to 60FPS. In addition, the framerate could be adjusted in
 * a continuous manner (making use of the ``V4L2_CID_VBLANK`` control).
 *
 * For native resolution the framerate can reach 88FPS.
 * For smaller resolutions, the maximum framerate will be much higher : for
 * example, it can reach 220FPS in 640x480 non-binned mode.
 */

static const struct vd56g3_mode vd56g3_supported_modes[] = {
	{
		.width = VD56G3_NATIVE_WIDTH,
		.height = VD56G3_NATIVE_HEIGHT,
	},
	{
		.width = VD56G3_DEFAULT_WIDTH,
		.height = VD56G3_DEFAULT_HEIGHT,
	},
	{
		.width = 1024,
		.height = 1280,
	},
	{
		.width = 1024,
		.height = 768,
	},
	{
		.width = 768,
		.height = 1024,
	},
	{
		.width = 720,
		.height = 1280,
	},
	{
		.width = 640,
		.height = 480,
	},
	{
		.width = 480,
		.height = 640,
	},
	{
		.width = 320,
		.height = 240,
	},
};

/*
 * Sensor support 8bits and 10bits output in both variants
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

#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
/* Big endian register addresses and 8b, 16b or 32b little endian values. */
static const struct regmap_config vd56g3_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
};
#endif

struct vd56g3 {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regulator_bulk_data supplies[ARRAY_SIZE(vd56g3_supply_names)];
	struct gpio_desc *reset_gpio;
	struct clk *xclk;
	struct regmap *regmap;
	u32 xclk_freq;
	u32 pll_prediv;
	u32 pll_mult;
	u32 pixel_clock;
	u16 oif_ctrl;
	u8 nb_of_lane;
	u32 gpios[VD56G3_NB_GPIOS];
	bool ext_vt_sync;
	unsigned long ext_leds_mask;
	bool is_mono;
	bool is_fastboot;
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	/* lock to protect all members below */
	struct mutex lock;
#endif
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
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	struct v4l2_mbus_framefmt active_fmt;
	struct v4l2_rect active_crop;
#endif
};

static inline struct vd56g3 *to_vd56g3(struct v4l2_subdev *sd)
{
#if KERNEL_VERSION(6, 2, 0) > LINUX_VERSION_CODE
	return container_of(sd, struct vd56g3, sd);
#else
	return container_of_const(sd, struct vd56g3, sd);
#endif
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
#if KERNEL_VERSION(6, 2, 0) > LINUX_VERSION_CODE
	return &container_of(ctrl->handler, struct vd56g3, ctrl_handler)->sd;
#else
	return &container_of_const(ctrl->handler, struct vd56g3, ctrl_handler)
			->sd;
#endif
}

/* -----------------------------------------------------------------------------
 * HW access : Big endian reg addresses and 8b, 16b or 32b little endian values
 */

#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
static int vd56g3_read(struct vd56g3 *sensor, u32 reg, u32 *val, int *err)
{
	struct i2c_client *client = sensor->i2c_client;
	unsigned int len = (reg >> CCI_REG_WIDTH_SHIFT) & 7;
	u8 buf[4];
	int ret;

	if (err && *err)
		return *err;

	reg = reg & CCI_REG_ADDR_MASK;

	ret = regmap_bulk_read(sensor->regmap, reg, buf, len);
	if (ret) {
		dev_err(&client->dev, "%s: Error reading reg 0x%4x: %d\n",
			__func__, reg, ret);
		goto out;
	}

	switch (len) {
	case 1:
		*val = buf[0];
		break;
	case 2:
		*val = get_unaligned_le16(buf);
		break;
	case 4:
		*val = get_unaligned_le32(buf);
		break;
	default:
		dev_err(&client->dev,
			"%s: Error invalid reg-width %u for reg 0x%04x\n",
			__func__, len, reg);
		ret = -EINVAL;
		break;
	}

out:
	if (ret && err)
		*err = ret;

	return ret;
}

static int vd56g3_write(struct vd56g3 *sensor, u32 reg, u32 val, int *err)
{
	struct i2c_client *client = sensor->i2c_client;
	unsigned int len = (reg >> CCI_REG_WIDTH_SHIFT) & 7;
	u8 buf[4];
	int ret;

	if (err && *err)
		return *err;

	reg = reg & CCI_REG_ADDR_MASK;
	switch (len) {
	case 1:
		buf[0] = val;
		break;
	case 2:
		put_unaligned_le16(val, buf);
		break;
	case 4:
		put_unaligned_le32(val, buf);
		break;
	default:
		dev_err(&client->dev,
			"%s: Error invalid reg-width %u for reg 0x%04x\n",
			__func__, len, reg);
		ret = -EINVAL;
		goto out;
	}

	ret = regmap_bulk_write(sensor->regmap, reg, buf, len);
	if (ret)
		dev_err(&client->dev, "%s: Error writing reg 0x%4x: %d\n",
			__func__, reg, ret);

out:
	if (ret && err)
		*err = ret;

	return ret;
}
#else
#define vd56g3_read(sensor, reg, val, err) \
	cci_read((sensor)->regmap, reg, (u64 *)val, err)

#define vd56g3_write(sensor, reg, val, err) \
	cci_write((sensor)->regmap, reg, (u64)val, err)
#endif

static int vd56g3_write_array(struct vd56g3 *sensor, u32 reg, unsigned int len,
			      const u8 *array, int *err)
{
	unsigned int chunk_sz = 1024;
	unsigned int sz;
	int ret;

	if (err && *err)
		return *err;

	/*
	 * This loop isn't necessary but in certains conditions (platforms, cpu
	 * load, etc.) it has been observed that the bulk write could timeout.
	 */
	while (len) {
		sz = min(len, chunk_sz);
		ret = regmap_bulk_write(sensor->regmap, reg, array, sz);
		if (ret < 0)
			goto out;
		len -= sz;
		reg += sz;
		array += sz;
	}

out:
	if (ret && err)
		*err = ret;

	return ret;
}

static int vd56g3_poll_reg(struct vd56g3 *sensor, u32 reg, u8 poll_val,
			   int *err)
{
	unsigned int val = 0;
	int ret;

	if (err && *err)
		return *err;

	ret = regmap_read_poll_timeout(sensor->regmap, CCI_REG_ADDR(reg), val,
				       (val == poll_val), 2000,
				       500 * USEC_PER_MSEC);

	if (ret && err)
		*err = ret;

	return ret;
}

static int vd56g3_wait_state(struct vd56g3 *sensor, int state, int *err)
{
	return vd56g3_poll_reg(sensor, VD56G3_REG_SYSTEM_FSM, state, err);
}

/* -----------------------------------------------------------------------------
 * Controls: definitions, helpers and handlers
 */

static const char *const vd56g3_tp_menu[] = { "Disabled", "Solid", "Colorbar",
					      "Gradbar",  "Hgrey", "Vgrey",
					      "Dgrey",	  "PN28" };

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
	default:
		return 8;
	case MEDIA_BUS_FMT_Y10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return 10;
	}
}

static u8 vd56g3_get_datatype(__u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	default:
		return MIPI_CSI2_DT_RAW8;
	case MEDIA_BUS_FMT_Y10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return MIPI_CSI2_DT_RAW10;
	}
}

static int vd56g3_get_temp_stream_enable(struct vd56g3 *sensor, int *temp)
{
	return vd56g3_read(sensor, VD56G3_REG_TEMPERATURE, temp, NULL);
}

static int vd56g3_get_temp_stream_disable(struct vd56g3 *sensor, int *temp)
{
	int ret = 0;

	/* request temperature read */
	vd56g3_write(sensor, VD56G3_REG_STBY, VD56G3_CMD_THSENS_READ, &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_STBY, VD56G3_CMD_ACK, &ret);
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

static int vd56g3_read_expo_cluster(struct vd56g3 *sensor, bool force_cur_val)
{
	int exposure = 0;
	int again = 0;
	int dgain = 0;
	int ret = 0;

	/*
	 * When 'force_cur_val' is enabled, save the ctrl value in 'cur.val'
	 * instead of the normal 'val', this is used during poweroff to cache
	 * volatile ctrls and enable coldstart.
	 */
	vd56g3_read(sensor, VD56G3_REG_APPLIED_COARSE_EXPOSURE, &exposure,
		    &ret);
	vd56g3_read(sensor, VD56G3_REG_APPLIED_ANALOG_GAIN, &again, &ret);
	vd56g3_read(sensor, VD56G3_REG_APPLIED_DIGITAL_GAIN, &dgain, &ret);
	if (ret)
		return ret;

	if (force_cur_val) {
		sensor->expo_ctrl->cur.val = exposure;
		sensor->again_ctrl->cur.val = again;
		sensor->dgain_ctrl->cur.val = dgain;
	} else {
		sensor->expo_ctrl->val = exposure;
		sensor->again_ctrl->val = again;
		sensor->dgain_ctrl->val = dgain;
	}

	return ret;
}

static int vd56g3_update_patgen(struct vd56g3 *sensor, u32 patgen_index)
{
	u32 pattern = patgen_index <= 3 ? patgen_index : patgen_index + 12;
	u16 patgen = pattern << VD56G3_PATGEN_TYPE_SHIFT;
	u8 duster = VD56G3_DUSTER_ENABLE_DEF_MODULES;
	u8 darkcal = VD56G3_DARKCAL_ENABLE;
	int ret = 0;

	if (patgen_index) {
		patgen |= VD56G3_PATGEN_ENABLE;
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
	u8 expo_state = is_auto ? VD56G3_EXP_MODE_AUTO : VD56G3_EXP_MODE_MANUAL;
	int ret = 0;

	if (sensor->ae_ctrl->is_new)
		vd56g3_write(sensor, VD56G3_REG_EXP_MODE, expo_state, &ret);

	/* In Auto expo, set coldstart parameters */
	if (is_auto && sensor->ae_ctrl->is_new) {
		vd56g3_write(sensor, VD56G3_REG_AE_COLDSTART_COARSE_EXPOSURE,
			     sensor->expo_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_AE_COLDSTART_ANALOG_GAIN,
			     sensor->again_ctrl->val, &ret);
		vd56g3_write(sensor, VD56G3_REG_AE_COLDSTART_DIGITAL_GAIN,
			     sensor->dgain_ctrl->val, &ret);
	}

	/* In Manual expo, set exposure, analog and digital gains */
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
	u8 expo_state = ae_lock ? VD56G3_EXP_MODE_FREEZE : VD56G3_EXP_MODE_AUTO;

	if (sensor->ae_ctrl->val == V4L2_EXPOSURE_AUTO)
		return vd56g3_write(sensor, VD56G3_REG_EXP_MODE, expo_state,
				    NULL);

	return 0;
}

static int vd56g3_write_gpiox(struct vd56g3 *sensor, unsigned long gpio_mask)
{
	unsigned long io;
	u32 gpio_val;
	int ret = 0;

	for_each_set_bit(io, &gpio_mask, VD56G3_NB_GPIOS) {
		gpio_val = sensor->gpios[io];

		if (gpio_val == VD56G3_GPIOX_VT_SLAVE_MODE &&
		    !sensor->slave_ctrl->val)
			gpio_val = VD56G3_GPIOX_GPIO_IN;

		if (gpio_val == VD56G3_GPIOX_STROBE_MODE &&
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
		ret = vd56g3_read_expo_cluster(sensor, false);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_mark_last_busy(&client->dev);
#if KERNEL_VERSION(6, 9, 0) > LINUX_VERSION_CODE
	pm_runtime_put_autosuspend(&client->dev);
#else
	__pm_runtime_put_autosuspend(&client->dev);
#endif

	return ret;
}

static int vd56g3_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
#else
	struct v4l2_subdev_state *state;
#endif
	const struct v4l2_rect *crop;
	unsigned int frame_length = 0;
	unsigned int expo_max;
	unsigned int ae_compensation;
	bool is_auto = false;
	int ret = 0;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	crop = &sensor->active_crop;
#elif KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
	state = v4l2_subdev_get_locked_active_state(sd);
	crop = v4l2_subdev_get_pad_crop(sd, state, 0);
#else
	state = v4l2_subdev_get_locked_active_state(sd);
	crop = v4l2_subdev_state_get_crop(state, 0);
#endif

	if (ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)
		return 0;

	/* Update controls state, range, etc. whatever the state of the HW */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		frame_length = crop->height + ctrl->val;
		expo_max = frame_length - VD56G3_EXPOSURE_MARGIN;
		ret = __v4l2_ctrl_modify_range(sensor->expo_ctrl,
					       VD56G3_EXPOSURE_MIN, expo_max, 1,
					       min(VD56G3_EXPOSURE_DEFAULT,
						   expo_max));
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

	if (ret)
		return ret;

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
		ae_compensation =
			DIV_ROUND_CLOSEST((int)vd56g3_ev_bias_qmenu[ctrl->val] *
					  256, 1000);
		ret = vd56g3_write(sensor, VD56G3_REG_AE_COMPENSATION,
				   ae_compensation, NULL);
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
#if KERNEL_VERSION(6, 9, 0) > LINUX_VERSION_CODE
	pm_runtime_put_autosuspend(&client->dev);
#else
	__pm_runtime_put_autosuspend(&client->dev);
#endif

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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_TEMPERATURE,
	.name = "Temperature in celsius",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = -1024,
	.max = 1023,
	.step = 1,
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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_AE_TARGET_PERCENTAGE,
	.name = "AE - Light level target (%)",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = 100,
	.def = 30,
	.step = 1
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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_AE_STEP_PROPORTION,
	.name = "AE - Convg. step proportion",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0x000,
	.max = 0x100,
	.def = 0x08c,
	.step = 1
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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_AE_LEAK_PROPORTION,
	.name = "AE - Convg. leak proportion",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0x0000,
	.max = 0x8000,
	.def = 0x2ccc,
	.step = 1
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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_DARKCAL_PEDESTAL,
	.name = "Dark Calibration Pedestal",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = 255,
	.step = 1,
	.def = 0x40,
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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_SLAVE_MODE,
	.name = "VT Slave Mode",
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 1,
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
	.ops = &vd56g3_ctrl_ops,
	.id = V4L2_CID_DIGITAL_GAIN,
	.name = "Digital Gain",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0x100,
	.max = 0x800,
	.step = 1,
	.def = 0x100,
};
#endif

static int vd56g3_update_controls(struct vd56g3 *sensor)
{
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
#else
	struct v4l2_subdev_state *state;
#endif
	const struct v4l2_rect *crop;
	unsigned int hblank;
	unsigned int vblank_min, vblank, vblank_max;
	unsigned int frame_length;
	unsigned int expo_max;
	int ret;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	crop = &sensor->active_crop;
#elif KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
	state = v4l2_subdev_get_locked_active_state(&sensor->sd);
	crop = v4l2_subdev_get_pad_crop(&sensor->sd, state, 0);
#else
	state = v4l2_subdev_get_locked_active_state(&sensor->sd);
	crop = v4l2_subdev_state_get_crop(state, 0);
#endif

	hblank = VD56G3_LINE_LENGTH_MIN - crop->width;
	vblank_min = VD56G3_VBLANK_MIN;
	vblank = VD56G3_FRAME_LENGTH_DEF_60FPS - crop->height;
	vblank_max = VD56G3_FRAME_LENGTH_MAX - crop->height;
	frame_length = crop->height + vblank;
	expo_max = frame_length - VD56G3_EXPOSURE_MARGIN;

	/* Update blanking and exposure (ranges + values) */
	ret = __v4l2_ctrl_modify_range(sensor->hblank_ctrl, hblank, hblank, 1,
				       hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(sensor->vblank_ctrl, vblank_min,
				       vblank_max, 1, vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(sensor->vblank_ctrl, vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(sensor->expo_ctrl, VD56G3_EXPOSURE_MIN,
				       expo_max, 1, VD56G3_EXPOSURE_DEFAULT);
	if (ret)
		return ret;

	return __v4l2_ctrl_s_ctrl(sensor->expo_ctrl, VD56G3_EXPOSURE_DEFAULT);
}

static int vd56g3_init_controls(struct vd56g3 *sensor)
{
	const struct v4l2_ctrl_ops *ops = &vd56g3_ctrl_ops;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
#if KERNEL_VERSION(5, 8, 0) > LINUX_VERSION_CODE
#else
	struct v4l2_fwnode_device_properties fwnode_props;
#endif
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 25);

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	/* we can use our own mutex for the ctrl lock */
	hdl->lock = &sensor->lock;
#endif

	/* Horizontal & vertical flips modify bayer code on RGB variant */
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

	sensor->patgen_ctrl =
		v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(vd56g3_tp_menu) - 1, 0,
					     0, vd56g3_tp_menu);

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

	sensor->ae_lock_ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_3A_LOCK, 0,
						 GENMASK(2, 0), 0, 0);

	sensor->ae_bias_ctrl =
		v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_AUTO_EXPOSURE_BIAS,
				       ARRAY_SIZE(vd56g3_ev_bias_qmenu) - 1,
				       ARRAY_SIZE(vd56g3_ev_bias_qmenu) / 2,
				       vd56g3_ev_bias_qmenu);

	/*
	 * Analog gain [1, 8] is computed with the following logic :
	 * 32/(32 - again_reg), with again_reg in the range [0:28]
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
	 * to hardcoded values, they will be updated in vd56g3_update_controls.
	 * Exposure being in an auto-cluster, set a significant value here.
	 */
	sensor->expo_ctrl = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
					      VD56G3_EXPOSURE_DEFAULT,
					      VD56G3_EXPOSURE_DEFAULT, 1,
					      VD56G3_EXPOSURE_DEFAULT);
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

	/* Additional controls based on device tree properties */
	if (sensor->ext_vt_sync)
		sensor->slave_ctrl =
			v4l2_ctrl_new_custom(hdl, &vd56g3_slave_ctrl, NULL);
	if (sensor->ext_leds_mask)
		sensor->led_ctrl =
			v4l2_ctrl_new_std_menu(hdl, ops,
					       V4L2_CID_FLASH_LED_MODE,
					       V4L2_FLASH_LED_MODE_FLASH, 0,
					       V4L2_FLASH_LED_MODE_NONE);

	if (hdl->error) {
		ret = hdl->error;
		goto free_ctrls;
	}

	v4l2_ctrl_cluster(2, &sensor->hflip_ctrl);
	v4l2_ctrl_auto_cluster(4, &sensor->ae_ctrl, V4L2_EXPOSURE_MANUAL, true);

#if KERNEL_VERSION(5, 8, 0) > LINUX_VERSION_CODE
#else
	/* Optional controls coming from fwnode (e.g. rotation, orientation). */
	ret = v4l2_fwnode_device_parse(&sensor->i2c_client->dev, &fwnode_props);
	if (ret)
		goto free_ctrls;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, ops, &fwnode_props);
	if (ret)
		goto free_ctrls;
#endif

	sensor->sd.ctrl_handler = hdl;

	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdl);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Videos ops
 */

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
static int vd56g3_stream_on(struct vd56g3 *sensor)
#else
static int vd56g3_stream_on(struct vd56g3 *sensor,
			    struct v4l2_subdev_state *state)
#endif
{
	const struct v4l2_mbus_framefmt *format;
	const struct v4l2_rect *crop;
	unsigned int csi_mbps = ((sensor->nb_of_lane == 2) ?
					 VD56G3_LINK_FREQ_DEF_2LANES :
					 VD56G3_LINK_FREQ_DEF_1LANE) * 2 / MEGA;
	unsigned int binning;
	int ret = 0;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	format = &sensor->active_fmt;
	crop = &sensor->active_crop;
#elif KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
	format = v4l2_subdev_get_pad_format(&sensor->sd, state, 0);
	crop = v4l2_subdev_get_pad_crop(&sensor->sd, state, 0);
#else
	format = v4l2_subdev_state_get_format(state, 0);
	crop = v4l2_subdev_state_get_crop(state, 0);
#endif

	/* configure clocks */
	vd56g3_write(sensor, VD56G3_REG_EXT_CLOCK, sensor->xclk_freq, &ret);
	vd56g3_write(sensor, VD56G3_REG_CLK_PLL_PREDIV, sensor->pll_prediv,
		     &ret);
	vd56g3_write(sensor, VD56G3_REG_CLK_SYS_PLL_MULT, sensor->pll_mult,
		     &ret);

	/* configure output */
	vd56g3_write(sensor, VD56G3_REG_FORMAT_CTRL,
		     vd56g3_get_bpp(format->code), &ret);
	vd56g3_write(sensor, VD56G3_REG_OIF_CTRL, sensor->oif_ctrl, &ret);
	vd56g3_write(sensor, VD56G3_REG_OIF_CSI_BITRATE, csi_mbps, &ret);
	vd56g3_write(sensor, VD56G3_REG_OIF_IMG_CTRL,
		     vd56g3_get_datatype(format->code), &ret);
	vd56g3_write(sensor, VD56G3_REG_ISL_ENABLE, 0, &ret);

	/* configure binning mode */
	switch (crop->width / format->width) {
	case 1:
	default:
		binning = READOUT_NORMAL;
		break;
	case 2:
		binning = READOUT_DIGITAL_BINNING_X2;
		break;
	}
	vd56g3_write(sensor, VD56G3_REG_READOUT_CTRL, binning, &ret);

	/* configure ROIs */
	vd56g3_write(sensor, VD56G3_REG_Y_START, crop->top, &ret);
	vd56g3_write(sensor, VD56G3_REG_Y_END, crop->top + crop->height - 1,
		     &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_X_START, crop->left, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_X_END,
		     crop->left + crop->width - 1, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_Y_START, 0, &ret);
	vd56g3_write(sensor, VD56G3_REG_OUT_ROI_Y_END, crop->height - 1, &ret);
	vd56g3_write(sensor, VD56G3_REG_AE_ROI_START_H, crop->left, &ret);
	vd56g3_write(sensor, VD56G3_REG_AE_ROI_END_H,
		     crop->left + crop->width - 1, &ret);
	vd56g3_write(sensor, VD56G3_REG_AE_ROI_START_V, 0, &ret);
	vd56g3_write(sensor, VD56G3_REG_AE_ROI_END_V, crop->height - 1, &ret);
	if (ret)
		return ret;

	/* Setup default GPIO values; could be overridden by V4L2 ctrl setup */
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
	vd56g3_write(sensor, VD56G3_REG_STBY, VD56G3_CMD_START_STREAM, &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_STBY, VD56G3_CMD_ACK, &ret);
	vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_STREAMING, &ret);

	return ret;
}

static int vd56g3_stream_off(struct vd56g3 *sensor)
{
	int ret;

	/* Retrieve Expo cluster to enable coldstart of AE */
	ret = vd56g3_read_expo_cluster(sensor, true);

	vd56g3_write(sensor, VD56G3_REG_STREAMING, VD56G3_CMD_STOP_STREAM,
		     &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_STREAMING, VD56G3_CMD_ACK, &ret);
	vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY, &ret);

	return ret;
}

static int vd56g3_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
#else
	struct v4l2_subdev_state *state;
#endif
	int ret = 0;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	mutex_lock(&sensor->lock);
#else
	state = v4l2_subdev_lock_and_get_active_state(sd);
#endif

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
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
		ret = vd56g3_stream_on(sensor);
#else
		ret = vd56g3_stream_on(sensor, state);
#endif
		if (ret) {
			dev_err(&client->dev, "Failed to start streaming\n");
			pm_runtime_put_sync(&client->dev);
		}
	} else {
		vd56g3_stream_off(sensor);
		pm_runtime_mark_last_busy(&client->dev);
#if KERNEL_VERSION(6, 9, 0) > LINUX_VERSION_CODE
		pm_runtime_put_autosuspend(&client->dev);
#else
		__pm_runtime_put_autosuspend(&client->dev);
#endif
	}

#if KERNEL_VERSION(4, 20, 0) > LINUX_VERSION_CODE
	if (!ret)
		sensor->streaming = enable;

unlock:
	mutex_unlock(&sensor->lock);

	if (!ret) {
		/* some controls are locked during streaming */
		v4l2_ctrl_grab(sensor->hflip_ctrl, enable);
		v4l2_ctrl_grab(sensor->vflip_ctrl, enable);
		v4l2_ctrl_grab(sensor->patgen_ctrl, enable);
		if (sensor->ext_vt_sync)
			v4l2_ctrl_grab(sensor->slave_ctrl, enable);
	}
#else
	if (!ret) {
		sensor->streaming = enable;

		/* some controls are locked during streaming */
		__v4l2_ctrl_grab(sensor->hflip_ctrl, enable);
		__v4l2_ctrl_grab(sensor->vflip_ctrl, enable);
		__v4l2_ctrl_grab(sensor->patgen_ctrl, enable);
		if (sensor->ext_vt_sync)
			__v4l2_ctrl_grab(sensor->slave_ctrl, enable);
	}

unlock:
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	mutex_unlock(&sensor->lock);
#else
	v4l2_subdev_unlock_state(state);
#endif
#endif

	return ret;
}

static const struct v4l2_subdev_video_ops vd56g3_video_ops = {
	.s_stream = vd56g3_s_stream,
};

/* -----------------------------------------------------------------------------
 * Pad ops
 */

/* Media bus code is dependent of :
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

	if (sensor->is_mono)
		j = 0;
	else
		j = 1 + (sensor->hflip_ctrl->val ? 1 : 0) +
		    (sensor->vflip_ctrl->val ? 2 : 0);

	return vd56g3_mbus_codes[i_bpp][j];
}

#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
static int vd56g3_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
#else
static int vd56g3_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
#endif
{
	struct vd56g3 *sensor = to_vd56g3(sd);

	if (code->index >= ARRAY_SIZE(vd56g3_mbus_codes))
		return -EINVAL;

	code->code =
		vd56g3_get_mbus_code(sensor, vd56g3_mbus_codes[code->index][0]);

	return 0;
}

#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
static int vd56g3_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
#else
static int vd56g3_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
#endif
{
	if (fse->index >= ARRAY_SIZE(vd56g3_supported_modes))
		return -EINVAL;

	fse->min_width = vd56g3_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = vd56g3_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void vd56g3_update_img_pad_format(struct vd56g3 *sensor,
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
	mbus_fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	mbus_fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
static int vd56g3_get_pad_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *sd_fmt)
#else
static int vd56g3_get_pad_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *sd_fmt)
#endif
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	struct v4l2_mbus_framefmt *pad_fmt;

	mutex_lock(&sensor->lock);

	if (sd_fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
		pad_fmt = v4l2_subdev_get_try_format(&sensor->sd, cfg,
						     sd_fmt->pad);
#else
		pad_fmt = v4l2_subdev_get_try_format(&sensor->sd, sd_state,
						     sd_fmt->pad);
#endif
		/* Image mbus code could change with H/V flips */
		pad_fmt->code = vd56g3_get_mbus_code(sensor, pad_fmt->code);
		sd_fmt->format = *pad_fmt;
	} else {
		sd_fmt->format = sensor->active_fmt;
	}

	mutex_unlock(&sensor->lock);

	return 0;
}
#endif

#if KERNEL_VERSION(4, 17, 0) > LINUX_VERSION_CODE
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
#endif

#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
static int vd56g3_set_pad_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *sd_fmt)
#else
static int vd56g3_set_pad_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *sd_fmt)
#endif
{
	struct vd56g3 *sensor = to_vd56g3(sd);
	const struct vd56g3_mode *new_mode;
	struct v4l2_mbus_framefmt *pad_fmt;
	struct v4l2_rect pad_crop;
	unsigned int binning;
	int ret = 0;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	mutex_lock(&sensor->lock);
#endif

	/* Identify the mode that best suits the requested resolution */
#if KERNEL_VERSION(4, 17, 0) > LINUX_VERSION_CODE
	new_mode = vd56g3_find_nearest_size(&sd_fmt->format);
#else
	new_mode = v4l2_find_nearest_size(vd56g3_supported_modes,
					  ARRAY_SIZE(vd56g3_supported_modes),
					  width, height, sd_fmt->format.width,
					  sd_fmt->format.height);
#endif
	/* Update fmt struct with identified resolution and mbus code */
	vd56g3_update_img_pad_format(sensor, new_mode, sd_fmt->format.code,
				     &sd_fmt->format);

	/* Compute crop rectangle (maximized via binning) */
	binning = min(VD56G3_NATIVE_WIDTH / sd_fmt->format.width,
		      VD56G3_NATIVE_HEIGHT / sd_fmt->format.height);
	binning = min(binning, 2U);
	pad_crop.width = sd_fmt->format.width * binning;
	pad_crop.height = sd_fmt->format.height * binning;
	pad_crop.left = (VD56G3_NATIVE_WIDTH - pad_crop.width) / 2;
	pad_crop.top = (VD56G3_NATIVE_HEIGHT - pad_crop.height) / 2;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	if (sd_fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
		pad_fmt = v4l2_subdev_get_try_format(sd, cfg, sd_fmt->pad);
#else
		pad_fmt = v4l2_subdev_get_try_format(sd, sd_state, sd_fmt->pad);
#endif
		*pad_fmt = sd_fmt->format;
	} else if (sd_fmt->format.width != sensor->active_fmt.width ||
		   sd_fmt->format.height != sensor->active_fmt.height ||
		   sd_fmt->format.code != sensor->active_fmt.code) {
		/*
		 * This nested 'if' only avoid to reset ctrls while format
		 * hasn't changed (userspace pb, we shouldn't interfere ?)
		 */
		ret = vd56g3_update_controls(sensor);
		if (!ret) {
			sensor->active_fmt = sd_fmt->format;
			sensor->active_crop = pad_crop;
		}
	}
#else
#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
	pad_fmt = v4l2_subdev_get_pad_format(sd, sd_state, sd_fmt->pad);
#else
	pad_fmt = v4l2_subdev_state_get_format(sd_state, sd_fmt->pad);
#endif

	/* Update active state's format and crop */
	if (sd_fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ret = vd56g3_update_controls(sensor);

	if (!ret) {
		*pad_fmt = sd_fmt->format;
#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
		*v4l2_subdev_get_pad_crop(sd, sd_state, sd_fmt->pad) = pad_crop;
#else
		*v4l2_subdev_state_get_crop(sd_state, sd_fmt->pad) = pad_crop;
#endif
	}
#endif

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	mutex_unlock(&sensor->lock);
#endif

	return ret;
}

#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
static int vd56g3_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
#else
static int vd56g3_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
#endif
{
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	struct vd56g3 *sensor = to_vd56g3(sd);
#endif

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
		sel->r = sensor->active_crop;
#elif KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
		sel->r = *v4l2_subdev_get_pad_crop(sd, sd_state, 0);
#else
		sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
#endif
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
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
static int vd56g3_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg)
#elif KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
static int vd56g3_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state)
#else
static int vd56g3_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
#endif
{
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	struct vd56g3 *sensor = to_vd56g3(sd);
	unsigned int def_mode = VD56G3_DEFAULT_MODE;
#if KERNEL_VERSION(5, 14, 0) > LINUX_VERSION_CODE
	struct v4l2_mbus_framefmt *img_pad_fmt =
		v4l2_subdev_get_try_format(sd, cfg, 0);
#else
	struct v4l2_mbus_framefmt *img_pad_fmt =
		v4l2_subdev_get_try_format(sd, sd_state, 0);
#endif
	/* Default resolution mode / raw8 */
	vd56g3_update_img_pad_format(sensor, &vd56g3_supported_modes[def_mode],
				     vd56g3_mbus_codes[0][0], img_pad_fmt);

	return 0;
#else
	/* Default resolution mode / raw8 */
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = vd56g3_mbus_codes[0][0],
			.width = vd56g3_supported_modes[1].width,
			.height = vd56g3_supported_modes[1].height,
		},
	};

	return vd56g3_set_pad_fmt(sd, sd_state, &fmt);
#endif
}
#endif

static const struct v4l2_subdev_core_ops vd56g3_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_pad_ops vd56g3_pad_ops = {
#if KERNEL_VERSION(4, 7, 0) > LINUX_VERSION_CODE
#elif KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
	.init_cfg = vd56g3_init_cfg,
#else
#endif
	.enum_mbus_code = vd56g3_enum_mbus_code,
	.enum_frame_size = vd56g3_enum_frame_size,
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	.get_fmt = vd56g3_get_pad_fmt,
#else
	.get_fmt = v4l2_subdev_get_fmt,
#endif
	.set_fmt = vd56g3_set_pad_fmt,
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

#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
#else
static const struct v4l2_subdev_internal_ops vd56g3_internal_ops = {
	.init_state = vd56g3_init_state,
};
#endif
/* -----------------------------------------------------------------------------
 * Boot section (includes FMW and VT Patch)
 */

static int vd56g3_patch(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	const u8 *patch = patch_cut2;
	int patch_size = sizeof(patch_cut2);
	u8 patch_major;
	u8 patch_minor;
	int cur_patch_rev = 0;
	int ret = 0;

	patch_major = patch[3];
	patch_minor = patch[2];

	vd56g3_write_array(sensor, 0x2000, patch_size, patch, &ret);
	vd56g3_write(sensor, VD56G3_REG_BOOT, VD56G3_CMD_PATCH_SETUP, &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_BOOT, VD56G3_CMD_ACK, &ret);
	vd56g3_read(sensor, VD56G3_REG_FWPATCH_REVISION, &cur_patch_rev, &ret);
	if (ret)
		return ret;

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
	int ret = 0;

	vd56g3_write(sensor, VD56G3_REG_BOOT, VD56G3_CMD_BOOT, &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_BOOT, VD56G3_CMD_ACK, &ret);
	vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY, &ret);

	return ret;
}

static int vd56g3_vtpatch(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int i;
	int vtpatch_offset = 0;
	int cur_vtpatch_rev = 0;
	int ret = 0;

	vd56g3_write(sensor, VD56G3_REG_VTPATCHING,
		     VD56G3_CMD_START_VTRAM_UPDATE, &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_VTPATCHING, VD56G3_CMD_ACK, &ret);
	vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY, &ret);

	for (i = 0; i < vtpatch_area_nb; i++) {
		vd56g3_write_array(sensor, vtpatch_desc[i].offset,
				   vtpatch_desc[i].size,
				   vtpatch + vtpatch_offset, &ret);

		vtpatch_offset += vtpatch_desc[i].size;
	}

	vd56g3_write(sensor, VD56G3_REG_VTPATCHING, VD56G3_CMD_END_VTRAM_UPDATE,
		     &ret);
	vd56g3_poll_reg(sensor, VD56G3_REG_VTPATCHING, VD56G3_CMD_ACK, &ret);
	vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_SW_STBY, &ret);
	vd56g3_read(sensor, VD56G3_REG_VTPATCH_ID, &cur_vtpatch_rev, &ret);
	if (ret)
		return ret;

	if (cur_vtpatch_rev != VT_REVISION) {
		dev_err(&client->dev, "bad vtpatch version, expected %d got %d",
			VT_REVISION, cur_vtpatch_rev);
		return -ENODEV;
	}
	dev_info(&client->dev, "VT patch %d applied", VT_REVISION);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Power management
 */

static int vd56g3_power_on(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(sensor->supplies),
				    sensor->supplies);
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
	usleep_range(3500, 4000);
	ret = vd56g3_wait_state(sensor, VD56G3_SYSTEM_FSM_READY_TO_BOOT, NULL);
	if (ret) {
		dev_err(&client->dev, "Sensor reset failed %d\n", ret);
		goto disable_clock;
	}

	return 0;

disable_clock:
	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
disable_reg:
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);

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

	if (!sensor->is_fastboot) {
		ret = vd56g3_patch(sensor);
		if (ret) {
			dev_err(&client->dev, "sensor patch failed %d", ret);
			return ret;
		}
	}

	ret = vd56g3_boot(sensor);
	if (ret) {
		dev_err(&client->dev, "sensor boot failed %d", ret);
		return ret;
	}

	if (!sensor->is_fastboot) {
		ret = vd56g3_vtpatch(sensor);
		if (ret) {
			dev_err(&client->dev, "sensor VT patch failed %d", ret);
			return ret;
		}
	}

	return 0;
}

static int vd56g3_power_off(struct vd56g3 *sensor)
{
	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);

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

static const struct dev_pm_ops vd56g3_pm_ops = {
	SET_RUNTIME_PM_OPS(vd56g3_runtime_suspend, vd56g3_runtime_resume, NULL)
};

/* -----------------------------------------------------------------------------
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
	u8 n_lanes;
	u64 frequency;
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

	/* Check lanes number */
	n_lanes = ep.bus.mipi_csi2.num_data_lanes;
	if (n_lanes != 1 && n_lanes != 2) {
		dev_err(&client->dev, "Invalid data lane number %d\n", n_lanes);
		ret = -EINVAL;
		goto done;
	}
	sensor->nb_of_lane = n_lanes;

	/* Clock lane must be first */
	if (ep.bus.mipi_csi2.clock_lane != 0) {
		dev_err(&client->dev, "Clk lane must be mapped to lane 0\n");
		ret = -EINVAL;
		goto done;
	}

	/*
	 * Prepare Output Interface conf based on lane settings
	 * logical to physical lane conversion (+ pad remaining slots)
	 */
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

	/* Check link frequency */
	if (!ep.nr_of_link_frequencies) {
		dev_err(&client->dev, "link-frequency not found in DT\n");
		ret = -EINVAL;
		goto done;
	}
	frequency = (n_lanes == 2) ? VD56G3_LINK_FREQ_DEF_2LANES :
				     VD56G3_LINK_FREQ_DEF_1LANE;
	if (ep.nr_of_link_frequencies != 1 ||
	    ep.link_frequencies[0] != frequency) {
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
	unsigned int i;
	int ret;

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
	endpoint =
		fwnode_graph_get_next_endpoint(of_fwnode_handle(dev->of_node),
					       NULL);
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

	return vd56g3_parse_dt_gpios(sensor);
}

static int vd56g3_get_regulators(struct vd56g3 *sensor)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++)
		sensor->supplies[i].supply = vd56g3_supply_names[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       ARRAY_SIZE(sensor->supplies),
				       sensor->supplies);
}

static int vd56g3_prepare_clock_tree(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	const unsigned int predivs[] = { 1, 2, 4 };
	u32 pll_out;
	int i;

	/* External clock must be in [6Mhz-27Mhz] */
	if (sensor->xclk_freq < 6 * HZ_PER_MHZ ||
	    sensor->xclk_freq > 27 * HZ_PER_MHZ) {
		dev_err(&client->dev,
			"Only 6Mhz-27Mhz clock range supported. Provided %lu MHz\n",
			sensor->xclk_freq / HZ_PER_MHZ);
		return -EINVAL;
	}

	/* PLL input should be in [6Mhz-12Mhz[ */
	for (i = 0; i < ARRAY_SIZE(predivs); i++) {
		sensor->pll_prediv = predivs[i];
		if (sensor->xclk_freq / sensor->pll_prediv < 12 * HZ_PER_MHZ)
			break;
	}

	/* PLL output clock must be as close as possible to 804Mhz */
	sensor->pll_mult = (VD56G3_TARGET_PLL * sensor->pll_prediv +
			    sensor->xclk_freq / 2) /
			   sensor->xclk_freq;
	pll_out = sensor->xclk_freq * sensor->pll_mult / sensor->pll_prediv;

	/* Target Pixel Clock for standard 10bit ADC mode : 160.8Mhz */
	sensor->pixel_clock = pll_out / VD56G3_VT_CLOCK_DIV;

	return 0;
}

#if KERNEL_VERSION(4, 16, 0) > LINUX_VERSION_CODE
static const struct of_device_id vd56g3_dt_ids[] = {
	{ .compatible = "st,vd56g3", .data = (void *)VD56G3_MODEL_VD56G3 },
	{ .compatible = "st,vd66gy", .data = (void *)VD56G3_MODEL_VD66GY },
	{ /* sentinel */ }
};
#endif

static int vd56g3_detect(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;
	unsigned int model;
	int model_id = 0;
	int device_revision = 0;
	int optical_revision = 0;
	int ret = 0;

#if KERNEL_VERSION(4, 16, 0) > LINUX_VERSION_CODE
	const struct of_device_id *dt_ids;

	dt_ids = of_match_device(of_match_ptr(vd56g3_dt_ids), dev);
	if (dt_ids)
		model = (uintptr_t)dt_ids->data;
#else
	model = (uintptr_t)device_get_match_data(dev);
#endif

	ret = vd56g3_read(sensor, VD56G3_REG_MODEL_ID, &model_id, NULL);
	if (ret)
		return ret;

	if (model_id != VD56G3_MODEL_ID) {
		dev_err(&client->dev, "Unsupported sensor id %x", model_id);
		return -ENODEV;
	}

	ret = vd56g3_read(sensor, VD56G3_REG_REVISION, &device_revision, NULL);
	if (ret)
		return ret;

	if ((device_revision >> 8) == VD56G3_REVISION_CUT2) {
		sensor->is_fastboot = false;
	} else if ((device_revision >> 8) == VD56G3_REVISION_CUT3) {
		sensor->is_fastboot = true;
	} else {
		dev_err(&client->dev, "Unsupported Cut version %x",
			device_revision);
		return -ENODEV;
	}

	ret = vd56g3_read(sensor, VD56G3_REG_OPTICAL_REVISION,
			  &optical_revision, NULL);
	if (ret)
		return ret;

	sensor->is_mono =
		((optical_revision & 1) == VD56G3_OPTICAL_REVISION_MONO);
	if ((sensor->is_mono && model == VD56G3_MODEL_VD66GY) ||
	    (!sensor->is_mono && model == VD56G3_MODEL_VD56G3)) {
		dev_err(&client->dev,
			"Found %s sensor, while %s model is defined in DT",
			(sensor->is_mono) ? "Mono" : "Bayer",
			(model == VD56G3_MODEL_VD56G3) ? "vd56g3" : "vd66gy");
		return -ENODEV;
	}

	return 0;
}

static int vd56g3_subdev_init(struct vd56g3 *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	unsigned int def_mode = VD56G3_DEFAULT_MODE;
#endif
	int ret;

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	mutex_init(&sensor->lock);
#endif

	/* Init sub device */
	v4l2_i2c_subdev_init(&sensor->sd, client, &vd56g3_subdev_ops);
#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
#else
	sensor->sd.internal_ops = &vd56g3_internal_ops;
#endif
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->sd.entity.ops = &vd56g3_subdev_entity_ops;

	/* Init source pad */
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
#if KERNEL_VERSION(4, 5, 0) > LINUX_VERSION_CODE
	sensor->sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	ret = media_entity_init(&sensor->sd.entity, 1, &sensor->pad, 0);
#else
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
#endif
	if (ret) {
		dev_err(&client->dev, "Failed to init media entity : %d", ret);
		return ret;
	}

	/* Init controls */
	ret = vd56g3_init_controls(sensor);
	if (ret) {
		dev_err(&client->dev, "Controls initialization failed %d", ret);
		goto err_media;
	}

	/* Init vd56g3 struct : default resolution + raw8 */
	sensor->streaming = false;
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	vd56g3_update_img_pad_format(sensor, &vd56g3_supported_modes[def_mode],
				     vd56g3_mbus_codes[0][0],
				     &sensor->active_fmt);
	sensor->active_crop.width = vd56g3_supported_modes[def_mode].width;
	sensor->active_crop.height = vd56g3_supported_modes[def_mode].height;
	sensor->active_crop.left = 2;
	sensor->active_crop.top = 2;
#else
	sensor->sd.state_lock = sensor->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret) {
		dev_err(&client->dev, "subdev init error: %d", ret);
		goto err_ctrls;
	}
#endif

	return vd56g3_update_controls(sensor);

#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
#else
err_ctrls:
	v4l2_ctrl_handler_free(sensor->sd.ctrl_handler);
#endif

err_media:
	media_entity_cleanup(&sensor->sd.entity);

	return ret;
}

static void vd56g3_subdev_cleanup(struct vd56g3 *sensor)
{
	v4l2_async_unregister_subdev(&sensor->sd);
#if KERNEL_VERSION(5, 19, 0) > LINUX_VERSION_CODE
	mutex_destroy(&sensor->lock);
#else
	v4l2_subdev_cleanup(&sensor->sd);
#endif
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(sensor->sd.ctrl_handler);
}

static int vd56g3_err_probe(struct device *dev, int ret, char *msg)
{
#if KERNEL_VERSION(5, 9, 0) > LINUX_VERSION_CODE
	dev_err(dev, "%s", msg);
	return ret;
#else
	return dev_err_probe(dev, ret, msg);
#endif
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
	if (ret)
		return vd56g3_err_probe(dev, ret,
					"Failed to parse Device Tree.");

	/* Get (and check) resources : power regs, ext clock, reset gpio */
	ret = vd56g3_get_regulators(sensor);
	if (ret)
		return vd56g3_err_probe(dev, ret, "Failed to get regulators.");

	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return vd56g3_err_probe(dev, PTR_ERR(sensor->xclk),
					"Failed to get xclk.");
	sensor->xclk_freq = clk_get_rate(sensor->xclk);
	ret = vd56g3_prepare_clock_tree(sensor);
	if (ret)
		return ret;

	sensor->reset_gpio =
		devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return vd56g3_err_probe(dev, PTR_ERR(sensor->reset_gpio),
					"Failed to get reset gpio.");

#if KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE
	sensor->regmap = devm_regmap_init_i2c(client, &vd56g3_regmap_config);
#else
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
#endif
	if (IS_ERR(sensor->regmap))
		return vd56g3_err_probe(dev, PTR_ERR(sensor->regmap),
					"Failed to init regmap.");

	/* Power ON */
	ret = vd56g3_power_on(sensor);
	if (ret)
		return vd56g3_err_probe(dev, ret, "Sensor power on failed.");

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

	/* Initialize, then register V4L2 subdev */
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
	if (sensor->is_fastboot)
		pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_mark_last_busy(dev);
#if KERNEL_VERSION(6, 9, 0) > LINUX_VERSION_CODE
	pm_runtime_put_autosuspend(dev);
#else
	__pm_runtime_put_autosuspend(dev);
#endif

	dev_info(&client->dev, "Successfully probe %s sensor",
		 (sensor->is_mono) ? "vd56g3" : "vd66gy");

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

#if KERNEL_VERSION(4, 16, 0) <= LINUX_VERSION_CODE
static const struct of_device_id vd56g3_dt_ids[] = {
	{ .compatible = "st,vd56g3", .data = (void *)VD56G3_MODEL_VD56G3 },
	{ .compatible = "st,vd66gy", .data = (void *)VD56G3_MODEL_VD66GY },
	{ /* sentinel */ }
};
#endif
MODULE_DEVICE_TABLE(of, vd56g3_dt_ids);

static struct i2c_driver vd56g3_i2c_driver = {
	.driver = {
		.name  = "vd56g3",
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

MODULE_AUTHOR("Benjamin Mugnier <benjamin.mugnier@foss.st.com>");
MODULE_AUTHOR("Mickael Guene <mickael.guene@st.com>");
MODULE_AUTHOR("Sylvain Petinot <sylvain.petinot@foss.st.com>");
MODULE_DESCRIPTION("ST VD56G3 sensor driver");
MODULE_LICENSE("GPL");
