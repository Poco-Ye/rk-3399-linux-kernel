// SPDX-License-Identifier: GPL-2.0
/*
 * imx317 driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

#define IMX317_LINK_FREQ_288MHZ		288000000
/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
#define IMX317_PIXEL_RATE		(IMX317_LINK_FREQ_288MHZ * 2 * 2 / 10)
#define IMX317_XVCLK_FREQ		24000000

#define CHIP_ID				0x03
#define IMX317_REG_CHIP_ID		0x3004

#define IMX317_REG_CTRL_MODE		0x3000
#define IMX317_MODE_SW_STANDBY		0x12
#define IMX317_MODE_STREAMING		0x00

#define IMX317_REG_EXPOSURE_H		0x300D
#define IMX317_REG_EXPOSURE_L		0x300C
#define IMX317_EXPOSURE_MIN		8
#define IMX317_EXPOSURE_STEP		1
#define IMX317_VTS_MAX			0x7fff

#define IMX317_REG_GAIN_H		0x300B
#define IMX317_REG_GAIN_L		0x300A
#define IMX317_GAIN_H_MASK		0x07
#define IMX317_GAIN_H_SHIFT		8
#define IMX317_GAIN_L_MASK		0xFF
#define IMX317_GAIN_MIN			0x0800
#define IMX317_GAIN_MAX			0xFFFF
#define IMX317_GAIN_STEP		1
#define IMX317_GAIN_DEFAULT		0x0BDF

#define IMX317_REG_VTS_H		0x30FA
#define IMX317_REG_VTS_M		0x30F9
#define IMX317_REG_VTS_L		0x30F8

#define REG_NULL			0xFFFF
#define REG_DELAY			0xFFFE

#define IMX317_REG_VALUE_08BIT		1
#define IMX317_REG_VALUE_16BIT		2
#define IMX317_REG_VALUE_24BIT		3

#define IMX317_LANES			2
#define IMX317_BITS_PER_SAMPLE		10

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

#define IMX317_NAME			"imx317"

static const char * const imx317_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define IMX317_NUM_SUPPLIES ARRAY_SIZE(imx317_supply_names)

struct regval {
	u16 addr;
	u8 val;
};

struct imx317_mode {
	u32 width;
	u32 height;
	u32 max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
};

struct imx317 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[IMX317_NUM_SUPPLIES];

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;
	bool			streaming;
	const struct imx317_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
};

#define to_imx317(sd) container_of(sd, struct imx317, subdev)

/*
 * Xclk 24Mhz
 */
static const struct regval imx317_global_regs[] = {
	{REG_NULL, 0x00},
};

/*
 * Xclk 24Mhz
 * max_framerate 30fps
 * mipi_datarate per lane 576Mbps
 * 2 lane
 */
static const struct regval imx317_1932x1094_regs[] = {
	{0x3000, 0x12},
	{0x303E, 0x03},
	{0x3120, 0xC0},
	{0x3121, 0x00},
	{0x3122, 0x02},
	{0x3123, 0x01},
	{0x3129, 0x9C},
	{0x312A, 0x02},
	{0x312D, 0x02},
	{0x3AC4, 0x01},
	{0x310B, 0x00},
	{0x30EE, 0x01},
	{0x3304, 0x32},
	{0x3306, 0x32},
	{0x3590, 0x32},
	{0x3686, 0x32},
	{0x3045, 0x32},
	{0x301A, 0x00},
	{0x304C, 0x00},
	{0x304D, 0x03},
	{0x331C, 0x1A},
	{0x3502, 0x02},
	{0x3529, 0x0E},
	{0x352A, 0x0E},
	{0x352B, 0x0E},
	{0x3538, 0x0E},
	{0x3539, 0x0E},
	{0x3553, 0x00},
	{0x357D, 0x05},
	{0x357F, 0x05},
	{0x3581, 0x04},
	{0x3583, 0x76},
	{0x3587, 0x01},
	{0x35BB, 0x0E},
	{0x35BC, 0x0E},
	{0x35BD, 0x0E},
	{0x35BE, 0x0E},
	{0x35BF, 0x0E},
	{0x366E, 0x00},
	{0x366F, 0x00},
	{0x3670, 0x00},
	{0x3671, 0x00},
	{0x3004, 0x02},
	{0x3005, 0x21},
	{0x3006, 0x00},
	{0x3007, 0x11},
	{0x300E, 0x00},
	{0x300F, 0x00},
	{0x3037, 0x00},
	{0x3038, 0x00},
	{0x3039, 0x00},
	{0x303A, 0x00},
	{0x303B, 0x00},
	{0x30DD, 0x00},
	{0x30DE, 0x00},
	{0x30DF, 0x00},
	{0x30E0, 0x00},
	{0x30E1, 0x00},
	{0x30E2, 0x02},

	{0x30F6, 0x70},
	{0x30F7, 0x02},
	{0x30F8, 0x0A},
	{0x30F9, 0x0F},
	{0x30FA, 0x00},

	{0x3130, 0x4E},
	{0x3131, 0x04},
	{0x3132, 0x46},
	{0x3133, 0x04},
	{0x3A54, 0x8C},
	{0x3A55, 0x07},
	{0x3342, 0x0A},
	{0x3343, 0x00},
	{0x3344, 0x1A},
	{0x3345, 0x00},
	{0x3528, 0x0E},
	{0x3554, 0x00},
	{0x3555, 0x01},
	{0x3556, 0x01},
	{0x3557, 0x01},
	{0x3558, 0x01},
	{0x3559, 0x00},
	{0x355A, 0x00},
	{0x35BA, 0x0E},
	{0x366A, 0x1B},
	{0x366B, 0x1A},
	{0x366C, 0x19},
	{0x366D, 0x17},
	{0x33A6, 0x01},
	{0x306B, 0x05},
	{0x3A41, 0x08},

	{0x3134, 0x5F},
	{0x3135, 0x00},
	{0x3136, 0x47},
	{0x3137, 0x00},
	{0x3138, 0x27},
	{0x3139, 0x00},
	{0x313A, 0x27},
	{0x313B, 0x00},
	{0x313C, 0x27},
	{0x313D, 0x00},
	{0x313E, 0x97},
	{0x313F, 0x00},
	{0x3140, 0x27},
	{0x3141, 0x00},
	{0x3142, 0x1F},
	{0x3143, 0x00},
	{0x3144, 0x0F},
	{0x3145, 0x00},
	{0x3A85, 0x03},
	{0x3A86, 0x47},
	{0x3A87, 0x00},
	{REG_DELAY, 0x0f},//delay 10ms

	{0x3000, 0x00},
	{0x303e, 0x03},
	{REG_DELAY, 0x0a},//delay 7ms

	{0x30f4, 0x00},
	{0x3018, 0xa2},
	{0x300c, 0x0c},
	{0x300d, 0x00},
	{0x312e, 0x01},//CSI_LANE_MODE//add
	{0x3aa2, 0x01},//PHYSICAL_LANE_NUM//add
	{0x3001, 0x16},
	{REG_NULL, 0x00},
};

static const struct imx317_mode supported_modes[] = {
	{
		.width = 1932,
		.height = 1094,
		.max_fps = 30,
		.exp_def = 0x000C,
		.hts_def = 0x0276,
		.vts_def = 0x0F0A,
		.reg_list = imx317_1932x1094_regs,
	},
};

static const s64 link_freq_menu_items[] = {
	IMX317_LINK_FREQ_288MHZ
};

static const char * const imx317_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

/* Write registers up to 4 at a time */
static int imx317_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

static int imx317_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		if (regs[i].addr == REG_DELAY) {
			usleep_range(regs[i].val, 2 * regs[i].val);
		} else {
			ret = imx317_write_reg(client, regs[i].addr,
				IMX317_REG_VALUE_08BIT, regs[i].val);
		}
	}

	return ret;
}

/* Read registers up to 4 at a time */
static int imx317_read_reg(struct i2c_client *client, u16 reg, unsigned int len,
			   u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

static int imx317_get_reso_dist(const struct imx317_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct imx317_mode *
imx317_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = imx317_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int imx317_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx317 *imx317 = to_imx317(sd);
	const struct imx317_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&imx317->mutex);

	mode = imx317_find_best_fit(fmt);
	fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx317->mutex);
		return -ENOTTY;
#endif
	} else {
		imx317->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(imx317->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(imx317->vblank, vblank_def,
					 IMX317_VTS_MAX - mode->height,
					 1, vblank_def);
	}

	mutex_unlock(&imx317->mutex);

	return 0;
}

static int imx317_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx317 *imx317 = to_imx317(sd);
	const struct imx317_mode *mode = imx317->cur_mode;

	mutex_lock(&imx317->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&imx317->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&imx317->mutex);

	return 0;
}

static int imx317_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int imx317_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int imx317_enable_test_pattern(struct imx317 *imx317, u32 pattern)
{
	return 0;
}

static int imx317_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx317 *imx317 = to_imx317(sd);
	const struct imx317_mode *mode = imx317->cur_mode;

	mutex_lock(&imx317->mutex);
	fi->interval.numerator = 10000;
	fi->interval.denominator = mode->max_fps * 10000;
	mutex_unlock(&imx317->mutex);

	return 0;
}

static void imx317_get_module_inf(struct imx317 *imx317,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, IMX317_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, imx317->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, imx317->len_name, sizeof(inf->base.lens));
}

static long imx317_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx317 *imx317 = to_imx317(sd);
	long ret = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		imx317_get_module_inf(imx317, (struct rkmodule_inf *)arg);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long imx317_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	long ret;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx317_ioctl(sd, cmd, inf);
		if (!ret)
			ret = copy_to_user(up, inf, sizeof(*inf));
		kfree(inf);
		break;
	case RKMODULE_AWB_CFG:
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}

		ret = copy_from_user(cfg, up, sizeof(*cfg));
		if (!ret)
			ret = imx317_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int __imx317_start_stream(struct imx317 *imx317)
{
	int ret;

	ret = imx317_write_array(imx317->client, imx317_global_regs);
	if (ret)
		return ret;

	ret = imx317_write_array(imx317->client, imx317->cur_mode->reg_list);
	if (ret)
		return ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&imx317->mutex);
	ret = v4l2_ctrl_handler_setup(&imx317->ctrl_handler);
	mutex_lock(&imx317->mutex);
	if (ret)
		return ret;

	return imx317_write_reg(imx317->client, IMX317_REG_CTRL_MODE,
				IMX317_REG_VALUE_08BIT, IMX317_MODE_STREAMING);
}

static int __imx317_stop_stream(struct imx317 *imx317)
{
	return imx317_write_reg(imx317->client, IMX317_REG_CTRL_MODE,
				IMX317_REG_VALUE_08BIT, IMX317_MODE_SW_STANDBY);
}

static int imx317_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx317 *imx317 = to_imx317(sd);
	struct i2c_client *client = imx317->client;
	int ret = 0;

	mutex_lock(&imx317->mutex);
	on = !!on;
	if (on == imx317->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __imx317_start_stream(imx317);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__imx317_stop_stream(imx317);
		pm_runtime_put(&client->dev);
	}

	imx317->streaming = on;

unlock_and_return:
	mutex_unlock(&imx317->mutex);

	return ret;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 imx317_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, IMX317_XVCLK_FREQ / 1000 / 1000);
}

static int __imx317_power_on(struct imx317 *imx317)
{
	int ret;
	u32 delay_us;
	struct device *dev = &imx317->client->dev;

	if (!IS_ERR_OR_NULL(imx317->pins_default)) {
		ret = pinctrl_select_state(imx317->pinctrl,
					   imx317->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	ret = clk_prepare_enable(imx317->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	if (!IS_ERR(imx317->reset_gpio))
		gpiod_set_value_cansleep(imx317->reset_gpio, 0);

	ret = regulator_bulk_enable(IMX317_NUM_SUPPLIES, imx317->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(imx317->reset_gpio))
		gpiod_set_value_cansleep(imx317->reset_gpio, 1);

	usleep_range(500, 1000);
	if (!IS_ERR(imx317->pwdn_gpio))
		gpiod_set_value_cansleep(imx317->pwdn_gpio, 1);

	/* 8192 cycles prior to first SCCB transaction */
	delay_us = imx317_cal_delay(8192);
	usleep_range(delay_us, delay_us * 2);

	return 0;

disable_clk:
	clk_disable_unprepare(imx317->xvclk);

	return ret;
}

static void __imx317_power_off(struct imx317 *imx317)
{
	int ret;
	struct device *dev = &imx317->client->dev;

	if (!IS_ERR(imx317->pwdn_gpio))
		gpiod_set_value_cansleep(imx317->pwdn_gpio, 0);
	clk_disable_unprepare(imx317->xvclk);
	if (!IS_ERR(imx317->reset_gpio))
		gpiod_set_value_cansleep(imx317->reset_gpio, 0);
	if (!IS_ERR_OR_NULL(imx317->pins_sleep)) {
		ret = pinctrl_select_state(imx317->pinctrl,
					   imx317->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	regulator_bulk_disable(IMX317_NUM_SUPPLIES, imx317->supplies);
}

static int imx317_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx317 *imx317 = to_imx317(sd);

	return __imx317_power_on(imx317);
}

static int imx317_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx317 *imx317 = to_imx317(sd);

	__imx317_power_off(imx317);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx317_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx317 *imx317 = to_imx317(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct imx317_mode *def_mode = &supported_modes[0];

	mutex_lock(&imx317->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx317->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static const struct dev_pm_ops imx317_pm_ops = {
	SET_RUNTIME_PM_OPS(imx317_runtime_suspend,
			   imx317_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx317_internal_ops = {
	.open = imx317_open,
};
#endif

static const struct v4l2_subdev_core_ops imx317_core_ops = {
	.ioctl = imx317_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx317_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx317_video_ops = {
	.s_stream = imx317_s_stream,
	.g_frame_interval = imx317_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx317_pad_ops = {
	.enum_mbus_code = imx317_enum_mbus_code,
	.enum_frame_size = imx317_enum_frame_sizes,
	.get_fmt = imx317_get_fmt,
	.set_fmt = imx317_set_fmt,
};

static const struct v4l2_subdev_ops imx317_subdev_ops = {
	.core	= &imx317_core_ops,
	.video	= &imx317_video_ops,
	.pad	= &imx317_pad_ops,
};

static int imx317_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx317 *imx317 = container_of(ctrl->handler,
					     struct imx317, ctrl_handler);
	struct i2c_client *client = imx317->client;
	s64 max;
	u32 val = 0;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = imx317->cur_mode->height + ctrl->val - 4;
		__v4l2_ctrl_modify_range(imx317->exposure,
					 imx317->exposure->minimum, max,
					 imx317->exposure->step,
					 imx317->exposure->default_value);
		break;
	}

	if (pm_runtime_get(&client->dev) <= 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		ret = imx317_write_reg(imx317->client,
				       IMX317_REG_EXPOSURE_H,
				       IMX317_REG_VALUE_08BIT,
				       (ctrl->val & 0xFF00) >> 8);
		ret |= imx317_write_reg(imx317->client,
					IMX317_REG_EXPOSURE_L,
					IMX317_REG_VALUE_08BIT,
					ctrl->val & 0xFF);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		val = 2048 * 2048;
		do_div(val, ctrl->val);
		if (val <= 2048)
			val = 2048 - val;
		else
			val = 0;

		if (val > 0x7A5)
			val = 0x7A5;

		ret = imx317_write_reg(imx317->client, IMX317_REG_GAIN_H,
				       IMX317_REG_VALUE_08BIT,
				       (val >> IMX317_GAIN_H_SHIFT) & IMX317_GAIN_H_MASK);
		ret |= imx317_write_reg(imx317->client, IMX317_REG_GAIN_L,
					IMX317_REG_VALUE_08BIT,
					val & IMX317_GAIN_L_MASK);
		break;
	case V4L2_CID_VBLANK:
		val = ctrl->val + imx317->cur_mode->height;
		ret = imx317_write_reg(imx317->client, IMX317_REG_VTS_H,
				       IMX317_REG_VALUE_08BIT,
				       (val & 0x0F0000) >> 16);
		ret |= imx317_write_reg(imx317->client, IMX317_REG_VTS_M,
					IMX317_REG_VALUE_08BIT,
					(val & 0x00FF00) >> 8);
		ret |= imx317_write_reg(imx317->client, IMX317_REG_VTS_L,
					IMX317_REG_VALUE_08BIT,
					val & 0x0000FF);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx317_enable_test_pattern(imx317, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx317_ctrl_ops = {
	.s_ctrl = imx317_set_ctrl,
};

static int imx317_initialize_controls(struct imx317 *imx317)
{
	const struct imx317_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &imx317->ctrl_handler;
	mode = imx317->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &imx317->mutex;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, IMX317_PIXEL_RATE, 1, IMX317_PIXEL_RATE);

	h_blank = mode->hts_def - mode->width;
	imx317->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (imx317->hblank)
		imx317->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	imx317->vblank = v4l2_ctrl_new_std(handler, &imx317_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				IMX317_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 4;
	imx317->exposure = v4l2_ctrl_new_std(handler, &imx317_ctrl_ops,
				V4L2_CID_EXPOSURE, IMX317_EXPOSURE_MIN,
				exposure_max, IMX317_EXPOSURE_STEP,
				mode->exp_def);

	imx317->anal_gain = v4l2_ctrl_new_std(handler, &imx317_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, IMX317_GAIN_MIN,
				IMX317_GAIN_MAX, IMX317_GAIN_STEP,
				IMX317_GAIN_DEFAULT);

	imx317->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&imx317_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(imx317_test_pattern_menu) - 1,
				0, 0, imx317_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(&imx317->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	imx317->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int imx317_check_sensor_id(struct imx317 *imx317,
				  struct i2c_client *client)
{
	struct device *dev = &imx317->client->dev;
	u32 id = 0;
	int ret;

	ret = imx317_read_reg(client, IMX317_REG_CHIP_ID,
			      IMX317_REG_VALUE_08BIT, &id);
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		return ret;
	}

	dev_info(dev, "Detected OV%06x sensor\n", CHIP_ID);

	return 0;
}

static int imx317_configure_regulators(struct imx317 *imx317)
{
	unsigned int i;

	for (i = 0; i < IMX317_NUM_SUPPLIES; i++)
		imx317->supplies[i].supply = imx317_supply_names[i];

	return devm_regulator_bulk_get(&imx317->client->dev,
				       IMX317_NUM_SUPPLIES,
				       imx317->supplies);
}

static int imx317_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx317 *imx317;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;

	imx317 = devm_kzalloc(dev, sizeof(*imx317), GFP_KERNEL);
	if (!imx317)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx317->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx317->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx317->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx317->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	imx317->client = client;
	imx317->cur_mode = &supported_modes[0];

	imx317->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx317->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}
	ret = clk_set_rate(imx317->xvclk, IMX317_XVCLK_FREQ);
	if (ret < 0) {
		dev_err(dev, "Failed to set xvclk rate (24MHz)\n");
		return ret;
	}
	if (clk_get_rate(imx317->xvclk) != IMX317_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");

	imx317->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx317->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	imx317->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(imx317->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	imx317->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx317->pinctrl)) {
		imx317->pins_default =
			pinctrl_lookup_state(imx317->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(imx317->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		imx317->pins_sleep =
			pinctrl_lookup_state(imx317->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(imx317->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	} else {
		dev_err(dev, "no pinctrl\n");
	}

	ret = imx317_configure_regulators(imx317);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	mutex_init(&imx317->mutex);

	sd = &imx317->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx317_subdev_ops);
	ret = imx317_initialize_controls(imx317);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx317_power_on(imx317);
	if (ret)
		goto err_free_handler;

	ret = imx317_check_sensor_id(imx317, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &imx317_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	imx317->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	ret = media_entity_init(&sd->entity, 1, &imx317->pad, 0);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx317->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx317->module_index, facing,
		 IMX317_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__imx317_power_off(imx317);
err_free_handler:
	v4l2_ctrl_handler_free(&imx317->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx317->mutex);

	return ret;
}

static int imx317_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx317 *imx317 = to_imx317(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx317->ctrl_handler);
	mutex_destroy(&imx317->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx317_power_off(imx317);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx317_of_match[] = {
	{ .compatible = "sony,imx317" },
	{},
};
MODULE_DEVICE_TABLE(of, imx317_of_match);
#endif

static const struct i2c_device_id imx317_match_id[] = {
	{ "sony,imx317", 0 },
	{ },
};

static struct i2c_driver imx317_i2c_driver = {
	.driver = {
		.name = IMX317_NAME,
		.pm = &imx317_pm_ops,
		.of_match_table = of_match_ptr(imx317_of_match),
	},
	.probe		= &imx317_probe,
	.remove		= &imx317_remove,
	.id_table	= imx317_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx317_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx317_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("OmniVision imx317 sensor driver");
MODULE_LICENSE("GPL v2");
