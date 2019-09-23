// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for VD56G3 global shutter sensor
 *
 * Copyright (C) STMicroelectronics SA 2019
 * Authors: Mickael Guene <mickael.guene@st.com>
 *          for STMicroelectronics.
 *
 */

#include <linux/i2c.h>
#include <linux/module.h>

static int vd56g3_probe(struct i2c_client *client)
{
	dev_info(&client->dev, "vd56g3 probe successfully");

	return 0;
}

static int vd56g3_remove(struct i2c_client *client)
{
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