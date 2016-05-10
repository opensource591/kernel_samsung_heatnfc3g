/*
 * OmniVision OV5640 sensor driver
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <uapi/linux/time.h>
#include <uapi/linux/videodev2.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/delay.h>
#include <linux/module.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-chip-ident.h>
#include <media/soc_camera.h>
#include <linux/videodev2_brcm.h>
#include "ov5640.h"

#ifdef CONFIG_VIDEO_ADP1653
#include "adp1653.h"
#endif

#ifdef CONFIG_VIDEO_AS3643
#include "as3643.h"
#endif

/* #define OV5640_DEBUG */

#define iprintk(format, arg...)	\
	printk(KERN_INFO"[%s]: "format"\n", __func__, ##arg)


#define OV5640_FLASH_THRESHHOLD		32
#define OV5640_HFLIP_MASK		0x06
#define OV5640_VFLIP_MASK		0x06
#define OV5640_BINNING_MASK		0x01
#define OV5640_TIMING_REG20		0x3820
#define OV5640_TIMING_REG21		0x3821
#define OV5640_NUM_OF_STD_CTRLS		15	/* no.of std + std_menu ctrls */

/* OV5640 has only one fixed colorspace per pixelcode */
struct ov5640_datafmt {
	enum v4l2_mbus_pixelcode code;
	enum v4l2_colorspace colorspace;
};

struct ov5640_timing_cfg {
	u16 x_addr_start;
	u16 y_addr_start;
	u16 x_addr_end;
	u16 y_addr_end;
	u16 h_output_size;
	u16 v_output_size;
	u16 h_total_size;
	u16 v_total_size;
	u16 isp_h_offset;
	u16 isp_v_offset;
	u8 h_odd_ss_inc;
	u8 h_even_ss_inc;
	u8 v_odd_ss_inc;
	u8 v_even_ss_inc;
	u8 out_mode_sel;
	u8 sclk_dividers;
	u8 sys_mipi_clk;

};

static const struct ov5640_datafmt ov5640_fmts[] = {
	/*
	 * Order important: first natively supported,
	 * second supported with a GPIO extender
	 */
	{V4L2_MBUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_JPEG},
	{V4L2_MBUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_JPEG},
/*	{V4L2_MBUS_FMT_JPEG_1X8, V4L2_COLORSPACE_JPEG}, */

};

enum ov5640_size {
	OV5640_SIZE_QVGA,	/* 320 x 240 */
	OV5640_SIZE_VGA,	/* 640 x 480 */
	OV5640_SIZE_720P,
	OV5640_SIZE_1280x960,	/* 1280 x 960  (1.2M) */
	OV5640_SIZE_UXGA,	/* 1600 x 1200 (2M)   */
	OV5640_SIZE_QXGA,	/* 2048 x 1536 (3M)   */
	OV5640_SIZE_5MP,
	OV5640_SIZE_LAST,
	OV5640_SIZE_MAX
};

enum cam_running_mode {
	CAM_RUNNING_MODE_NOTREADY,
	CAM_RUNNING_MODE_PREVIEW,
	CAM_RUNNING_MODE_CAPTURE,
	CAM_RUNNING_MODE_CAPTURE_DONE,
	CAM_RUNNING_MODE_RECORDING,
};
enum cam_running_mode runmode;

static const struct v4l2_frmsize_discrete ov5640_frmsizes[OV5640_SIZE_LAST] = {
	{320, 240},
	{640, 480},
	{1280, 720},
	{1280, 960},
	{1600, 1200},
	{2048, 1536},
	{2560, 1920},
};

/* Scalers to map image resolutions into AF 80x60 virtual viewfinder */
static const struct ov5640_af_zone_scale af_zone_scale[OV5640_SIZE_LAST] = {
	{4, 4},
	{8, 8},
	{16, 12},
	{16, 16},
	{20, 15},
	{26, 26},
	{32, 32},
};

/* Find a data format by a pixel code in an array */
static int ov5640_find_datafmt(enum v4l2_mbus_pixelcode code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ov5640_fmts); i++)
		if (ov5640_fmts[i].code == code)
			break;

	/* If not found, select latest */
	if (i >= ARRAY_SIZE(ov5640_fmts))
		i = ARRAY_SIZE(ov5640_fmts) - 1;

	return i;
}

/* Find a frame size in an array */
static int ov5640_find_framesize(u32 width, u32 height)
{
	int i;

	for (i = 0; i < OV5640_SIZE_LAST; i++) {
		if ((ov5640_frmsizes[i].width >= width) &&
		    (ov5640_frmsizes[i].height >= height))
			break;
	}

	/* If not found, select biggest */
	if (i >= OV5640_SIZE_LAST)
		i = OV5640_SIZE_LAST - 1;

	return i;
}

struct ov5640 {
	struct v4l2_subdev subdev;
	struct v4l2_subdev_sensor_interface_parms *plat_parms;
	struct v4l2_ctrl_handler hdl;
	int i_size;
	int i_fmt;
	int brightness;
	int contrast;
	int sharpness;
	int saturation;
	int hflip;
	int vflip;
	int framerate;
	enum v4l2_colorfx			colorlevel;
	enum v4l2_power_line_frequency		antibanding;
	enum v4l2_auto_n_preset_white_balance	whitebalance;
	enum v4l2_auto_focus_range		focus_mode;
	enum v4l2_flash_led_mode		flashmode;
	/*
	 * focus_status = 1 focusing
	 * focus_status = 0 focus cancelled or not focusing
	 */
	atomic_t focus_status;

	/*
	 * touch_focus holds number of valid touch focus areas. 0 = none
	 */
	int touch_focus;
	v4l2_touch_area touch_area[OV5640_MAX_FOCUS_AREAS];
	short fireflash;
};

static int ov5640_set_flash_mode(struct v4l2_ctrl *ctrl,
				struct i2c_client *client);

static struct ov5640 *to_ov5640(const struct i2c_client *client)
{

	return container_of(i2c_get_clientdata(client), struct ov5640, subdev);
}

static const struct ov5640_timing_cfg timing_cfg_yuv[OV5640_SIZE_LAST] = {
	[OV5640_SIZE_QVGA] = {
		/* Timing control  2624 x 1952 --> 2592 x 1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 320,
		.v_output_size	= 240,
		/* ISP Windowing size 1296 x 972 --> 1280 x 960 */
		.isp_h_offset	= 8,
		.isp_v_offset	= 6,
		/* Total size (+blanking) */
		.h_total_size	= 2200,
		.v_total_size	= 1280,
		/* Sensor Read Binning Enabled */
		.h_odd_ss_inc	= 3,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 3,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x01,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x11,
	},
	[OV5640_SIZE_VGA] = {
		/* Timing control  2624 x 1952 --> 2592 x 1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 640,
		.v_output_size	= 480,
		/* ISP Windowing size  1296 x 972 --> 1280 x 960 */
		.isp_h_offset	= 8,
		.isp_v_offset	= 6,
		/* Total size (+blanking) */
		.h_total_size	= 2200,
		.v_total_size	= 1280,
		/* Sensor Read Binning Enabled */
		.h_odd_ss_inc	= 3,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 3,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x01,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x11,
	},
	[OV5640_SIZE_720P] = {
		/* Timing control  2624 x 1952 --> 2592 x 1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 1280,
		.v_output_size	= 720,
		/* ISP Windowing size 1296 x 972 --> 1280 x 960 */
		.isp_h_offset	= 8,
		.isp_v_offset	= 6,
		/* Total size (+blanking) */
		.h_total_size	= 2200,
		.v_total_size	= 1280,
		/* Sensor Read Binning Enabled */
		.h_odd_ss_inc	= 3,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 3,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x01,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x11,
	},
	[OV5640_SIZE_1280x960] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 1280,
		.v_output_size	= 960,
		/* ISP Windowing size 1296x972 --> 1280x960 */
		.isp_h_offset	= 8,
		.isp_v_offset	= 6,
		/* Total size (+blanking) */
		.h_total_size	= 2200,
		.v_total_size	= 1280,
		/* Sensor Read Binning Enabled */
		.h_odd_ss_inc	= 3,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 3,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x01,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x11,
	},
	[OV5640_SIZE_UXGA] = {
		/* Timing control  2624 x 1952 --> 2592 x 1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 1600,
		.v_output_size	= 1200,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x00,
		.sclk_dividers	= 0x02,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_QXGA] = {
		/* Timing control  2624 x 1952 --> 2592 x 1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 2048,
		.v_output_size	= 1536,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Enabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x00,
		.sclk_dividers	= 0x02,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_5MP] = {
		/* Timing control  2624 x 1952 --> 2592 x 1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 2560,
		.v_output_size	= 1920,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Enabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x00,
		.sclk_dividers	= 0x02,
		.sys_mipi_clk	= 0x12,
	},
};

static const struct ov5640_timing_cfg timing_cfg_jpeg[OV5640_SIZE_LAST] = {
	[OV5640_SIZE_QVGA] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 320,
		.v_output_size	= 240,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_VGA] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 640,
		.v_output_size	= 480,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_720P] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 1280,
		.v_output_size	= 720,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_1280x960] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 1280,
		.v_output_size	= 960,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_UXGA] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 1600,
		.v_output_size	= 1200,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_QXGA] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 2048,
		.v_output_size	= 1536,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
	[OV5640_SIZE_5MP] = {
		/* Timing control 2624x1952 --> 2592x1944 */
		.x_addr_start	= 16,
		.y_addr_start	= 4,
		.x_addr_end	= 2607,
		.y_addr_end	= 1947,
		/* Output image size */
		.h_output_size	= 2560,
		.v_output_size	= 1920,
		/* ISP Windowing size 2592x1944 --> 2560x1920 */
		.isp_h_offset	= 16,
		.isp_v_offset	= 12,
		/* Total size (+blanking) */
		.h_total_size	= 2844,
		.v_total_size	= 1968,
		/* Sensor Read Binning Disabled */
		.h_odd_ss_inc	= 1,
		.h_even_ss_inc	= 1,
		.v_odd_ss_inc	= 1,
		.v_even_ss_inc	= 1,
		.out_mode_sel	= 0x20,
		.sclk_dividers	= 0x01,
		.sys_mipi_clk	= 0x12,
	},
};


static int ov5640_config_timing(struct i2c_client *client);

/**
 *ov5640_reg_read - Read a value from a register in an ov5640 sensor device
 *@client: i2c driver client structure
 *@reg: register address / offset
 *@val: stores the value that gets read
 *
 * Read a value from a register in an ov5640 sensor device.
 * The value is returned in 'val'.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov5640_reg_read(struct i2c_client *client, u16 reg, u8 *val)
{

	int ret;
	u8 data[2] = { 0 };
	struct i2c_msg msg = {
		.addr	= client->addr,
		.flags	= 0,
		.len	= 2,
		.buf	= data,
	};

	data[0] = (u8) (reg >> 8);
	data[1] = (u8) (reg & 0xff);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		goto err;

	msg.flags = I2C_M_RD;
	msg.len	  = 1;
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		goto err;

	*val = data[0];
	return 0;

err:
	dev_err(&client->dev, "Failed reading register 0x%02x!\n", reg);
	return ret;
}

/**
 * Write a value to a register in ov5640 sensor device.
 *@client: i2c driver client structure.
 *@reg: Address of the register to read value from.
 *@val: Value to be written to a specific register.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov5640_reg_write(struct i2c_client *client, u16 reg, u8 val)
{

	int ret;
	unsigned char data[3] = { (u8) (reg >> 8), (u8) (reg & 0xff), val };
	struct i2c_msg msg = {
		.addr	= client->addr,
		.flags	= 0,
		.len	= 3,
		.buf	= data,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "Failed writing register 0x%02x!\n", reg);
		return ret;
	}

	return 0;
}

static int ov5640_s_ctrl(struct v4l2_ctrl *ctrl);
static int ov5640_g_volatile_ctrl(struct v4l2_ctrl *ctrl);

static const struct v4l2_ctrl_ops ov5640_ctrl_ops = {
	.g_volatile_ctrl	= ov5640_g_volatile_ctrl,
	.s_ctrl			= ov5640_s_ctrl,
};

static const struct v4l2_ctrl_config ov5640_controls[] = {
	{
		.ops	= &ov5640_ctrl_ops,
		.id	= V4L2_CID_CAM_PREVIEW_ONOFF,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Preview_on_off",
		.min	= 0,
		.max	= 1,
		.step	= 1,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_VOLATILE,
	},
	{
		.ops	= &ov5640_ctrl_ops,
		.id	= V4L2_CID_CAMERA_FRAME_RATE,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Framerate control",
		.min	= FRAME_RATE_AUTO,
		.max	= (1 << FRAME_RATE_AUTO | 1 << FRAME_RATE_5 |
				1 << FRAME_RATE_10 | 1 << FRAME_RATE_15 |
				1 << FRAME_RATE_25 | 1 << FRAME_RATE_30),
		.step	= 1,
		.def	= FRAME_RATE_AUTO,
		.flags	= V4L2_CTRL_FLAG_VOLATILE,
	},
/*	{
		.ops	= &ov5640_ctrl_ops,
		.id	= V4L2_CID_CAMERA_TOUCH_AF_AREA,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Touch focus areas",
		.min	= 0,
		.max	= OV5640_MAX_FOCUS_AREAS,
		.step	= 1,
		.def	= 1,
		.flags	= V4L2_CTRL_FLAG_VOLATILE,
	}, */
	{
		.ops	= &ov5640_ctrl_ops,
		.id	= V4L2_CID_CAM_CAPTURE,
		/* v4l2 control framework does not call s_ctrl if
		 * current control value == previous control value.
		 * If type = V4L2_CTRL_TYPE_BUTTON then s_ctrl is called always.
		 *
		 * For BUTTON controls: min = max = step = def = 0
		 * and flag = write_only.
		 */
		.type	= V4L2_CTRL_TYPE_BUTTON,
		.name	= "Camera capture",
		.min	= 0,
		.max	= 0,
		.step	= 0,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_WRITE_ONLY,
	},
	{
		.ops	= &ov5640_ctrl_ops,
		.id	= V4L2_CID_CAM_CAPTURE_DONE,
		.type	= V4L2_CTRL_TYPE_BUTTON,
		.name	= "Camera capture done",
		.min	= 0,
		.max	= 0,
		.step	= 0,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_WRITE_ONLY,
	},
	{
		.ops	= &ov5640_ctrl_ops,
		.id	= V4L2_CID_CAM_MOUNTING,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Sensor mounting",
		.min	= 0,
		.max	= 0x10 | 0x20 | 0x40, /* (1 << V4L2_IN_ST_HFLIP |
					      1 << V4L2_IN_ST_VFLIP |
					      1 << V4L2_IN_ST_BACK), */
		.step	= 1,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_VOLATILE,
	},
};

/**
 * Initialize a list of ov5640 registers.
 * The list of registers is terminated by the pair of values
 *@client: i2c driver client structure.
 *@reglist[]: List of address of the registers to write data.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov5640_reg_writes(struct i2c_client *client,
			     const struct ov5640_reg reglist[])
{

	int err = 0, index;

	for (index = 0; ((reglist[index].reg != 0xFFFF) && (err == 0)); index++) {
		err |=
		    ov5640_reg_write(client, reglist[index].reg,
				     reglist[index].val);
		/*  Check for Pause condition */
		if ((reglist[index + 1].reg == 0xFFFF)
		    && (reglist[index + 1].val != 0)) {
			msleep(reglist[index + 1].val);
			index += 1;
		}
	}
	return 0;
}

static int ov5640_reglist_compare(struct i2c_client *client,
				  const struct ov5640_reg reglist[])
{
#ifdef OV5640_DEBUG

	int err = 0, index;
	u8 reg;

	for (index = 0; ((reglist[index].reg != 0xFFFF) && (err == 0)); index++) {
		err |= ov5640_reg_read(client, reglist[index].reg, &reg);
		if (reglist[index].val != reg) {
			iprintk("reg err:reg=0x%x val=0x%x rd=0x%x",
				reglist[index].reg, reglist[index].val, reg);
		}
		/*  Check for Pause condition */
		if ((reglist[index + 1].reg == 0xFFFF)
		    && (reglist[index + 1].val != 0)) {
			msleep(reglist[index + 1].val);
			index += 1;
		}
	}
#endif
	return 0;
}

/**
 * Write an array of data to ov5640 sensor device.
 * @client: i2c driver client structure.
 * @reg: Address of the register to read value from.
 * @data: pointer to data to be written starting at specific register.
 * @size: # of data to be written starting at specific register.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov5640_array_write(struct i2c_client *client,
			      const u8 *data, u16 size)
{
	int ret;
	struct i2c_msg msg = {
		.addr	= client->addr,
		.flags	= 0,
		.len	= size,
		.buf	= (u8 *) data,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "Failed writing array to 0x%02x!\n",
			((data[0] << 8) | (data[1])));
		return ret;
	}

	return 0;
}

static int ov5640_af_ack(struct i2c_client *client, int num_trys)
{
	int ret = 0;
	u8 af_ack = 0;
	int i;

	for (i = 0; i < num_trys; i++) {
		ov5640_reg_read(client, OV5640_CMD_ACK, &af_ack);
		if (af_ack == 0)
			break;
		msleep(50);
	}
	if (af_ack != 0) {
		dev_dbg(&client->dev, "af ack failed\n");
		return OV5640_AF_FAIL;
	}
	return ret;
}

static int ov5640_af_fw_status(struct i2c_client *client)
{
	u8 af_st = 0;

	ov5640_reg_read(client, OV5640_CMD_FW_STATUS, &af_st);

	return (int)af_st;
}

static int ov5640_af_enable(struct i2c_client *client)
{
	int ret = 0;

	ret = ov5640_reg_writes(client, ov5640_afpreinit_tbl);
	if (ret)
		return ret;

	ret = ov5640_array_write(client, ov5640_afinit_data,
				 sizeof(ov5640_afinit_data)
				 / sizeof(ov5640_afinit_data[0]));
	if (ret)
		return ret;

	ret = ov5640_reg_writes(client, ov5640_afpostinit_tbl);
	if (ret)
		return ret;

	ov5640_af_fw_status(client);

	return ret;
}

static int ov5640_af_release(struct i2c_client *client)
{
	int ret = 0;

	ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
	if (ret)
		return ret;
	ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x08);
	if (ret)
		return ret;
	ov5640_af_fw_status(client);
	return ret;
}

static int ov5640_af_center(struct i2c_client *client)
{
	int ret = 0;

	ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
	if (ret)
		return ret;
	ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x80);
	if (ret)
		return ret;
	ret = ov5640_af_ack(client, 50);
	if (ret) {
		dev_dbg(&client->dev, "failed\n");
		return OV5640_AF_FAIL;
	}

	return ret;
}

static int ov5640_af_macro(struct i2c_client *client)
{
	int ret = 0;
	u8 reg;

	ret = ov5640_af_release(client);
	if (ret)
		return ret;
	/* move VCM all way out */
	ret = ov5640_reg_read(client, 0x3603, &reg);
	if (ret)
		return ret;
	reg &= ~(0x3f);
	ret = ov5640_reg_write(client, 0x3603, reg);
	if (ret)
		return ret;

	ret = ov5640_reg_read(client, 0x3602, &reg);
	if (ret)
		return ret;
	reg &= ~(0xf0);
	ret = ov5640_reg_write(client, 0x3602, reg);
	if (ret)
		return ret;

	/* set direct mode */
	ret = ov5640_reg_read(client, 0x3602, &reg);
	if (ret)
		return ret;
	reg &= ~(0x07);
	ret = ov5640_reg_write(client, 0x3602, reg);
	if (ret)
		return ret;

	return ret;
}

static int ov5640_af_infinity(struct i2c_client *client)
{
	int ret = 0;
	u8 reg;

	ret = ov5640_af_release(client);
	if (ret)
		return ret;
	/* move VCM all way in */
	ret = ov5640_reg_read(client, 0x3603, &reg);
	if (ret)
		return ret;
	reg |= 0x3f;
	ret = ov5640_reg_write(client, 0x3603, reg);
	if (ret)
		return ret;

	ret = ov5640_reg_read(client, 0x3602, &reg);
	if (ret)
		return ret;
	reg |= 0xf0;
	ret = ov5640_reg_write(client, 0x3602, reg);
	if (ret)
		return ret;

	/* set direct mode */
	ret = ov5640_reg_read(client, 0x3602, &reg);
	if (ret)
		return ret;
	reg &= ~(0x07);
	ret = ov5640_reg_write(client, 0x3602, reg);
	if (ret)
		return ret;

	return ret;
}

/* Set the touch area x,y in VVF coordinates*/
static int ov5640_af_touch(struct i2c_client *client)
{
	int ret = OV5640_AF_SUCCESS;
	struct ov5640 *ov5640 = to_ov5640(client);

	/* verify # zones correct */
	if (ov5640->touch_focus) {

		/* touch zone config */
		ret = ov5640_reg_write(client, 0x3024,
				       (u8) ov5640->touch_area[0].leftTopX);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x3025,
				       (u8) ov5640->touch_area[0].leftTopY);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x81);
		if (ret)
			return ret;
		ret = ov5640_af_ack(client, 50);
		if (ret) {
			dev_dbg(&client->dev, "zone config ack failed\n");
			return ret;
		}

	}

	return ret;
}

/* Set the touch area, areas can overlap and
 * are givin in current sensor resolution coords
 */
static int ov5640_af_area(struct i2c_client *client)
{
	int ret = OV5640_AF_SUCCESS;
	struct ov5640 *ov5640 = to_ov5640(client);
	u8 weight[OV5640_MAX_FOCUS_AREAS];
	int i;

	/* verify # zones correct */
	if ((ov5640->touch_focus) &&
	    (ov5640->touch_focus <= OV5640_MAX_FOCUS_AREAS)) {

		/* enable zone config */
		ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x8f);
		if (ret)
			return ret;
		ret = ov5640_af_ack(client, 50);
		if (ret) {
			dev_dbg(&client->dev, "zone config ack failed\n");
			return ret;
		}

		/* clear all zones */
		for (i = 0; i < OV5640_MAX_FOCUS_AREAS; i++)
			weight[i] = 0;

		/* write area to sensor */
		for (i = 0; i < ov5640->touch_focus; i++) {

			ret = ov5640_reg_write(client, 0x3024,
					       (u8) ov5640->
					       touch_area[i].leftTopX);
			if (ret)
				return ret;
			ret = ov5640_reg_write(client, 0x3025,
					       (u8) ov5640->
					       touch_area[i].leftTopY);
			if (ret)
				return ret;
			ret = ov5640_reg_write(client, 0x3026,
					       (u8) (ov5640->
						     touch_area[i].leftTopX +
						     ov5640->
						     touch_area
						     [i].rightBottomX));
			if (ret)
				return ret;
			ret = ov5640_reg_write(client, 0x3027,
					       (u8) (ov5640->
						     touch_area[i].leftTopY +
						     ov5640->
						     touch_area
						     [i].rightBottomY));
			if (ret)
				return ret;
			ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
			if (ret)
				return ret;
			ret = ov5640_reg_write(client, OV5640_CMD_MAIN,
					       (0x90 + i));
			if (ret)
				return ret;
			ret = ov5640_af_ack(client, 50);
			if (ret) {
				dev_dbg(&client->dev, "zone update failed\n");
				return ret;
			}
			weight[i] = (u8) ov5640->touch_area[i].weight;
		}

		/* enable zone with weight */
		ret = ov5640_reg_write(client, 0x3024, weight[0]);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x3025, weight[1]);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x3026, weight[2]);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x3027, weight[3]);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x3028, weight[4]);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x98);
		if (ret)
			return ret;
		ret = ov5640_af_ack(client, 50);
		if (ret) {
			dev_dbg(&client->dev, "weights failed\n");
			return ret;
		}

		/* launch zone configuration */
		ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x9f);
		if (ret)
			return ret;
		ret = ov5640_af_ack(client, 50);
		if (ret) {
			dev_dbg(&client->dev, "launch failed\n");
			return ret;
		}
	}

	return ret;
}

/* Convert touch area from sensor resolution coords to ov5640 VVF zone */
static int ov5640_af_zone_conv(struct i2c_client *client,
			       v4l2_touch_area *zone_area, int zone)
{
	int ret = 0;
	u32 x0, y0, x1, y1, weight;
	struct ov5640 *ov5640 = to_ov5640(client);

	/* Reset zone */
	ov5640->touch_area[zone].leftTopX	= 0;
	ov5640->touch_area[zone].leftTopY	= 0;
	ov5640->touch_area[zone].rightBottomX	= 0;
	ov5640->touch_area[zone].rightBottomY	= 0;
	ov5640->touch_area[zone].weight		= 0;

	/* x y w h are in current sensor resolution dimensions */
	if (((u32) zone_area->leftTopX + (u32) zone_area->rightBottomX)
	    > ov5640_frmsizes[ov5640->i_size].width) {
		iprintk("zone width error: x=0x%x w=0x%x",
			zone_area->leftTopX, zone_area->rightBottomX);
		ret = -EINVAL;
		goto out;
	} else if (((u32) zone_area->leftTopY + (u32) zone_area->rightBottomY)
		   > ov5640_frmsizes[ov5640->i_size].height) {
		iprintk("zone height error: y=0x%x h=0x%x",
			zone_area->leftTopY, zone_area->rightBottomY);
		ret = -EINVAL;
		goto out;
	} else if ((u32) zone_area->weight > 1000) {

		iprintk("zone weight error: weight=0x%x", zone_area->weight);
		ret = -EINVAL;
		goto out;
	}

	/* conv area to sensor VVF zone */
	x0 = (u32) zone_area->leftTopX / af_zone_scale[ov5640->i_size].x_scale;
	if (x0 > (OV5640_AF_NORMALIZED_W - 8))
		x0 = (OV5640_AF_NORMALIZED_W - 8);
	x1 = ((u32) zone_area->leftTopX + (unsigned int)zone_area->rightBottomX)
	    / af_zone_scale[ov5640->i_size].x_scale;
	if (x1 > OV5640_AF_NORMALIZED_W)
		x1 = OV5640_AF_NORMALIZED_W;
	y0 = (u32) zone_area->leftTopY / af_zone_scale[ov5640->i_size].y_scale;
	if (y0 > (OV5640_AF_NORMALIZED_H - 8))
		y0 = (OV5640_AF_NORMALIZED_H - 8);
	y1 = ((u32) zone_area->leftTopY + (unsigned int)zone_area->rightBottomY)
	    / af_zone_scale[ov5640->i_size].y_scale;
	if (y1 > OV5640_AF_NORMALIZED_H)
		y1 = OV5640_AF_NORMALIZED_H;

	/* weight ranges from 1-1000 */
	/* Convert weight */
	weight = 0;
	if ((zone_area->weight > 0) && (zone_area->weight <= 125))
		weight = 1;
	else if ((zone_area->weight > 125) && (zone_area->weight <= 250))
		weight = 2;
	else if ((zone_area->weight > 250) && (zone_area->weight <= 375))
		weight = 3;
	else if ((zone_area->weight > 375) && (zone_area->weight <= 500))
		weight = 4;
	else if ((zone_area->weight > 500) && (zone_area->weight <= 625))
		weight = 5;
	else if ((zone_area->weight > 625) && (zone_area->weight <= 750))
		weight = 6;
	else if ((zone_area->weight > 750) && (zone_area->weight <= 875))
		weight = 7;
	else if (zone_area->weight > 875)
		weight = 8;

	/* Minimum zone size */
	if (((x1 - x0) >= 8) && ((y1 - y0) >= 8)) {

		ov5640->touch_area[zone].leftTopX	= (int)x0;
		ov5640->touch_area[zone].leftTopY	= (int)y0;
		ov5640->touch_area[zone].rightBottomX	= (int)(x1 - x0);
		ov5640->touch_area[zone].rightBottomY	= (int)(y1 - y0);
		ov5640->touch_area[zone].weight		= (int)weight;

	} else {
		dev_dbg(&client->dev,
			"zone %d size failed: x0=%d x1=%d y0=%d y1=%d w=%d\n",
			zone, x0, x1, y0, y1, weight);
		ret = -EINVAL;
		goto out;
	}

out:

	return ret;
}

static int ov5640_af_status(struct i2c_client *client, int num_trys)
{
	int ret = OV5640_AF_SUCCESS;
	struct ov5640 *ov5640 = to_ov5640(client);
	int af_st = 0;
	u8 af_zone0, af_zone1, af_zone2, af_zone3, af_zone4;

	if (ov5640->focus_mode == V4L2_AUTO_FOCUS_RANGE_AUTO) {
		/* Check if Focused */
		af_st = ov5640_af_fw_status(client);
		if (af_st != 0x10) {
			dev_dbg(&client->dev, "focus pending\n");
			ret = OV5640_AF_PENDING;
			goto out;
		}

		/* Check if Zones Focused */
		ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
		ov5640_reg_write(client, OV5640_CMD_MAIN, 0x07);

		ret = ov5640_af_ack(client, num_trys);
		if (ret) {
			dev_dbg(&client->dev, "zones ack failed\n");
			ret = OV5640_AF_FAIL;
			goto out;
		}

		ov5640_reg_read(client, 0x3024, &af_zone0);
		ov5640_reg_read(client, 0x3025, &af_zone1);
		ov5640_reg_read(client, 0x3026, &af_zone2);
		ov5640_reg_read(client, 0x3027, &af_zone3);
		ov5640_reg_read(client, 0x3028, &af_zone4);
		if ((af_zone0 != 0) && (af_zone1 != 0) && (af_zone2 != 0)
		    && (af_zone3 != 0) && (af_zone4 != 0)) {
			dev_dbg(&client->dev, "zones failed\n");
			ret = OV5640_AF_FAIL;
			iprintk("zones failed");
			goto out;
		}

	}

out:
	return ret;
}

/* For capture routines */
#define XVCLK 1300
static int ae_target = 44;
static int ae_low, ae_high;
static int preview_sysclk, preview_hts;

static int ov5640_get_sysclk(struct v4l2_subdev *sd)
{
	/* calculate sysclk */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5640 *ov5640 = to_ov5640(client);
	u8 val;
	int Multiplier, PreDiv, VCO, SysDiv, Pll_rdiv, Bit_div2x, sclk_rdiv,
	    sysclk;

	int sclk_rdiv_map[] = { 1, 2, 4, 8 };
	int  i = ov5640->i_size;
	const struct ov5640_timing_cfg *timing_cfg;

	if (ov5640_fmts[ov5640->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		timing_cfg = &timing_cfg_jpeg[i];
	else
		timing_cfg = &timing_cfg_yuv[i];

	ov5640_reg_read(client, 0x3034, &val);
	val &= 0x0F;
	Bit_div2x = 1;
	if (val == 8 || val == 10)
		Bit_div2x = val / 2;

	ov5640_reg_read(client, 0x3035, &val);
	SysDiv = val >> 4;
	if (SysDiv == 0)
		SysDiv = 16;

	ov5640_reg_read(client, 0x3036, &val);
	if (val == 0)
		val = 0x68;
	Multiplier = val;

	ov5640_reg_read(client, 0x3037, &val);
	if ((val & 0x0f) == 0)
		val = 0x12;

	PreDiv = val & 0x0f;
	Pll_rdiv = ((val >> 4) & 0x01) + 1;

	val = timing_cfg->sclk_dividers;
	val &= 0x03;
	sclk_rdiv = sclk_rdiv_map[val];

	VCO = XVCLK * Multiplier / PreDiv;

	sysclk = VCO / SysDiv / Pll_rdiv * 2 / Bit_div2x / sclk_rdiv;

	return sysclk;
}

static int ov5640_set_vts(struct v4l2_subdev *sd, int VTS)
{
	/* write VTS to registers */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 val;

	val = VTS & 0xFF;
	ov5640_reg_write(client, 0x380F, val);
	val = VTS >> 8;
	ov5640_reg_write(client, 0x380E, val);

	return 0;
}

static int ov5640_get_shutter(struct v4l2_subdev *sd)
{
	/* read shutter, in number of line period */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int shutter;
	u8 val;

	ov5640_reg_read(client, 0x3500, &val);
	shutter = (val & 0x0f);
	ov5640_reg_read(client, 0x3501, &val);
	shutter = (shutter << 8) + val;
	ov5640_reg_read(client, 0x3502, &val);
	shutter = (shutter << 4) + (val >> 4);

	return shutter;
}

static int ov5640_set_shutter(struct v4l2_subdev *sd, int shutter)
{
	/* write shutter, in number of line period */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 val;

	shutter = shutter & 0xFFFF;
	val = (shutter & 0x0F) << 4;
	ov5640_reg_write(client, 0x3502, val);

	val = (shutter & 0xFFF) >> 4;
	ov5640_reg_write(client, 0x3501, val);

	val = shutter >> 12;
	ov5640_reg_write(client, 0x3500, val);

	return 0;
}

static int ov5640_get_red_gain16(struct v4l2_subdev *sd)
{
	/* read gain, 16 = 1x */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 gain16;
	u8 val;

	ov5640_reg_read(client, 0x3400, &val);
	gain16 = val & 0x0F;
	ov5640_reg_read(client, 0x3401, &val);
	gain16 = (gain16 << 8) + val;

	return gain16;
}


static int ov5640_get_green_gain16(struct v4l2_subdev *sd)
{
	/* read gain, 16 = 1x */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 gain16;
	u8 val;

	ov5640_reg_read(client, 0x3402, &val);
	gain16 = val & 0x0F;
	ov5640_reg_read(client, 0x3403, &val);
	gain16 = (gain16 << 8) + val;

	return gain16;
}


static int ov5640_get_blue_gain16(struct v4l2_subdev *sd)
{
	/* read gain, 16 = 1x */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 gain16;
	u8 val;

	ov5640_reg_read(client, 0x3404, &val);
	gain16 = val & 0x0F;
	ov5640_reg_read(client, 0x3405, &val);
	gain16 = (gain16 << 8) + val;

	return gain16;
}


static int ov5640_get_gain16(struct v4l2_subdev *sd)
{
	/* read gain, 16 = 1x */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 gain16;
	u8 val;

	ov5640_reg_read(client, 0x350A, &val);
	gain16 = val & 0x03;
	ov5640_reg_read(client, 0x350B, &val);
	gain16 = (gain16 << 8) + val;

	return gain16;
}

static int ov5640_set_gain16(struct v4l2_subdev *sd, int gain16)
{
	/* write gain, 16 = 1x */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 val;
	gain16 = gain16 & 0x3FF;

	val = gain16 & 0xFF;
	ov5640_reg_write(client, 0x350b, val);

	val = gain16 >> 8;
	ov5640_reg_write(client, 0x350a, val);

	return 0;
}

static int ov5640_get_banding(struct v4l2_subdev *sd)
{
	/* get banding filter value */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 val;
	int banding;

	ov5640_reg_read(client, 0x3c01, &val);

	if (val & 0x80) {
		/* manual */
		ov5640_reg_read(client, 0x3c00, &val);
		if (val & 0x04) {
			/* 50Hz */
			banding = 50;
		} else {
			/* 60Hz */
			banding = 60;
		}
	} else {
		/* auto */
		ov5640_reg_read(client, 0x3c0c, &val);
		if (val & 0x01) {
			/* 50Hz */
			banding = 50;
		} else {
			/* 60Hz */
			banding = 60;
		}
	}
	return banding;
}

static void ov5640_set_night_mode(struct v4l2_subdev *sd, int night)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 val;
	switch (night) {
	case 0:		/*Off */
		ov5640_reg_read(client, 0x3a00, &val);
		val &= 0xFB;	/* night mode off, bit[2] = 0 */
		ov5640_reg_write(client, 0x3a00, val);
		break;
	case 1:		/* On */
		ov5640_reg_read(client, 0x3a00, &val);
		val |= 0x04;	/* night mode on, bit[2] = 1 */
		ov5640_reg_write(client, 0x3a00, val);
		break;
	default:
		break;
	}
}

static void ov5640_set_banding(struct v4l2_subdev *sd)
{
	int preview_vts;
	int band_step60, max_band60, band_step50, max_band50;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5640 *ov5640 = to_ov5640(client);
	const struct ov5640_timing_cfg *timing_cfg;
	int i = ov5640->i_size;

	if (ov5640_fmts[ov5640->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		timing_cfg = &timing_cfg_jpeg[i];
	else
		timing_cfg = &timing_cfg_yuv[i];

	/* read preview PCLK */
	preview_sysclk = ov5640_get_sysclk(sd);

	/* read preview HTS */
	preview_hts = timing_cfg->h_total_size;

	/* read preview VTS */
	preview_vts = timing_cfg->v_total_size;
	dev_info(&client->dev,
			"%s: preview_hts=0x%x, VTS: 0x%x, preview_sysclk=%ul\n",
		       __func__, preview_hts, preview_vts, preview_sysclk);

	ov5640_reg_write(client, 0x3a02, (preview_vts >> 8));
	ov5640_reg_write(client, 0x3a03, (preview_vts & 0xff));
	ov5640_reg_write(client, 0x3a14, (preview_vts >> 8));
	ov5640_reg_write(client, 0x3a15, (preview_vts & 0xff));

	/* calculate banding filter */
	/* 60Hz */
	band_step60 = preview_sysclk * 100 / preview_hts * 100 / 120;
	ov5640_reg_write(client, 0x3a0a, (band_step60 >> 8));
	ov5640_reg_write(client, 0x3a0b, (band_step60 & 0xff));

	max_band60 = (int)((preview_vts - 4) / band_step60);
	ov5640_reg_write(client, 0x3a0d, max_band60);

	/* 50Hz */
	band_step50 = preview_sysclk * 100 / preview_hts;
	ov5640_reg_write(client, 0x3a08, (band_step50 >> 8));
	ov5640_reg_write(client, 0x3a09, (band_step50 & 0xff));

	max_band50 = (int)((preview_vts - 4) / band_step50);
	ov5640_reg_write(client, 0x3a0e, max_band50);
	dev_info(&client->dev, "%s: band_step60:0x%x max_band60:0x%x\n",
				__func__, band_step60, max_band60);
	dev_info(&client->dev, "%s: band_step50:0x%x max_band50:0x%x\n",
				__func__, band_step50, max_band50);
}

static int ov5640_set_ae_target(struct v4l2_subdev *sd, int target)
{
	/* stable in high */
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int fast_high, fast_low;
	ae_low = target * 23 / 25;	/* 0.92 */
	ae_high = target * 27 / 25;	/* 1.08 */

	fast_high = ae_high << 1;
	if (fast_high > 255)
		fast_high = 255;

	fast_low = ae_low >> 1;

	ov5640_reg_write(client, 0x3a0f, ae_high);
	ov5640_reg_write(client, 0x3a10, ae_low);
	ov5640_reg_write(client, 0x3a1b, ae_high);
	ov5640_reg_write(client, 0x3a1e, ae_low);
	ov5640_reg_write(client, 0x3a11, fast_high);
	ov5640_reg_write(client, 0x3a1f, fast_low);

	return 0;
}

static int ov5640_config_preview(struct v4l2_subdev *sd)
{
	int ret;
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ov5640_reg_write(client, 0x3503, 0x00);
	ret = ov5640_config_timing(client);
	ov5640_set_banding(sd);
	ov5640_set_night_mode(sd, 0);
	ov5640_set_ae_target(sd, ae_target);

	return ret;
}

static int ov5640_config_capture(struct v4l2_subdev *sd)
{
	int ret = 0;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int preview_shutter, preview_gain16;
	u8 average;
	int capture_shutter, capture_gain16;
	int red_gain16, green_gain16, blue_gain16;
	int capture_sysclk, capture_hts, capture_vts;
	int banding, capture_bandingfilter, capture_max_band;
	long capture_gain16_shutter;
	struct ov5640 *ov5640 = to_ov5640(client);
	const struct ov5640_timing_cfg *timing_cfg;
	int i;

	/*disable aec/agc */
	ov5640_reg_write(client, 0x3503, 0x03);

	/* read preview shutter */
	preview_shutter = ov5640_get_shutter(sd);

	/* read preview gain */
	preview_gain16 = ov5640_get_gain16(sd);
	dev_info(&client->dev, "%s:preview_shutter=0x%x, preview_gain16=0x%x\n",
			       __func__, preview_shutter, preview_gain16);
	/*ov5640_reg_read(client, 0x558c, &preview_uv); */
	red_gain16	= ov5640_get_red_gain16(sd);
	green_gain16	= ov5640_get_green_gain16(sd);
	blue_gain16	= ov5640_get_blue_gain16(sd);

	/* get average */
	ov5640_reg_read(client, 0x56a1, &average);
	dev_info(&client->dev, "%s: preview avg=0x%x\n", __func__, average);

	/* turn off night mode for capture */
	ov5640_set_night_mode(sd, 0);

	/* Write capture setting */
	ov5640_reg_writes(client, hawaii_capture_init);
	ov5640_config_timing(client);

	/* preview_hts & preview_sysclk are static and set in config_preview */
	/* print preview_hts and preview_sysclk */
	dev_info(&client->dev, "%s: preview_hts=0x%x, preview_sysclk=%ul\n",
	       __func__, preview_hts, preview_sysclk);

	/* read capture VTS */
	i = ov5640->i_size;
	if (ov5640_fmts[ov5640->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		timing_cfg = &timing_cfg_jpeg[i];
	else
		timing_cfg = &timing_cfg_yuv[i];

	capture_vts	= timing_cfg->v_total_size;
	capture_hts	= timing_cfg->h_total_size;
	capture_sysclk	= ov5640_get_sysclk(sd);
	dev_info(&client->dev,
	       "%s: capture_vts=0x%x, capture_hts=0x%x, capture_sysclk=%ul\n",
	       __func__, capture_vts, capture_hts, capture_sysclk);

	/* calculate capture banding filter */
	banding = ov5640_get_banding(sd);
	if (banding == 60) {
		/* 60Hz */
		capture_bandingfilter =
		    capture_sysclk * 100 / capture_hts * 100 / 120;
	} else {
		/* 50Hz */
		capture_bandingfilter = capture_sysclk * 100 / capture_hts;
	}
	capture_max_band = (int)((capture_vts - 4) / capture_bandingfilter);
	/*preview_shutter = preview_shutter * 5 / 4; */

	/* calculate capture shutter/gain16 */
	capture_gain16_shutter =
	    preview_gain16 * preview_shutter * capture_sysclk;
	if (average > ae_low && average < ae_high) {
		/* in stable range */
		capture_gain16_shutter =
		    capture_gain16_shutter / preview_sysclk * preview_hts /
		    capture_hts * ae_target / average;
	} else {
		capture_gain16_shutter =
		    capture_gain16_shutter / preview_sysclk * preview_hts /
		    capture_hts;
	}

	/* gain to shutter */
	if (capture_gain16_shutter < (capture_bandingfilter * 16)) {
		/* shutter < 1/100 */
		capture_shutter = capture_gain16_shutter / 16;
		if (capture_shutter < 1)
			capture_shutter = 1;

		capture_gain16 = capture_gain16_shutter / capture_shutter;
		if (capture_gain16 < 16) {
			capture_gain16 = 16;
		}
	} else {
		if (capture_gain16_shutter >
		    (capture_bandingfilter * capture_max_band * 16)) {
			/* exposure reach max */
			capture_shutter =
			    capture_bandingfilter * capture_max_band;
			capture_gain16 =
			    capture_gain16_shutter / capture_shutter;
		} else {
			/* 1/100 < capture_shutter =< max,
			 * capture_shutter = n/100 */
			capture_shutter =
				(int)(capture_gain16_shutter / 16 /
					capture_bandingfilter) *
					capture_bandingfilter;
			capture_gain16 =
				capture_gain16_shutter / capture_shutter;
		}
	}

	/* write capture gain */
	ov5640_set_gain16(sd, capture_gain16);

	/* write capture shutter */
	/* capture_shutter = capture_shutter * 122 / 100; */
	dev_info(&client->dev, "%s shutter=0x%x, capture_vts=0x%x\n", __func__,
				capture_shutter, capture_vts);
	if (capture_shutter > (capture_vts - 4)) {
		capture_vts = capture_shutter + 4;
		ov5640_set_vts(sd, capture_vts);
	}
	ov5640_set_shutter(sd, capture_shutter);

	return ret;
}

static int ov5640_flash_control(struct i2c_client *client,
				enum v4l2_flash_led_mode control)
{
	int ret = 0;

	switch (control) {
	case V4L2_FLASH_LED_MODE_FLASH:
		#ifdef CONFIG_VIDEO_ADP1653
		adp1653_gpio_strobe(0);
		adp1653_gpio_toggle(1);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0x5);
		adp1653_set_ind_led(1);
		/* Flash current indicator LED ON */
		adp1653_set_torch_flash(28);
		/* Strobing should hapen later */
		#endif
		#ifdef CONFIG_VIDEO_AS3643
		as3643_gpio_toggle(1);
		usleep_range(25, 30);
		as3643_set_ind_led(0x80, 900000);
		#endif
		break;
	case V4L2_FLASH_LED_MODE_TORCH:
		#ifdef CONFIG_VIDEO_ADP1653
		adp1653_gpio_toggle(1);
		adp1653_gpio_strobe(0);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0);
		adp1653_set_ind_led(1);
		/* Torch current no indicator LED */
		adp1653_set_torch_flash(10);
		adp1653_sw_strobe(1);
		#endif
		#ifdef CONFIG_VIDEO_AS3643
		as3643_gpio_toggle(1);
		usleep_range(25, 30);
		as3643_set_torch_flash(0x80);
		#endif
		break;
	case V4L2_FLASH_LED_MODE_NONE:
	default:
		#ifdef CONFIG_VIDEO_ADP1653
		adp1653_clear_all();
		adp1653_gpio_toggle(0);
		#endif
		#ifdef CONFIG_VIDEO_AS3643
		as3643_clear_all();
		as3643_gpio_toggle(0);
		#endif
		break;
	}

	return ret;
}

static int ov5640_pre_flash(struct i2c_client *client)
{
	int ret = 0;
	struct ov5640 *ov5640 = to_ov5640(client);

	ov5640->fireflash = 0;
	if (V4L2_FLASH_LED_MODE_FLASH == ov5640->flashmode) {
		ret = ov5640_flash_control(client, ov5640->flashmode);
		ov5640->fireflash = 1;
	} else if (V4L2_FLASH_LED_MODE_FLASH_AUTO == ov5640->flashmode) {
		u8 average = 0;
		ov5640_reg_read(client, 0x56a1, &average);
		if ((average & 0xFF) < OV5640_FLASH_THRESHHOLD) {
			ret = ov5640_flash_control(client,
						V4L2_FLASH_LED_MODE_FLASH);
			ov5640->fireflash = 1;
		}
	}
	if (1 == ov5640->fireflash)
		msleep(50);

	return ret;
}

static int ov5640_af_start(struct i2c_client *client)
{

	int ret = 0;
	struct ov5640 *ov5640 = to_ov5640(client);

	if (ov5640->focus_mode == V4L2_AUTO_FOCUS_RANGE_MACRO) {
		/*
		 * FIXME: Can the af_area be set before af_macro, or does
		 * this need to be inside the af_macro func?
		 * ret = ov5640_af_area(client);
		 */
		ret = ov5640_af_macro(client);
	} else if (ov5640->focus_mode == V4L2_AUTO_FOCUS_RANGE_INFINITY)
		ret = ov5640_af_infinity(client);
	else {
		if (ov5640->touch_focus) {
			if (ov5640->touch_focus == 1)
				ret = ov5640_af_touch(client);
			else
				ret = ov5640_af_area(client);
		} else
			ret = ov5640_af_center(client);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_ACK, 0x01);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, OV5640_CMD_MAIN, 0x03);
		if (ret)
			return ret;
	}

	return ret;
}

static int ov5640_config_timing(struct i2c_client *client)
{
	struct ov5640 *ov5640 = to_ov5640(client);
	int ret, i = ov5640->i_size;
	const struct ov5640_timing_cfg *timing_cfg;
	u8 val;

	dev_info(&client->dev, "%s: code[0x%x] i:%d\n", __func__,
			       ov5640_fmts[ov5640->i_fmt].code, i);

	if (ov5640_fmts[ov5640->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		timing_cfg = &timing_cfg_jpeg[i];
	else
		timing_cfg = &timing_cfg_yuv[i];

	ret = ov5640_reg_write(client,
			       0x3800,
			       (timing_cfg->x_addr_start & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3801, timing_cfg->x_addr_start & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3802,
			       (timing_cfg->y_addr_start & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3803, timing_cfg->y_addr_start & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3804, (timing_cfg->x_addr_end & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3805, timing_cfg->x_addr_end & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3806, (timing_cfg->y_addr_end & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3807, timing_cfg->y_addr_end & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3808,
			       (timing_cfg->h_output_size & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3809, timing_cfg->h_output_size & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x380A,
			       (timing_cfg->v_output_size & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x380B, timing_cfg->v_output_size & 0xFF);
	if (ret)
		return ret;


	ret = ov5640_reg_write(client,
			       0x380C,
			       (timing_cfg->h_total_size & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x380D, timing_cfg->h_total_size & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x380E,
			       (timing_cfg->v_total_size & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x380F, timing_cfg->v_total_size & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3810,
			       (timing_cfg->isp_h_offset & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3811, timing_cfg->isp_h_offset & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3812,
			       (timing_cfg->isp_v_offset & 0xFF00) >> 8);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3813, timing_cfg->isp_v_offset & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3814,
			       ((timing_cfg->h_odd_ss_inc & 0xF) << 4) |
			       (timing_cfg->h_even_ss_inc & 0xF));
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3815,
			       ((timing_cfg->v_odd_ss_inc & 0xF) << 4) |
			       (timing_cfg->v_even_ss_inc & 0xF));
	if (ret)
		return ret;

	ov5640_reg_read(client, OV5640_TIMING_REG21, &val);
	if (timing_cfg->out_mode_sel & OV5640_BINNING_MASK)
		val |= OV5640_BINNING_MASK;
	else
		val &= ~(OV5640_BINNING_MASK);

	if (ov5640->hflip)
		val |= OV5640_HFLIP_MASK;
	else
		val &= ~(OV5640_HFLIP_MASK);

	ret = ov5640_reg_write(client, OV5640_TIMING_REG21, val);
	if (ret)
		return ret;

	ov5640_reg_read(client, OV5640_TIMING_REG20, &val);
	if (ov5640->vflip)
		val |= OV5640_VFLIP_MASK;
	else
		val &= ~(OV5640_VFLIP_MASK);

	ret = ov5640_reg_write(client, OV5640_TIMING_REG20, val);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client,
			       0x3108, timing_cfg->sclk_dividers & 0xFF);
	if (ret)
		return ret;

	ret = ov5640_reg_write(client, 0x3035, timing_cfg->sys_mipi_clk & 0xFF);
	if (ret)
		return ret;

	/* msleep(50); */

	return ret;
}

static int stream_mode = -1;
static int ov5640_s_stream(struct v4l2_subdev *sd, int enable)
{

	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	dev_info(&client->dev, "%s: enable:%d runmode:%d  stream_mode:%d\n",
			       __func__, enable, runmode, stream_mode);

	if (enable == stream_mode)
		return ret;

	if (enable) {
		int delayMs = 50;
		if (CAM_RUNNING_MODE_PREVIEW == runmode) {
			/* need more delay to get stable
			 * preview output, or burst capture may
			 * calc based on wrong exposure/gain
			 */
			delayMs = 200;
		}
		/* Power Up, Start Streaming */
		/* ret = ov5640_reg_writes(client, ov5640_stream); */
		/* use MIPI on/off as OVT suggested on AppNote */
		ov5640_reg_write(client, 0x4202, 0x00);

#ifdef TEST_PATTERN
		ov5640_reg_write(client, 0x503D, 0x80);
#endif
		msleep(delayMs);
	} else {
		/* Stop Streaming, Power Down */
		/* ret = ov5640_reg_writes(client, ov5640_power_down); */
		ov5640_reg_write(client, 0x4202, 0x0f);
	}
	stream_mode = enable;

	return ret;
}

static int ov5640_set_bus_param(struct soc_camera_device *icd,
				unsigned long flags)
{

	/* TODO: Do the right thing here, and validate bus params */
	return 0;
}
static int afFWLoaded = -1;
static int initNeeded = -1;

static int ov5640_g_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{

	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5640 *ov5640 = to_ov5640(client);

	mf->width	= ov5640_frmsizes[ov5640->i_size].width;
	mf->height	= ov5640_frmsizes[ov5640->i_size].height;
	mf->code	= ov5640_fmts[ov5640->i_fmt].code;
	mf->colorspace	= ov5640_fmts[ov5640->i_fmt].colorspace;
	mf->field	= V4L2_FIELD_NONE;

	return 0;
}

static int ov5640_try_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{

	int i_fmt;
	int i_size;

	i_fmt = ov5640_find_datafmt(mf->code);

	mf->code	= ov5640_fmts[i_fmt].code;
	mf->colorspace	= ov5640_fmts[i_fmt].colorspace;
	mf->field	= V4L2_FIELD_NONE;

	i_size = ov5640_find_framesize(mf->width, mf->height);

	mf->width	= ov5640_frmsizes[i_size].width;
	mf->height	= ov5640_frmsizes[i_size].height;

	return 0;
}

static int ov5640_s_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{

	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5640 *ov5640 = to_ov5640(client);
	int ret = 0;

	ret = ov5640_try_fmt(sd, mf);
	if (ret < 0)
		return ret;

	ov5640->i_size	= ov5640_find_framesize(mf->width, mf->height);
	ov5640->i_fmt	= ov5640_find_datafmt(mf->code);

	/* To avoide reentry init sensor, remove from here */
	if (initNeeded > 0) {
		ret = ov5640_reg_writes(client, hawaii_common_init);
		initNeeded = 0;
	}
	if (ret) {
		dev_err(&client->dev,
				"Error configuring configscript_common1\n");
		return ret;
	}

	switch ((u32) ov5640_fmts[ov5640->i_fmt].code) {
	case V4L2_MBUS_FMT_UYVY8_2X8:
		ret = ov5640_reg_writes(client, hawaii_preview_init);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x4300, 0x32);
		if (ret)
			return ret;
		break;
	case V4L2_MBUS_FMT_YUYV8_2X8:
		ret = ov5640_reg_writes(client, hawaii_preview_init);
		if (ret)
			return ret;
		ret = ov5640_reg_write(client, 0x4300, 0x30);
		if (ret)
			return ret;
		break;
	case V4L2_MBUS_FMT_JPEG_1X8:
		ret = ov5640_reg_writes(client, jpeg_init_common);
		if (ret)
			return ret;
		break;
	default:
		/* This shouldn't happen */
		ret = -EINVAL;
		return ret;
	}

	if (CAM_RUNNING_MODE_PREVIEW == runmode)
		ov5640_config_preview(sd);

	return ret;
}

static int ov5640_g_chip_ident(struct v4l2_subdev *sd,
			       struct v4l2_dbg_chip_ident *id)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (id->match.type != V4L2_CHIP_MATCH_I2C_ADDR)
		return -EINVAL;

	if (id->match.addr != client->addr)
		return -ENODEV;

	id->ident = V4L2_IDENT_OV5640;
	id->revision = 0;

	return 0;
}

/*
 * return value of this function should be
 * 0 == CAMERA_AF_STATUS_FOCUSED
 * 1 == CAMERA_AF_STATUS_FAILED
 * 2 == CAMERA_AF_STATUS_SEARCHING
 * 3 == CAMERA_AF_STATUS_CANCELLED
 * to keep consistent with auto_focus_result
 * in videodev2_brcm.h
 */
static int ov5640_get_af_status(struct i2c_client *client, int num_trys)
{
	int ret = OV5640_AF_PENDING;
	struct ov5640 *ov5640 = to_ov5640(client);

	if (atomic_read(&ov5640->focus_status)
	    == OV5640_FOCUSING) {
		ret = ov5640_af_status(client, num_trys);
		/*
		 * convert OV5640_AF_* to auto_focus_result
		 * in videodev2_brcm
		 */
		switch (ret) {
		case V4L2_AUTO_FOCUS_STATUS_REACHED:
			ret = CAMERA_AF_STATUS_FOCUSED;
			break;
		case V4L2_AUTO_FOCUS_STATUS_BUSY:
			ret = CAMERA_AF_STATUS_SEARCHING;
			break;
		case V4L2_AUTO_FOCUS_STATUS_FAILED:
			ret = CAMERA_AF_STATUS_FAILED;
			break;
		case V4L2_AUTO_FOCUS_STATUS_IDLE:
		default:
			ret = CAMERA_AF_STATUS_CANCELLED;
			break;
		}
	}
	if (atomic_read(&ov5640->focus_status)
	    == OV5640_NOT_FOCUSING) {
		ret = CAMERA_AF_STATUS_CANCELLED;	/* cancelled? */
	}
	if ((CAMERA_AF_STATUS_FOCUSED == ret) ||
	    (CAMERA_AF_STATUS_FAILED == ret))
		atomic_set(&ov5640->focus_status, OV5640_NOT_FOCUSING);

	return ret;
}

void ov5640_check_mounting(struct i2c_client *client, struct v4l2_ctrl *ctrl)
{
	struct soc_camera_subdev_desc *ssd = client->dev.platform_data;
	struct v4l2_subdev_sensor_interface_parms *iface_parms;

	if (ssd && ssd->drv_priv) {
		iface_parms = ssd->drv_priv;

		if (iface_parms->orientation == V4L2_SUBDEV_SENSOR_PORTRAIT)
			ctrl->val |= V4L2_IN_ST_HFLIP;

		if (iface_parms->facing == V4L2_SUBDEV_SENSOR_BACK)
			ctrl->val |= V4L2_IN_ST_BACK;
	} else
		dev_err(&client->dev, "Missing interface parameters\n");
}

static int ov5640_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5640 *ov5640 = container_of(ctrl->handler,
					       struct ov5640, hdl);

	struct v4l2_subdev *sd = &ov5640->subdev;
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "ov5640_g_volatile_ctrl\n");

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		ctrl->val = ov5640->brightness;
		break;
	case V4L2_CID_CONTRAST:
		ctrl->val = ov5640->contrast;
		break;
	case V4L2_CID_COLORFX:
		ctrl->val = ov5640->colorlevel;
		break;
	case V4L2_CID_SATURATION:
		ctrl->val = ov5640->saturation;
		break;
	case V4L2_CID_SHARPNESS:
		ctrl->val = ov5640->sharpness;
		break;
	case V4L2_CID_POWER_LINE_FREQUENCY:
		ctrl->val = ov5640->antibanding;
		break;
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
		ctrl->val = ov5640->whitebalance;
		break;
	case V4L2_CID_CAMERA_FRAME_RATE:
		ctrl->val = ov5640->framerate;
		break;
	case V4L2_CID_AUTO_FOCUS_RANGE:
		ctrl->val = ov5640->focus_mode;
		break;
	case V4L2_CID_CAMERA_TOUCH_AF_AREA:
		ctrl->val = ov5640->touch_focus;
		break;
	case V4L2_CID_AUTO_FOCUS_STATUS:
		/*
		 * this is called from another thread to read AF status
		 */
		ctrl->val = ov5640_get_af_status(client, 100);
		ov5640->touch_focus = 0;
		break;
	case V4L2_CID_FLASH_LED_MODE:
		ctrl->val = ov5640->flashmode;
		break;
	case V4L2_CID_HFLIP:
		ctrl->val = ov5640->hflip;
		break;
	case V4L2_CID_VFLIP:
		ctrl->val = ov5640->vflip;
		break;
	case V4L2_CID_CAM_MOUNTING:
		ov5640_check_mounting(client, ctrl);
		break;
	}

	return 0;
}

static int ov5640_s_ctrl(struct v4l2_ctrl *ctrl)
{

	struct ov5640 *ov5640 = container_of(ctrl->handler,
					       struct ov5640, hdl);

	struct v4l2_subdev *sd = &ov5640->subdev;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 ov_reg;
	int ret = 0;

	if (ctrl == NULL) {
		dev_err(&client->dev, "%s: v4l2_ctrl is null\n", __func__);
		return -EINVAL;
	}

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:

		if (ctrl->val > EV_PLUS_2)
			return -EINVAL;

		ov5640->brightness = ctrl->val;
		switch (ov5640->brightness) {
		case EV_MINUS_2:
			ret = ov5640_reg_writes(client,
						ov5640_brightness_lv4_tbl);
			break;
		case EV_MINUS_1:
			ret = ov5640_reg_writes(client,
						ov5640_brightness_lv3_tbl);
			break;
		case EV_PLUS_1:
			ret = ov5640_reg_writes(client,
						ov5640_brightness_lv1_tbl);
			break;
		case EV_PLUS_2:
			ret = ov5640_reg_writes(client,
						ov5640_brightness_lv0_tbl);
			break;
		default:
			ret = ov5640_reg_writes(client,
					ov5640_brightness_lv2_default_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_CONTRAST:

		if (ctrl->val > CONTRAST_PLUS_1)
			return -EINVAL;

		ov5640->contrast = ctrl->val;
		switch (ov5640->contrast) {
		case CONTRAST_MINUS_1:
			ret = ov5640_reg_writes(client,
						ov5640_contrast_lv5_tbl);
			break;
		case CONTRAST_PLUS_1:
			ret = ov5640_reg_writes(client,
						ov5640_contrast_lv0_tbl);
			break;
		default:
			ret = ov5640_reg_writes(client,
					ov5640_contrast_default_lv3_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_COLORFX:

		if (ctrl->val > V4L2_COLORFX_NEGATIVE)
			return -EINVAL;

		ov5640->colorlevel = ctrl->val;

		switch (ov5640->colorlevel) {
		case V4L2_COLORFX_BW:
			ret = ov5640_reg_writes(client, ov5640_effect_bw_tbl);
			break;
		case V4L2_COLORFX_SEPIA:
			ret = ov5640_reg_writes(client,
						ov5640_effect_sepia_tbl);
			break;
		case V4L2_COLORFX_NEGATIVE:
			ret = ov5640_reg_writes(client,
						ov5640_effect_negative_tbl);
			break;
		default:
		case V4L2_COLORFX_NONE:
			ret = ov5640_reg_writes(client,
						ov5640_effect_normal_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_SATURATION:

		if (ctrl->val > OV5640_SATURATION_MAX)
			return -EINVAL;

		ov5640->saturation = ctrl->val;
		switch (ov5640->saturation) {
		case OV5640_SATURATION_MIN:
			ret = ov5640_reg_writes(client,
						ov5640_saturation_lv0_tbl);
			break;
		case OV5640_SATURATION_MAX:
			ret = ov5640_reg_writes(client,
						ov5640_saturation_lv5_tbl);
			break;
		default:
			ret = ov5640_reg_writes(client,
					ov5640_saturation_default_lv3_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_SHARPNESS:

		if (ctrl->val > OV5640_SHARPNESS_MAX)
			return -EINVAL;

		ov5640->sharpness = ctrl->val;
		switch (ov5640->sharpness) {
		case OV5640_SHARPNESS_MIN:
			ret = ov5640_reg_writes(client,
						ov5640_sharpness_lv0_tbl);
			break;
		case OV5640_SHARPNESS_MAX:
			ret = ov5640_reg_writes(client,
						ov5640_sharpness_lv3_tbl);
			break;
		default:
			ret = ov5640_reg_writes(client,
					ov5640_sharpness_default_lv2_tbl);
			break;
		}
		if (ret)
			return ret;
		break;

	case V4L2_CID_POWER_LINE_FREQUENCY:

		if (ctrl->val > V4L2_CID_POWER_LINE_FREQUENCY_AUTO)
			return -EINVAL;

		ov5640->antibanding = ctrl->val;

		switch (ov5640->antibanding) {
		case V4L2_CID_POWER_LINE_FREQUENCY_DISABLED:
			break;
		case V4L2_CID_POWER_LINE_FREQUENCY_50HZ:
			ret = ov5640_reg_writes(client,
						ov5640_antibanding_50z_tbl);
			break;
		case V4L2_CID_POWER_LINE_FREQUENCY_60HZ:
			ret = ov5640_reg_writes(client,
						ov5640_antibanding_60z_tbl);
			break;
		case V4L2_CID_POWER_LINE_FREQUENCY_AUTO:
		default:
			ret = ov5640_reg_writes(client,
						ov5640_antibanding_auto_tbl);
			break;
		}
		if (ret)
			return ret;
		break;

	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:

		if (ctrl->val > V4L2_WHITE_BALANCE_CLOUDY)
			return -EINVAL;

		ov5640->whitebalance = ctrl->val;

		ret = ov5640_reg_read(client, 0x3406, &ov_reg);
		if (ret)
			return ret;

		switch (ov5640->whitebalance) {
		case V4L2_WHITE_BALANCE_FLUORESCENT:
			ov_reg |= 0x01;
			ret = ov5640_reg_write(client, 0x3406, ov_reg);
			ret = ov5640_reg_writes(client, ov5640_wb_fluorescent);
			break;
		case V4L2_WHITE_BALANCE_DAYLIGHT:
			ov_reg |= 0x01;
			ret = ov5640_reg_write(client, 0x3406, ov_reg);
			ret = ov5640_reg_writes(client, ov5640_wb_daylight);
			break;
		case V4L2_WHITE_BALANCE_CLOUDY:
			ov_reg |= 0x01;
			ret = ov5640_reg_write(client, 0x3406, ov_reg);
			ret = ov5640_reg_writes(client, ov5640_wb_cloudy);
			break;
		case V4L2_WHITE_BALANCE_INCANDESCENT:
			ov_reg |= 0x01;
			ret = ov5640_reg_write(client, 0x3406, ov_reg);
			ret = ov5640_reg_writes(client, ov5640_wb_tungsten);
			break;
		case V4L2_WHITE_BALANCE_MANUAL:
			break;
		default:
		case V4L2_WHITE_BALANCE_AUTO:
			ov_reg &= ~(0x01);
			ret = ov5640_reg_write(client, 0x3406, ov_reg);
			ret = ov5640_reg_writes(client, ov5640_wb_def);
			break;
		}
		if (ret) {
			printk(KERN_ERR "Some error in AWB\n");
			return ret;
		}
		break;

	case V4L2_CID_CAMERA_FRAME_RATE:

		if (ctrl->val > FRAME_RATE_30)
			return -EINVAL;

		if ((ov5640->i_size < OV5640_SIZE_QVGA) ||
		    (ov5640->i_size > OV5640_SIZE_1280x960)) {
			if (ctrl->val == FRAME_RATE_30 ||
			    ctrl->val == FRAME_RATE_AUTO)
				return 0;
			else
				return -EINVAL;
		}

		ov5640->framerate = ctrl->val;
		iprintk("framerate = %d\n", ov5640->framerate);

		switch (ov5640->framerate) {
		case FRAME_RATE_5:
			ret = ov5640_reg_writes(client, ov5640_fps_5);
			break;
		case FRAME_RATE_7:
			ret = ov5640_reg_writes(client, ov5640_fps_7);
			break;
		case FRAME_RATE_10:
			ret = ov5640_reg_writes(client, ov5640_fps_10);
			break;
		case FRAME_RATE_15:
			ret = ov5640_reg_writes(client, ov5640_fps_15);
			break;
		case FRAME_RATE_20:
			ret = ov5640_reg_writes(client, ov5640_fps_20);
			break;
		case FRAME_RATE_25:
			ret = ov5640_reg_writes(client, ov5640_fps_25);
			break;
		case FRAME_RATE_30:
		case FRAME_RATE_AUTO:
		default:
			ret = ov5640_reg_writes(client, ov5640_fps_30);
			break;
		}
		if (ret)
			return ret;
		break;

	case V4L2_CID_AUTO_FOCUS_RANGE:

		if (ctrl->val > V4L2_AUTO_FOCUS_RANGE_INFINITY)
			return -EINVAL;

		ov5640->focus_mode = ctrl->val;

		/*
		 * Donot start the AF cycle here
		 * AF Start will be called later in
		 * V4L2_CID_CAMERA_SET_AUTO_FOCUS only for auto, macro mode
		 * it wont be called for infinity.
		 * Donot worry about resolution change for now.
		 * From userspace we set the resolution first
		 * and then set the focus mode.
		 */
		switch (ov5640->focus_mode) {
		case V4L2_AUTO_FOCUS_RANGE_MACRO:
			/*
			 * set the table for macro mode
			 */
			break;
		case V4L2_AUTO_FOCUS_RANGE_INFINITY:
			/*
			 * set the table for infinity
			 */
			ret = 0;
			break;
		default:
			ret = ov5640_af_enable(client);
			break;
		}

		if (ret)
			return ret;
		break;

	case V4L2_CID_CAMERA_TOUCH_AF_AREA:

		if (ov5640->touch_focus < OV5640_MAX_FOCUS_AREAS) {
			v4l2_touch_area touch_area;
			if (copy_from_user(&touch_area,
					   (v4l2_touch_area *) ctrl->val,
					   sizeof(v4l2_touch_area)))
				return -EINVAL;

			iprintk("z=%d x=0x%x y=0x%x w=0x%x h=0x%x weight=0x%x",
				ov5640->touch_focus, touch_area.leftTopX,
				touch_area.leftTopY, touch_area.rightBottomX,
				touch_area.rightBottomY, touch_area.weight);

			ret = ov5640_af_zone_conv(client, &touch_area,
						  ov5640->touch_focus);
			if (ret == 0)
				ov5640->touch_focus++;
			ret = 0;

		} else
			dev_dbg(&client->dev,
				"Maximum touch focus areas already set\n");

		break;

	case V4L2_CID_AUTO_FOCUS_STOP:
		if (atomic_read(&ov5640->focus_status)
				== OV5640_FOCUSING) {
			ret = ov5640_af_release(client);
			atomic_set(&ov5640->focus_status,
					OV5640_NOT_FOCUSING);
		}
		ov5640->touch_focus = 0;
		break;

	case V4L2_CID_AUTO_FOCUS_START:
		if (1 != afFWLoaded) {
			ret = ov5640_af_enable(client);
			if (ret)
				return ret;
			afFWLoaded = 1;
		}
		/* check if preflash is needed */
		ret = ov5640_pre_flash(client);

		ret = ov5640_af_start(client);
		atomic_set(&ov5640->focus_status, OV5640_FOCUSING);
		if (ret)
			return ret;
		break;

	case V4L2_CID_FLASH_LED_MODE:
		ret = ov5640_set_flash_mode(ctrl, client);
		break;

	case V4L2_CID_CAM_PREVIEW_ONOFF:
		{
			printk(KERN_INFO
			       "ov5640 PREVIEW_ONOFF:%d runmode = %d\n",
			       ctrl->val, runmode);
			if (ctrl->val)
				runmode = CAM_RUNNING_MODE_PREVIEW;
			else
				runmode = CAM_RUNNING_MODE_NOTREADY;

			break;
		}

	case V4L2_CID_CAM_CAPTURE:
		runmode = CAM_RUNNING_MODE_CAPTURE;
		if (ov5640->fireflash) {
			ov5640_flash_control(client, V4L2_FLASH_LED_MODE_FLASH);
			msleep(50);
		}
		ov5640_config_capture(sd);
		break;

	case V4L2_CID_CAM_CAPTURE_DONE:
		printk(KERN_INFO "ov5640 runmode = capture_done\n");
		runmode = CAM_RUNNING_MODE_CAPTURE_DONE;
		break;

	case V4L2_CID_HFLIP:
		if (ctrl->val != ov5640->hflip) {
			ov5640_reg_read(client, OV5640_TIMING_REG21, &ov_reg);
			if (ctrl->val)
				ov_reg |= OV5640_HFLIP_MASK;
			else
				ov_reg &= ~(OV5640_HFLIP_MASK);

			ret = ov5640_reg_write(client, OV5640_TIMING_REG21,
						ov_reg);
			if (ret)
				return ret;
			ov5640->hflip = ctrl->val;
		}
		break;

	case V4L2_CID_VFLIP:
		if (ctrl->val != ov5640->vflip) {
			ov5640_reg_read(client, OV5640_TIMING_REG21, &ov_reg);
			if (ctrl->val)
				ov_reg |= OV5640_VFLIP_MASK;
			else
				ov_reg &= ~(OV5640_VFLIP_MASK);

			ret = ov5640_reg_write(client, OV5640_TIMING_REG20,
						ov_reg);
			if (ret)
				return ret;
			ov5640->vflip = ctrl->val;
		}
		break;
	}

	return ret;
}

int set_flash_mode(enum v4l2_flash_led_mode mode, struct ov5640 *ov5640)
{
	if (ov5640->flashmode == mode)
		return 0;

#ifdef CONFIG_VIDEO_ADP1653
	if (mode == V4L2_FLASH_LED_MODE_NONE) {
		if (ov5640->flashmode != V4L2_FLASH_LED_MODE_NONE) {
			adp1653_clear_all();
			adp1653_gpio_toggle(0);
			mode = V4L2_FLASH_LED_MODE_NONE;
		}
	} else if (mode == V4L2_FLASH_LED_MODE_TORCH) {
		if ((ov5640->flashmode == V4L2_FLASH_LED_MODE_FLASH)
		|| (ov5640->flashmode == V4L2_FLASH_LED_MODE_FLASH_AUTO))
			set_flash_mode(V4L2_FLASH_LED_MODE_NONE, ov5640);
		adp1653_gpio_toggle(1);
		adp1653_gpio_strobe(0);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0);
		adp1653_set_ind_led(1);
		/* Torch current no indicator LED */
		adp1653_set_torch_flash(10);
		adp1653_sw_strobe(1);
	} else if (mode == V4L2_FLASH_LED_MODE_FLASH) {
		if ((ov5640->flashmode == V4L2_FLASH_LED_MODE_TORCH)
		|| (ov5640->flashmode == V4L2_FLASH_LED_MODE_FLASH_AUTO))
			set_flash_mode(V4L2_FLASH_LED_MODE_NONE, ov5640);
		adp1653_gpio_strobe(0);
		adp1653_gpio_toggle(1);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0x5);
		adp1653_set_ind_led(1);
		/* Flash current indicator LED ON */
		adp1653_set_torch_flash(28);
		/* Strobing should hapen later */
	} else if (mode == V4L2_FLASH_LED_MODE_FLASH_AUTO) {
		if ((ov5640->flashmode == V4L2_FLASH_LED_MODE_TORCH)
		|| (ov5640->flashmode == V4L2_FLASH_LED_MODE_FLASH))
			set_flash_mode(V4L2_FLASH_LED_MODE_NONE, ov5640);
		adp1653_gpio_strobe(0);
		adp1653_gpio_toggle(1);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0x5);
		adp1653_set_ind_led(1);
		/* Flash current indicator LED ON */
		adp1653_set_torch_flash(28);
		/* Camera sensor will strobe if required */
	} else {
		return -EINVAL;
	}
	ov5640->flashmode = mode;
#endif

#ifdef CONFIG_VIDEO_AS3643
	if (mode == V4L2_FLASH_LED_MODE_NONE) {
		if (ov5640->flashmode != V4L2_FLASH_LED_MODE_NONE) {
			as3643_clear_all();
			as3643_gpio_toggle(0);
		}
	} else if (mode == V4L2_FLASH_LED_MODE_TORCH) {
		as3643_gpio_toggle(1);
		usleep_range(25, 30);
		as3643_set_torch_flash(0x80);
	} else if (mode == V4L2_FLASH_LED_MODE_FLASH) {
		as3643_gpio_toggle(1);
		usleep_range(25, 30);
		as3643_set_ind_led(0x80, 900000);
	} else if (mode == V4L2_FLASH_LED_MODE_FLASH_AUTO) {
		u8 average;
		struct i2c_client *client =
			v4l2_get_subdevdata(&(ov5640->subdev));
		ov5640_reg_read(client, 0x56a1, &average);
		if ((average&0xFF) < 32) {
			as3643_gpio_toggle(1);
			usleep_range(25, 30);
			as3643_set_ind_led(0x80, 900000);
		}
	} else {
		return -EINVAL;
	}
	ov5640->flashmode = mode;
#endif

	return 0;
}

static int ov5640_set_flash_mode(struct v4l2_ctrl *ctrl,
				struct i2c_client *client)
{
	int ret = 0;
	struct ov5640 *ov5640 = to_ov5640(client);
	enum v4l2_flash_led_mode mode;

	if (ctrl)
		mode = ctrl->val;
	else {
		dev_err(&client->dev, "v4l2_ctrl is null\n");
		return -EINVAL;
	}

	switch (mode) {
	case V4L2_FLASH_LED_MODE_FLASH:
	case V4L2_FLASH_LED_MODE_FLASH_AUTO:
		ov5640->flashmode = mode;
		ov5640->fireflash = 1;
		break;
	case V4L2_FLASH_LED_MODE_TORCH:
		ov5640_flash_control(client, mode);
		break;
	case V4L2_FLASH_LED_MODE_NONE:
	default:
		ov5640_flash_control(client, mode);
		ov5640->flashmode = mode;
		ov5640->fireflash = 0;
		break;
	}

	return ret;
}



static long ov5640_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case VIDIOC_THUMB_SUPPORTED:
		{
			int *p = arg;
			*p = 0;	/* no we don't support thumbnail */
			break;
		}
	case VIDIOC_JPEG_G_PACKET_INFO:
		{
			struct v4l2_jpeg_packet_info *p =
				(struct v4l2_jpeg_packet_info *)arg;
			p->padded = 0;
			p->packet_size = 0x400;
			break;
		}
	case VIDIOC_SENSOR_G_OPTICAL_INFO:
		{
			struct v4l2_sensor_optical_info *p =
				(struct v4l2_sensor_optical_info *)arg;
			/* assuming 67.5 degree diagonal viewing angle */
			p->hor_angle.numerator = 5401;
			p->hor_angle.denominator = 100;
			p->ver_angle.numerator = 3608;
			p->ver_angle.denominator = 100;
			p->focus_distance[0] = 10;    /* near focus in cm */
			p->focus_distance[1] = 100;   /* optimal focus in cm */
			p->focus_distance[2] = -1;    /* infinity */
			p->focal_length.numerator = 342;
			p->focal_length.denominator = 100;
			break;
		}
		default:
			ret = -ENOIOCTLCMD;
			break;
	}
	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int ov5640_g_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (reg->match.type != V4L2_CHIP_MATCH_I2C_ADDR || reg->size > 2)
		return -EINVAL;

	if (reg->match.addr != client->addr)
		return -ENODEV;

	reg->size = 2;
	if (ov5640_reg_read(client, reg->reg, &reg->val))
		return -EIO return 0;
}

static int ov5640_s_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (reg->match.type != V4L2_CHIP_MATCH_I2C_ADDR || reg->size > 2)
		return -EINVAL;

	if (reg->match.addr != client->addr)
		return -ENODEV;

	if (ov5640_reg_write(client, reg->reg, reg->val))
		return -EIO;

	return 0;
}
#endif
static int ov5640_init(struct i2c_client *client)
{
	struct ov5640 *ov5640 = to_ov5640(client);
	int ret = 0;

	ret = ov5640_reg_writes(client, configscript_common1);
	if (ret)
		goto out;

#if defined(CONFIG_RHEA_CLOVER_ICS)
	/*Code turn off flash led */
	if (ov5640_reg_write(client, 0x3000, 0x00))
		goto out;
	if (ov5640_reg_write(client, 0x3004, 0xFF))
		goto out;
	if (ov5640_reg_write(client, 0x3016, 0x02))
		goto out;
	if (ov5640_reg_write(client, 0x3b07, 0x0A))
		goto out;
	if (ov5640_reg_write(client, 0x3b00, 0x03))
		goto out;
#endif

	/* Power Up, Start Streaming for AF Init */
	ret = ov5640_reg_writes(client, ov5640_stream);
	if (ret)
		goto out;
	/* Delay for sensor streaming */
	msleep(20);

	/* AF Init */
	ret = ov5640_af_enable(client);
	if (ret)
		goto out;

	/* Stop Streaming, Power Down */
	ret = ov5640_reg_writes(client, ov5640_power_down);

	/* default brightness and contrast */
	ov5640->brightness	= EV_DEFAULT;
	ov5640->contrast	= CONTRAST_DEFAULT;
	ov5640->colorlevel	= V4L2_COLORFX_NONE;
	ov5640->antibanding	= V4L2_CID_POWER_LINE_FREQUENCY_AUTO;
	ov5640->whitebalance	= V4L2_WHITE_BALANCE_AUTO;
	ov5640->framerate	= FRAME_RATE_AUTO;
	ov5640->focus_mode	= V4L2_AUTO_FOCUS_RANGE_AUTO;
	ov5640->touch_focus	= 0;
	atomic_set(&ov5640->focus_status, OV5640_NOT_FOCUSING);
	ov5640->flashmode	= V4L2_FLASH_LED_MODE_NONE;
	ov5640->fireflash	= 0;

	dev_dbg(&client->dev, "Sensor initialized\n");

out:
	return ret;
}

static int ov5640_s_power(struct v4l2_subdev *sd, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);
	struct ov5640 *ov5640 = to_ov5640(client);
	int ret = 0;

	if (!on)
		return soc_camera_power_off(&client->dev, ssdd);
	initNeeded = 1;
	return soc_camera_power_on(&client->dev, ssdd);
}

/*
 * Interface active, can use i2c. If it fails, it can indeed mean, that
 *this wasn't our capture interface, so, we wait for the right one
 */
static int ov5640_video_probe(struct i2c_client *client)
{
	unsigned long flags;
	int ret = 0;
	u8 id_high, id_low, revision = 0;


	ret = ov5640_reg_read(client, OV5640_CHIP_ID_HIGH, &id_high);
	ret += ov5640_reg_read(client, OV5640_CHIP_ID_LOW, &id_low);
	ret += ov5640_reg_read(client, 0x302A, &revision);
	if (ret) {
		dev_err(&client->dev, "Failure to detect OV5640 chip\n");
		goto out;
	}

	revision &= 0xF;
	flags = SOCAM_DATAWIDTH_8;
	dev_info(&client->dev, "Detected a OV5640 chip, revision %x\n",
		 revision);

out:
	return ret;
}

static void ov5640_video_remove(struct soc_camera_device *icd)
{

	dev_dbg(icd->control, "Video removed: %p, %p\n",
		icd->parent, icd->vdev);
}


static struct v4l2_subdev_core_ops ov5640_subdev_core_ops = {
	.g_chip_ident	= ov5640_g_chip_ident,
	.s_power	= ov5640_s_power,
	.ioctl		= ov5640_ioctl,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register	= ov5640_g_register,
	.s_register	= ov5640_s_register,
#endif
};

static int ov5640_enum_fmt(struct v4l2_subdev *sd, unsigned int index,
			   enum v4l2_mbus_pixelcode *code)
{

	if (index >= ARRAY_SIZE(ov5640_fmts))
		return -EINVAL;

	*code = ov5640_fmts[index].code;
	return 0;
}

static int ov5640_enum_framesizes(struct v4l2_subdev *sd,
				  struct v4l2_frmsizeenum *fsize)
{

	if (fsize->index >= OV5640_SIZE_LAST)
		return -EINVAL;

	fsize->type		= V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->pixel_format	= V4L2_PIX_FMT_UYVY;

	fsize->discrete		= ov5640_frmsizes[fsize->index];

	return 0;
}

/* we only support fixed frame rate */
static int ov5640_enum_frameintervals(struct v4l2_subdev *sd,
				      struct v4l2_frmivalenum *interval)
{
	int size;

	if (interval->index >= 1)
		return -EINVAL;

	interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;

	size = ov5640_find_framesize(interval->width, interval->height);

	switch (size) {
	case OV5640_SIZE_5MP:
		interval->discrete.numerator	= 2;
		interval->discrete.denominator	= 15;
		break;
	case OV5640_SIZE_QXGA:
	case OV5640_SIZE_UXGA:
		interval->discrete.numerator	= 1;
		interval->discrete.denominator	= 15;
		break;
	case OV5640_SIZE_720P:
		interval->discrete.numerator	= 1;
		interval->discrete.denominator	= 0;
		break;
	case OV5640_SIZE_VGA:
	case OV5640_SIZE_QVGA:
	case OV5640_SIZE_1280x960:
	default:
		interval->discrete.numerator	= 1;
		interval->discrete.denominator	= 24;
		break;
	}
	return 0;
}

static int ov5640_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5640 *ov5640 = to_ov5640(client);
	struct v4l2_captureparm *cparm;

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	cparm = &param->parm.capture;

	memset(param, 0, sizeof(*param));
	param->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	cparm->capability = V4L2_CAP_TIMEPERFRAME;

	switch (ov5640->i_size) {
	case OV5640_SIZE_5MP:
		cparm->timeperframe.numerator	= 2;
		cparm->timeperframe.denominator	= 15;
		break;
	case OV5640_SIZE_QXGA:
	case OV5640_SIZE_UXGA:
		cparm->timeperframe.numerator	= 1;
		cparm->timeperframe.denominator	= 15;
		break;
	case OV5640_SIZE_1280x960:
	case OV5640_SIZE_720P:
	case OV5640_SIZE_VGA:
	case OV5640_SIZE_QVGA:
	default:
		cparm->timeperframe.numerator	= 1;
		cparm->timeperframe.denominator	= 24;
		break;
	}

	return 0;
}

static int ov5640_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{

	/*
	 * FIXME: This just enforces the hardcoded framerates until this is
	 * flexible enough.
	 */
	return ov5640_g_parm(sd, param);
}

static struct v4l2_subdev_video_ops ov5640_subdev_video_ops = {
	.s_stream		= ov5640_s_stream,
	.s_mbus_fmt		= ov5640_s_fmt,
	.g_mbus_fmt		= ov5640_g_fmt,
	.try_mbus_fmt		= ov5640_try_fmt,
	.enum_mbus_fmt		= ov5640_enum_fmt,
	.enum_mbus_fsizes	= ov5640_enum_framesizes,
	.enum_framesizes	= ov5640_enum_framesizes,
	.enum_frameintervals	= ov5640_enum_frameintervals,
	.g_parm			= ov5640_g_parm,
	.s_parm			= ov5640_s_parm,
};

static int ov5640_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{

	/* Quantity of initial bad frames to skip. Revisit. */
	/* Waiting for AWB stability, avoid green color issue */
	*frames = 2;

	return 0;
}

static int ov5640_g_interface_parms(struct v4l2_subdev *sd,
				struct v4l2_subdev_sensor_interface_parms
				*parms)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5640 *ov5640 = to_ov5640(client);
	u8 sclk_dividers;

	if (!parms)
		return -EINVAL;

	parms->if_type	= ov5640->plat_parms->if_type;
	parms->if_mode	= ov5640->plat_parms->if_mode;
	parms->parms	= ov5640->plat_parms->parms;

	/* set the hs term time */
	if (ov5640_fmts[ov5640->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		sclk_dividers = timing_cfg_jpeg[ov5640->i_size].sclk_dividers;
	else
		sclk_dividers = timing_cfg_yuv[ov5640->i_size].sclk_dividers;

	if (sclk_dividers == 0x01)
		parms->parms.serial.hs_term_time = 0x01;
	else
		parms->parms.serial.hs_term_time = 0x08;

	#ifdef CONFIG_MACH_HAWAII_GARNET_C_A18
	parms->parms.serial.hs_term_time = 0x06;
	#endif
	switch (ov5640->framerate) {
	case FRAME_RATE_5:
		parms->parms.serial.hs_settle_time = 9;
		break;
	case FRAME_RATE_7:
		parms->parms.serial.hs_settle_time = 6;
		break;
	case FRAME_RATE_10:
	case FRAME_RATE_15:
	case FRAME_RATE_25:
	case FRAME_RATE_30:
	case FRAME_RATE_AUTO:
	default:
		parms->parms.serial.hs_settle_time = 2;
		break;
	}

	return 0;
}

static struct v4l2_subdev_sensor_ops ov5640_subdev_sensor_ops = {
	.g_skip_frames		= ov5640_g_skip_frames,
	.g_interface_parms	= ov5640_g_interface_parms,
};

static struct v4l2_subdev_ops ov5640_subdev_ops = {
	.core	= &ov5640_subdev_core_ops,
	.video	= &ov5640_subdev_video_ops,
	.sensor	= &ov5640_subdev_sensor_ops,
};

static int ov5640_probe(struct i2c_client *client,
			const struct i2c_device_id *did)
{
	unsigned int i;
	struct ov5640 *ov5640;
	struct soc_camera_subdev_desc *ssd = client->dev.platform_data;
	struct soc_camera_desc *sdesc = container_of(ssd, struct soc_camera_desc, subdev_desc);
	struct soc_camera_device *icd;
	int ret;

	if (!ssd) {
		dev_err(&client->dev, "OV5640 driver needs platform data\n");
		return -EINVAL;
	}

	if (!ssd->drv_priv) {
		dev_err(&client->dev,
			"OV5640 driver needs i/f platform data\n");
		return -EINVAL;
	}

	ov5640 = kzalloc(sizeof(struct ov5640), GFP_KERNEL);
	if (!ov5640)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&ov5640->subdev, client, &ov5640_subdev_ops);

	ov5640->i_size	= OV5640_SIZE_1280x960;
	ov5640->i_fmt	= 0;	/* First format in the list */
	ov5640->plat_parms = ssd->drv_priv;

	struct v4l2_subdev *subdev = i2c_get_clientdata(client); // change this to &ov5640->subdev


	v4l2_i2c_subdev_init(&ov5640->subdev, client, &ov5640_subdev_ops);

	v4l2_ctrl_handler_init(&ov5640->hdl, ARRAY_SIZE(ov5640_controls) +
						OV5640_NUM_OF_STD_CTRLS);
	if (ov5640->hdl.error)
		dev_dbg(&client->dev, "Error set during init itself! %d\n",
			ov5640->hdl.error);

	/* register standard controls */
	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_BRIGHTNESS, EV_MINUS_2, EV_PLUS_2, 1, EV_DEFAULT);

	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops, V4L2_CID_CONTRAST,
			CONTRAST_MINUS_1, CONTRAST_PLUS_1, 1, CONTRAST_DEFAULT);

	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops, V4L2_CID_SATURATION,
		OV5640_SATURATION_MIN, OV5640_SATURATION_MAX,
		OV5640_SATURATION_STEP, OV5640_SATURATION_DEF);

	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops, V4L2_CID_SHARPNESS,
		OV5640_SHARPNESS_MIN, OV5640_SHARPNESS_MAX,
		OV5640_SHARPNESS_STEP, OV5640_SHARPNESS_DEF);

	/* For BUTTON controls: min = max = step = def = 0 */
/*	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_AUTO_FOCUS_START, 0, 0, 0, 0);

	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_AUTO_FOCUS_STOP, 0, 0, 0, 0);

	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
			V4L2_CID_AUTO_FOCUS_STATUS, 0,
			(V4L2_AUTO_FOCUS_STATUS_BUSY |
			V4L2_AUTO_FOCUS_STATUS_REACHED |
			V4L2_AUTO_FOCUS_STATUS_FAILED),
			0, V4L2_AUTO_FOCUS_STATUS_IDLE);
*/
	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_HFLIP, 0, 1, 1, 0);
	ov5640->hflip = 0;

	/* Correct flipped mounting on hawaii_garnet_edn010 */
#ifdef CONFIG_MACH_HAWAII_GARNET
	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_VFLIP, 0, 1, 1, 1);
	ov5640->vflip = 1;
#else
	v4l2_ctrl_new_std(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_VFLIP, 0, 1, 1, 0);
	ov5640->vflip = 0;
#endif

	if (ov5640->hdl.error) {
		dev_err(&client->dev,
			"Standard controls initialization error %d\n",
			ov5640->hdl.error);
		ret = ov5640->hdl.error;
		goto ctrl_hdl_err;
	}

	/* register standard menu controls */
	v4l2_ctrl_new_std_menu(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_COLORFX, V4L2_COLORFX_NEGATIVE, 0,
		V4L2_COLORFX_NONE);

	v4l2_ctrl_new_std_menu(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_POWER_LINE_FREQUENCY,
		V4L2_CID_POWER_LINE_FREQUENCY_AUTO, 0,
		V4L2_CID_POWER_LINE_FREQUENCY_50HZ);

	v4l2_ctrl_new_std_menu(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		V4L2_WHITE_BALANCE_CLOUDY, 0,
		V4L2_WHITE_BALANCE_AUTO);

	v4l2_ctrl_new_std_menu(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_AUTO_FOCUS_RANGE,
		V4L2_AUTO_FOCUS_RANGE_INFINITY, 0,
		V4L2_AUTO_FOCUS_RANGE_AUTO);

	v4l2_ctrl_new_std_menu(&ov5640->hdl, &ov5640_ctrl_ops,
		V4L2_CID_FLASH_LED_MODE,
		V4L2_FLASH_LED_MODE_TORCH, 0,
		V4L2_FLASH_LED_MODE_FLASH);

	if (ov5640->hdl.error) {
		dev_err(&client->dev,
			"Standard menu controls initialization error %d\n",
			ov5640->hdl.error);
		ret = ov5640->hdl.error;
		goto ctrl_hdl_err;
	}

	/* register custom controls */
	for (i = 0; i < ARRAY_SIZE(ov5640_controls); ++i)
		v4l2_ctrl_new_custom(&ov5640->hdl, &ov5640_controls[i], NULL);

	ov5640->subdev.ctrl_handler = &ov5640->hdl;
	if (ov5640->hdl.error) {
		dev_err(&client->dev,
			"Custom controls initialization error %d\n",
			ov5640->hdl.error);
		ret = ov5640->hdl.error;
		goto ctrl_hdl_err;
	}

	ret = ov5640_s_power(subdev, 1);
	if (ret < 0)
		return ret;

	ret = ov5640_video_probe(client);
	if (ret) {
		pr_err("ov5640_probe: failed to probe the sensor\n");
		goto vid_probe_fail;
	}

	/* init the sensor here */
	ret = ov5640_init(client);
	if (ret) {
		dev_err(&client->dev, "Failed to initialize sensor\n");
		goto init_fail;
	}

	return ret;

ctrl_hdl_err:
init_fail:
vid_probe_fail:
	v4l2_ctrl_handler_free(&ov5640->hdl);
	kfree(ov5640);
	pr_err("ov5640_probe failed with ret = %d\n", ret);
	return ret;
}

static int ov5640_remove(struct i2c_client *client)
{
	struct ov5640 *ov5640 = to_ov5640(client);
	struct soc_camera_device *icd = client->dev.platform_data;
	v4l2_ctrl_handler_free(&ov5640->hdl);
	ov5640_video_remove(icd);
	client->driver = NULL;
	kfree(ov5640);

	return 0;
}

static const struct i2c_device_id ov5640_id[] = {
	{"ov5640", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ov5640_id);

static struct i2c_driver ov5640_i2c_driver = {
	.driver = {
		.name = "ov5640",
	},
	.probe		= ov5640_probe,
	.remove		= ov5640_remove,
	.id_table	= ov5640_id,
};

static int __init ov5640_mod_init(void)
{

	return i2c_add_driver(&ov5640_i2c_driver);
}

static void __exit ov5640_mod_exit(void)
{

	i2c_del_driver(&ov5640_i2c_driver);
}

late_initcall(ov5640_mod_init);
module_exit(ov5640_mod_exit);

MODULE_DESCRIPTION("OmniVision OV5640 Camera driver");
MODULE_AUTHOR("Sergio Aguirre <saaguirre@ti.com>");
MODULE_LICENSE("GPL v2");
