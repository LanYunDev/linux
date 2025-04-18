// SPDX-License-Identifier: GPL-2.0-only
/*
 * An I2C driver for the Intersil ISL 12022
 *
 * Author: Roman Fietze <roman.fietze@telemotive.de>
 *
 * Based on the Philips PCF8563 RTC
 * by Alessandro Zummo <a.zummo@towertech.it>.
 */

#include <linux/bcd.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>

#include <asm/byteorder.h>

/* RTC - Real time clock registers */
#define ISL12022_REG_SC		0x00
#define ISL12022_REG_MN		0x01
#define ISL12022_REG_HR		0x02
#define ISL12022_REG_DT		0x03
#define ISL12022_REG_MO		0x04
#define ISL12022_REG_YR		0x05
#define ISL12022_REG_DW		0x06

/* CSR - Control and status registers */
#define ISL12022_REG_SR		0x07
#define ISL12022_REG_INT	0x08
#define ISL12022_REG_PWR_VBAT	0x0a
#define ISL12022_REG_BETA	0x0d

/* ALARM - Alarm registers */
#define ISL12022_REG_SCA0	0x10
#define ISL12022_REG_MNA0	0x11
#define ISL12022_REG_HRA0	0x12
#define ISL12022_REG_DTA0	0x13
#define ISL12022_REG_MOA0	0x14
#define ISL12022_REG_DWA0	0x15
#define ISL12022_ALARM		ISL12022_REG_SCA0
#define ISL12022_ALARM_LEN	(ISL12022_REG_DWA0 - ISL12022_REG_SCA0 + 1)

/* TEMP - Temperature sensor registers */
#define ISL12022_REG_TEMP_L	0x28

/* ISL register bits */
#define ISL12022_HR_MIL		(1 << 7)	/* military or 24 hour time */

#define ISL12022_SR_ALM		(1 << 4)
#define ISL12022_SR_LBAT85	(1 << 2)
#define ISL12022_SR_LBAT75	(1 << 1)

#define ISL12022_INT_ARST	(1 << 7)
#define ISL12022_INT_WRTC	(1 << 6)
#define ISL12022_INT_IM		(1 << 5)
#define ISL12022_INT_FOBATB	(1 << 4)
#define ISL12022_INT_FO_MASK	GENMASK(3, 0)
#define ISL12022_INT_FO_OFF	0x0
#define ISL12022_INT_FO_32K	0x1

#define ISL12022_REG_VB85_MASK	GENMASK(5, 3)
#define ISL12022_REG_VB75_MASK	GENMASK(2, 0)

#define ISL12022_ALARM_ENABLE	(1 << 7)	/* for all ALARM registers  */

#define ISL12022_BETA_TSE	(1 << 7)

static struct i2c_driver isl12022_driver;

struct isl12022 {
	struct rtc_device *rtc;
	struct regmap *regmap;
	int irq;
	bool irq_enabled;
};

static umode_t isl12022_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type,
					 u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return 0444;

	return 0;
}

/*
 * A user-initiated temperature conversion is not started by this function,
 * so the temperature is updated once every ~60 seconds.
 */
static int isl12022_hwmon_read_temp(struct device *dev, long *mC)
{
	struct regmap *regmap = dev_get_drvdata(dev);
	int temp, ret;
	__le16 buf;

	ret = regmap_bulk_read(regmap, ISL12022_REG_TEMP_L, &buf, sizeof(buf));
	if (ret)
		return ret;
	/*
	 * Temperature is represented as a 10-bit number, unit half-Kelvins.
	 */
	temp = le16_to_cpu(buf);
	temp *= 500;
	temp -= 273000;

	*mC = temp;

	return 0;
}

static int isl12022_hwmon_read(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return isl12022_hwmon_read_temp(dev, val);

	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info * const isl12022_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops isl12022_hwmon_ops = {
	.is_visible = isl12022_hwmon_is_visible,
	.read = isl12022_hwmon_read,
};

static const struct hwmon_chip_info isl12022_hwmon_chip_info = {
	.ops = &isl12022_hwmon_ops,
	.info = isl12022_hwmon_info,
};

static void isl12022_hwmon_register(struct device *dev)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	struct device *hwmon;
	int ret;

	if (!IS_REACHABLE(CONFIG_HWMON))
		return;

	ret = regmap_update_bits(regmap, ISL12022_REG_BETA,
				 ISL12022_BETA_TSE, ISL12022_BETA_TSE);
	if (ret) {
		dev_warn(dev, "unable to enable temperature sensor\n");
		return;
	}

	hwmon = devm_hwmon_device_register_with_info(dev, "isl12022", regmap,
						     &isl12022_hwmon_chip_info,
						     NULL);
	if (IS_ERR(hwmon))
		dev_warn(dev, "unable to register hwmon device: %pe\n", hwmon);
}

/*
 * In the routines that deal directly with the isl12022 hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch.
 */
static int isl12022_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	u8 buf[ISL12022_REG_INT + 1];
	int ret;

	ret = regmap_bulk_read(regmap, ISL12022_REG_SC, buf, sizeof(buf));
	if (ret)
		return ret;

	dev_dbg(dev,
		"raw data is sec=%02x, min=%02x, hr=%02x, mday=%02x, mon=%02x, year=%02x, wday=%02x, sr=%02x, int=%02x",
		buf[ISL12022_REG_SC],
		buf[ISL12022_REG_MN],
		buf[ISL12022_REG_HR],
		buf[ISL12022_REG_DT],
		buf[ISL12022_REG_MO],
		buf[ISL12022_REG_YR],
		buf[ISL12022_REG_DW],
		buf[ISL12022_REG_SR],
		buf[ISL12022_REG_INT]);

	tm->tm_sec = bcd2bin(buf[ISL12022_REG_SC] & 0x7F);
	tm->tm_min = bcd2bin(buf[ISL12022_REG_MN] & 0x7F);
	tm->tm_hour = bcd2bin(buf[ISL12022_REG_HR] & 0x3F);
	tm->tm_mday = bcd2bin(buf[ISL12022_REG_DT] & 0x3F);
	tm->tm_wday = buf[ISL12022_REG_DW] & 0x07;
	tm->tm_mon = bcd2bin(buf[ISL12022_REG_MO] & 0x1F) - 1;
	tm->tm_year = bcd2bin(buf[ISL12022_REG_YR]) + 100;

	dev_dbg(dev, "%s: %ptR\n", __func__, tm);

	return 0;
}

static int isl12022_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	int ret;
	u8 buf[ISL12022_REG_DW + 1];

	dev_dbg(dev, "%s: %ptR\n", __func__, tm);

	/* Ensure the write enable bit is set. */
	ret = regmap_update_bits(regmap, ISL12022_REG_INT,
				 ISL12022_INT_WRTC, ISL12022_INT_WRTC);
	if (ret)
		return ret;

	/* hours, minutes and seconds */
	buf[ISL12022_REG_SC] = bin2bcd(tm->tm_sec);
	buf[ISL12022_REG_MN] = bin2bcd(tm->tm_min);
	buf[ISL12022_REG_HR] = bin2bcd(tm->tm_hour) | ISL12022_HR_MIL;

	buf[ISL12022_REG_DT] = bin2bcd(tm->tm_mday);

	/* month, 1 - 12 */
	buf[ISL12022_REG_MO] = bin2bcd(tm->tm_mon + 1);

	/* year and century */
	buf[ISL12022_REG_YR] = bin2bcd(tm->tm_year % 100);

	buf[ISL12022_REG_DW] = tm->tm_wday & 0x07;

	return regmap_bulk_write(regmap, ISL12022_REG_SC, buf, sizeof(buf));
}

static int isl12022_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct rtc_time *tm = &alarm->time;
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	u8 buf[ISL12022_ALARM_LEN];
	unsigned int i, yr;
	int ret;

	ret = regmap_bulk_read(regmap, ISL12022_ALARM, buf, sizeof(buf));
	if (ret) {
		dev_dbg(dev, "%s: reading ALARM registers failed\n",
			__func__);
		return ret;
	}

	/* The alarm doesn't store the year so get it from the rtc section */
	ret = regmap_read(regmap, ISL12022_REG_YR, &yr);
	if (ret) {
		dev_dbg(dev, "%s: reading YR register failed\n", __func__);
		return ret;
	}

	dev_dbg(dev,
		"%s: sc=%02x, mn=%02x, hr=%02x, dt=%02x, mo=%02x, dw=%02x yr=%u\n",
		__func__, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], yr);

	tm->tm_sec  = bcd2bin(buf[ISL12022_REG_SCA0 - ISL12022_ALARM] & 0x7F);
	tm->tm_min  = bcd2bin(buf[ISL12022_REG_MNA0 - ISL12022_ALARM] & 0x7F);
	tm->tm_hour = bcd2bin(buf[ISL12022_REG_HRA0 - ISL12022_ALARM] & 0x3F);
	tm->tm_mday = bcd2bin(buf[ISL12022_REG_DTA0 - ISL12022_ALARM] & 0x3F);
	tm->tm_mon  = bcd2bin(buf[ISL12022_REG_MOA0 - ISL12022_ALARM] & 0x1F) - 1;
	tm->tm_wday = buf[ISL12022_REG_DWA0 - ISL12022_ALARM]         & 0x07;
	tm->tm_year = bcd2bin(yr) + 100;

	for (i = 0; i < ISL12022_ALARM_LEN; i++) {
		if (buf[i] & ISL12022_ALARM_ENABLE) {
			alarm->enabled = 1;
			break;
		}
	}

	dev_dbg(dev, "%s: %ptR\n", __func__, tm);

	return 0;
}

static int isl12022_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct rtc_time *alarm_tm = &alarm->time;
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	u8 regs[ISL12022_ALARM_LEN] = { 0, };
	struct rtc_time rtc_tm;
	int ret, enable, dw;

	ret = isl12022_rtc_read_time(dev, &rtc_tm);
	if (ret)
		return ret;

	/* If the alarm time is before the current time disable the alarm */
	if (!alarm->enabled || rtc_tm_sub(alarm_tm, &rtc_tm) <= 0)
		enable = 0;
	else
		enable = ISL12022_ALARM_ENABLE;

	/*
	 * Set non-matching day of the week to safeguard against early false
	 * matching while setting all the alarm registers (this rtc lacks a
	 * general alarm/irq enable/disable bit).
	 */
	ret = regmap_read(regmap, ISL12022_REG_DW, &dw);
	if (ret) {
		dev_dbg(dev, "%s: reading DW failed\n", __func__);
		return ret;
	}
	/* ~4 days into the future should be enough to avoid match */
	dw = ((dw + 4) % 7) | ISL12022_ALARM_ENABLE;
	ret = regmap_write(regmap, ISL12022_REG_DWA0, dw);
	if (ret) {
		dev_dbg(dev, "%s: writing DWA0 failed\n", __func__);
		return ret;
	}

	/* Program the alarm and enable it for each setting */
	regs[ISL12022_REG_SCA0 - ISL12022_ALARM] = bin2bcd(alarm_tm->tm_sec) | enable;
	regs[ISL12022_REG_MNA0 - ISL12022_ALARM] = bin2bcd(alarm_tm->tm_min) | enable;
	regs[ISL12022_REG_HRA0 - ISL12022_ALARM] = bin2bcd(alarm_tm->tm_hour) | enable;
	regs[ISL12022_REG_DTA0 - ISL12022_ALARM] = bin2bcd(alarm_tm->tm_mday) | enable;
	regs[ISL12022_REG_MOA0 - ISL12022_ALARM] = bin2bcd(alarm_tm->tm_mon + 1) | enable;
	regs[ISL12022_REG_DWA0 - ISL12022_ALARM] = bin2bcd(alarm_tm->tm_wday & 7) | enable;

	/* write ALARM registers */
	ret = regmap_bulk_write(regmap, ISL12022_ALARM, &regs, sizeof(regs));
	if (ret) {
		dev_dbg(dev, "%s: writing ALARM registers failed\n", __func__);
		return ret;
	}

	return 0;
}

static irqreturn_t isl12022_rtc_interrupt(int irq, void *data)
{
	struct isl12022 *isl12022 = data;
	struct rtc_device *rtc = isl12022->rtc;
	struct device *dev = &rtc->dev;
	struct regmap *regmap = isl12022->regmap;
	u32 val = 0;
	unsigned long events = 0;
	int ret;

	ret = regmap_read(regmap, ISL12022_REG_SR, &val);
	if (ret) {
		dev_dbg(dev, "%s: reading SR failed\n", __func__);
		return IRQ_HANDLED;
	}

	if (val & ISL12022_SR_ALM)
		events |= RTC_IRQF | RTC_AF;

	if (events & RTC_AF)
		dev_dbg(dev, "alarm!\n");

	if (!events)
		return IRQ_NONE;

	rtc_update_irq(rtc, 1, events);
	return IRQ_HANDLED;
}

static int isl12022_rtc_alarm_irq_enable(struct device *dev,
					 unsigned int enabled)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);

	/* Make sure enabled is 0 or 1 */
	enabled = !!enabled;

	if (isl12022->irq_enabled == enabled)
		return 0;

	if (enabled)
		enable_irq(isl12022->irq);
	else
		disable_irq(isl12022->irq);

	isl12022->irq_enabled = enabled;

	return 0;
}

static int isl12022_setup_irq(struct device *dev, int irq)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	unsigned int reg_mask, reg_val;
	u8 buf[ISL12022_ALARM_LEN] = { 0, };
	int ret;

	/* Clear and disable all alarm registers */
	ret = regmap_bulk_write(regmap, ISL12022_ALARM, buf, sizeof(buf));
	if (ret)
		return ret;

	/*
	 * Enable automatic reset of ALM bit and enable single event interrupt
	 * mode.
	 */
	reg_mask = ISL12022_INT_ARST | ISL12022_INT_IM | ISL12022_INT_FO_MASK;
	reg_val = ISL12022_INT_ARST | ISL12022_INT_FO_OFF;
	ret = regmap_write_bits(regmap, ISL12022_REG_INT,
				reg_mask, reg_val);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, irq, NULL,
					isl12022_rtc_interrupt,
					IRQF_SHARED | IRQF_ONESHOT,
					isl12022_driver.driver.name,
					isl12022);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to request irq %d\n", irq);

	isl12022->irq = irq;
	return 0;
}

static int isl12022_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	u32 user, val;
	int ret;

	switch (cmd) {
	case RTC_VL_READ:
		ret = regmap_read(regmap, ISL12022_REG_SR, &val);
		if (ret)
			return ret;

		user = 0;
		if (val & ISL12022_SR_LBAT85)
			user |= RTC_VL_BACKUP_LOW;

		if (val & ISL12022_SR_LBAT75)
			user |= RTC_VL_BACKUP_EMPTY;

		return put_user(user, (u32 __user *)arg);

	default:
		return -ENOIOCTLCMD;
	}
}

static const struct rtc_class_ops isl12022_rtc_ops = {
	.ioctl		= isl12022_rtc_ioctl,
	.read_time	= isl12022_rtc_read_time,
	.set_time	= isl12022_rtc_set_time,
	.read_alarm	= isl12022_rtc_read_alarm,
	.set_alarm	= isl12022_rtc_set_alarm,
	.alarm_irq_enable = isl12022_rtc_alarm_irq_enable,
};

static const struct regmap_config regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.use_single_write = true,
};

static int isl12022_register_clock(struct device *dev)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	struct clk_hw *hw;
	int ret;

	if (!device_property_present(dev, "#clock-cells")) {
		/*
		 * Disabling the F_OUT pin reduces the power
		 * consumption in battery mode by ~25%.
		 */
		regmap_update_bits(regmap, ISL12022_REG_INT, ISL12022_INT_FO_MASK,
				   ISL12022_INT_FO_OFF);

		return 0;
	}

	if (!IS_ENABLED(CONFIG_COMMON_CLK))
		return 0;

	/*
	 * For now, only support a fixed clock of 32768Hz (the reset default).
	 */
	ret = regmap_update_bits(regmap, ISL12022_REG_INT,
				 ISL12022_INT_FO_MASK, ISL12022_INT_FO_32K);
	if (ret)
		return ret;

	hw = devm_clk_hw_register_fixed_rate(dev, "isl12022", NULL, 0, 32768);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, hw);
}

static const u32 trip_levels[2][7] = {
	{ 2125000, 2295000, 2550000, 2805000, 3060000, 4250000, 4675000 },
	{ 1875000, 2025000, 2250000, 2475000, 2700000, 3750000, 4125000 },
};

static void isl12022_set_trip_levels(struct device *dev)
{
	struct isl12022 *isl12022 = dev_get_drvdata(dev);
	struct regmap *regmap = isl12022->regmap;
	u32 levels[2] = {0, 0};
	int ret, i, j, x[2];
	u8 val, mask;

	device_property_read_u32_array(dev, "isil,battery-trip-levels-microvolt",
				       levels, 2);

	for (i = 0; i < 2; i++) {
		for (j = 0; j < ARRAY_SIZE(trip_levels[i]) - 1; j++) {
			if (levels[i] <= trip_levels[i][j])
				break;
		}
		x[i] = j;
	}

	val = FIELD_PREP(ISL12022_REG_VB85_MASK, x[0]) |
		FIELD_PREP(ISL12022_REG_VB75_MASK, x[1]);
	mask = ISL12022_REG_VB85_MASK | ISL12022_REG_VB75_MASK;

	ret = regmap_update_bits(regmap, ISL12022_REG_PWR_VBAT, mask, val);
	if (ret)
		dev_warn(dev, "unable to set battery alarm levels: %d\n", ret);

	/*
	 * Force a write of the TSE bit in the BETA register, in order
	 * to trigger an update of the LBAT75 and LBAT85 bits in the
	 * status register. In battery backup mode, those bits have
	 * another meaning, so without this, they may contain stale
	 * values for up to a minute after power-on.
	 */
	regmap_write_bits(regmap, ISL12022_REG_BETA,
			  ISL12022_BETA_TSE, ISL12022_BETA_TSE);
}

static int isl12022_probe(struct i2c_client *client)
{
	struct isl12022 *isl12022;
	struct rtc_device *rtc;
	struct regmap *regmap;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	/* Allocate driver state */
	isl12022 = devm_kzalloc(&client->dev, sizeof(*isl12022), GFP_KERNEL);
	if (!isl12022)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap), "regmap allocation failed\n");
	isl12022->regmap = regmap;

	dev_set_drvdata(&client->dev, isl12022);

	ret = isl12022_register_clock(&client->dev);
	if (ret)
		return ret;

	isl12022_set_trip_levels(&client->dev);
	isl12022_hwmon_register(&client->dev);

	rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);
	isl12022->rtc = rtc;

	rtc->ops = &isl12022_rtc_ops;
	rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc->range_max = RTC_TIMESTAMP_END_2099;

	if (client->irq > 0) {
		ret = isl12022_setup_irq(&client->dev, client->irq);
		if (ret)
			return ret;
	} else {
		clear_bit(RTC_FEATURE_ALARM, rtc->features);
	}

	return devm_rtc_register_device(rtc);
}

static const struct of_device_id isl12022_dt_match[] = {
	{ .compatible = "isl,isl12022" }, /* for backward compat., don't use */
	{ .compatible = "isil,isl12022" },
	{ },
};
MODULE_DEVICE_TABLE(of, isl12022_dt_match);

static const struct i2c_device_id isl12022_id[] = {
	{ "isl12022" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, isl12022_id);

static struct i2c_driver isl12022_driver = {
	.driver		= {
		.name	= "rtc-isl12022",
		.of_match_table = isl12022_dt_match,
	},
	.probe		= isl12022_probe,
	.id_table	= isl12022_id,
};

module_i2c_driver(isl12022_driver);

MODULE_AUTHOR("roman.fietze@telemotive.de");
MODULE_DESCRIPTION("ISL 12022 RTC driver");
MODULE_LICENSE("GPL");
