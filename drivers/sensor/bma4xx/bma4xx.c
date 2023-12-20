/* Bosch BMA4xx 3-axis accelerometer driver
 *
 * Copyright (c) 2023 Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT bosch_bma4xx

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(bma4xx, CONFIG_SENSOR_LOG_LEVEL);
#include "bma4xx.h"

/**
 * @brief Helper for converting m/s^2 offset values into register values
 */
static int bma4xx_offset_to_reg_val(const struct sensor_value *val, uint8_t *reg_val)
{
	int32_t ug = sensor_ms2_to_ug(val);

	if (ug < BMA4XX_OFFSET_MICROG_MIN || ug > BMA4XX_OFFSET_MICROG_MAX) {
		return -ERANGE;
	}

	*reg_val = ug / BMA4XX_OFFSET_MICROG_PER_BIT;
	return 0;
}

/**
 * @brief Set the X, Y, or Z axis offsets.
 */
static int bma4xx_attr_set_offset(const struct device *dev, enum sensor_channel chan,
				  const struct sensor_value *val)
{
	struct bma4xx_data *bma4xx = dev->data;
	uint8_t reg_addr;
	uint8_t reg_val[3];
	int rc;

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
		reg_addr = BMA4XX_REG_OFFSET_0 + (chan - SENSOR_CHAN_ACCEL_X);
		rc = bma4xx_offset_to_reg_val(val, &reg_val[0]);
		if (rc) {
			return rc;
		}
		return bma4xx->hw_ops->write_reg(dev, reg_addr, reg_val[0]);
	case SENSOR_CHAN_ACCEL_XYZ:
		/* Expect val to point to an array of three sensor_values */
		reg_addr = BMA4XX_REG_OFFSET_0;
		for (int i = 0; i < 3; i++) {
			rc = bma4xx_offset_to_reg_val(&val[i], &reg_val[i]);
			if (rc) {
				return rc;
			}
		}
		return bma4xx->hw_ops->write_data(dev, reg_addr, (uint8_t *)reg_val,
						  sizeof(reg_val));
	default:
		return -ENOTSUP;
	}
}

static const uint32_t odr_to_reg_map[] = {
	0,          /* Invalid */
	781250,     /* 0.78125 Hz (25/32) => 0x1 */
	1562500,    /* 1.5625 Hz (25/16) => 0x2 */
	3125000,    /* 3.125 Hz (25/8) => 0x3 */
	6250000,    /* 6.25 Hz (25/4) => 0x4 */
	12500000,   /* 12.5 Hz (25/2) => 0x5 */
	25000000,   /* 25 Hz => 0x6 */
	50000000,   /* 50 Hz => 0x7*/
	100000000,  /* 100 Hz => 0x8*/
	200000000,  /* 200 Hz => 0x9*/
	400000000,  /* 400 Hz => 0xa*/
	800000000,  /* 800 Hz => 0xb*/
	1600000000, /* 1600 Hz => 0xc*/
};

/**
 * @brief Convert an ODR rate in Hz to a register value
 */
static int bma4xx_odr_to_reg(uint32_t microhertz, uint8_t *reg_val)
{
	if (microhertz == 0) {
		/* Illegal ODR value */
		return -ERANGE;
	}

	for (uint8_t i = 0; i < ARRAY_SIZE(odr_to_reg_map); i++) {
		if (microhertz <= odr_to_reg_map[i]) {
			*reg_val = i;
			return 0;
		}
	}

	/* Requested ODR too high */
	return -ERANGE;
}

/**
 * Set the sensor's acceleration offset (per axis). Use bma4xx_commit_nvm() to save these
 * offsets to nonvolatile memory so they are automatically set during power-on-reset.
 */
static int bma4xx_attr_set_odr(const struct device *dev, const struct sensor_value *val)
{
	struct bma4xx_data *bma4xx = dev->data;
	int status;
	uint8_t reg_val;

	/* Convert ODR Hz value to microhertz and round up to closest register setting */
	status = bma4xx_odr_to_reg(val->val1 * 1000000 + val->val2, &reg_val);
	if (status < 0) {
		return status;
	}

	status = bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_ACCEL_CONFIG, BMA4XX_MASK_ACC_CONF_ODR,
					    reg_val);
	if (status < 0) {
		return status;
	}

	bma4xx->accel_odr = reg_val;
	return 0;
}

static const uint32_t fs_to_reg_map[] = {
	2000000,  /* +/-2G => 0x0 */
	4000000,  /* +/-4G => 0x1 */
	8000000,  /* +/-8G => 0x2 */
	16000000, /* +/-16G => 0x3 */
};

static int bma4xx_fs_to_reg(int32_t range_ug, uint8_t *reg_val)
{
	if (range_ug == 0) {
		/* Illegal value */
		return -ERANGE;
	}

	range_ug = abs(range_ug);

	for (uint8_t i = 0; i < 4; i++) {
		if (range_ug <= fs_to_reg_map[i]) {
			*reg_val = i;
			return 0;
		}
	}

	/* Requested range too high */
	return -ERANGE;
}

/**
 * Set the sensor's full-scale range
 */
static int bma4xx_attr_set_range(const struct device *dev, const struct sensor_value *val)
{
	struct bma4xx_data *bma4xx = dev->data;
	int status;
	uint8_t reg_val;

	/* Convert m/s^2 to micro-G's and find closest register setting */
	status = bma4xx_fs_to_reg(sensor_ms2_to_ug(val), &reg_val);
	if (status < 0) {
		return status;
	}

	status = bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_ACCEL_RANGE, BMA4XX_MASK_ACC_RANGE,
					    reg_val);
	if (status < 0) {
		return status;
	}

	bma4xx->accel_fs_range = reg_val;
	return 0;
}

/**
 * Set the sensor's bandwidth parameter (one of BMA4XX_BWP_*)
 */
static int bma4xx_attr_set_bwp(const struct device *dev, const struct sensor_value *val)
{
	/* Require that `val2` is unused, and that `val1` is in range of a valid BWP */
	if (val->val2 || val->val1 < BMA4XX_BWP_OSR4_AVG1 || val->val1 > BMA4XX_BWP_RES_AVG128) {
		return -EINVAL;
	}

	struct bma4xx_data *bma4xx = dev->data;

	return bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_ACCEL_CONFIG, BMA4XX_MASK_ACC_CONF_BWP,
					  (((uint8_t)val->val1) << BMA4XX_SHIFT_ACC_CONF_BWP));
}

/**
 * @brief Implement the sensor API attribute set method.
 */
static int bma4xx_attr_set(const struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr, const struct sensor_value *val)
{
	switch (attr) {
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		return bma4xx_attr_set_odr(dev, val);
	case SENSOR_ATTR_FULL_SCALE:
		return bma4xx_attr_set_range(dev, val);
	case SENSOR_ATTR_OFFSET:
		return bma4xx_attr_set_offset(dev, chan, val);
	case SENSOR_ATTR_CONFIGURATION:
		/* Use for setting the bandwidth parameter (BWP) */
		return bma4xx_attr_set_bwp(dev, val);
	default:
		return -ENOTSUP;
	}
}

/**
 * Internal device initialization function for both bus types.
 */
static int bma4xx_chip_init(const struct device *dev)
{
	struct bma4xx_data *bma4xx = dev->data;
	const struct bma4xx_config *cfg = dev->config;
	int status;

	/* Sensor bus-specific initialization */
	status = cfg->bus_init(dev);
	if (status) {
		LOG_ERR("bus_init failed: %d", status);
		return status;
	}

	/* Read Chip ID */
	status = bma4xx->hw_ops->read_reg(dev, BMA4XX_REG_CHIP_ID, &bma4xx->chip_id);
	if (status) {
		LOG_ERR("could not read chip_id: %d", status);
		return status;
	}
	LOG_DBG("chip_id is 0x%02x", bma4xx->chip_id);

	if (bma4xx->chip_id != BMA4XX_CHIP_ID_BMA422) {
		LOG_WRN("Driver tested for BMA422. Check for unintended operation.");
	}

	/* Issue soft reset command */
	status = bma4xx->hw_ops->write_reg(dev, BMA4XX_REG_CMD, BMA4XX_CMD_SOFT_RESET);
	if (status) {
		LOG_ERR("Could not soft-reset chip: %d", status);
		return status;
	}

	k_sleep(K_USEC(1000));

	/* Default is: range = +/-4G, ODR = 100 Hz, BWP = "NORM_AVG4" */
	bma4xx->accel_fs_range = BMA4XX_RANGE_4G;
	bma4xx->accel_bwp = BMA4XX_BWP_NORM_AVG4;
	bma4xx->accel_odr = BMA4XX_ODR_100;

	/* Switch to performance power mode */
	status = bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_ACCEL_CONFIG, BMA4XX_BIT_ACC_PERF_MODE,
					    BMA4XX_BIT_ACC_PERF_MODE);
	if (status) {
		LOG_ERR("Could not enable performance power save mode: %d", status);
		return status;
	}

	/* Enable accelerometer */
	status = bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_POWER_CTRL, BMA4XX_BIT_ACC_EN,
					    BMA4XX_BIT_ACC_EN);
	if (status) {
		LOG_ERR("Could not enable accel: %d", status);
		return status;
	}

	return status;
}

/*
 * Sample fetch and conversion
 */

/**
 * @brief Read accelerometer data from the BMA4xx
 */
static int bma4xx_sample_fetch(const struct device *dev, int16_t *x, int16_t *y, int16_t *z)
{
	struct bma4xx_data *bma4xx = dev->data;
	uint8_t read_data[6];
	int status;

	/* Burst read regs DATA_8 through DATA_13, which holds the accel readings */
	status = bma4xx->hw_ops->read_data(dev, BMA4XX_REG_DATA_8, (uint8_t *)&read_data,
					   BMA4XX_REG_DATA_13 - BMA4XX_REG_DATA_8 + 1);
	if (status < 0) {
		LOG_ERR("Cannot read accel data: %d", status);
		return status;
	}

	/* Values in accel_data[N] are left-aligned and will read 16x actual */
	*x = (read_data[1] << 8) | (read_data[0] & 0xF0);
	*y = (read_data[3] << 8) | (read_data[2] & 0xF0);
	*z = (read_data[5] << 8) | (read_data[4] & 0xF0);

	LOG_DBG("XYZ reg vals are %d, %d, %d", *x, *y, *z);

	return 0;
}

#ifdef CONFIG_BMA4XX_TEMPERATURE
/**
 * @brief Read temperature register on BMA4xx
 */
static int bma4xx_temp_fetch(const struct device *dev, int8_t *temp)
{
	struct bma4xx_data *bma4xx = dev->data;
	int status;

	status = bma4xx->hw_ops->read_reg(dev, BMA4XX_REG_TEMPERATURE, temp);
	if (status) {
		LOG_ERR("could not read temp reg: %d", status);
		return status;
	}

	LOG_DBG("temp reg val is %d", *temp);
	return 0;
}
#endif

/*
 * RTIO submit and encoding
 */

static int bma4xx_submit_one_shot(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe)
{
	struct bma4xx_data *bma4xx = dev->data;

	const struct sensor_read_config *cfg = iodev_sqe->sqe.iodev->data;
	const enum sensor_channel *const channels = cfg->channels;
	const size_t num_channels = cfg->count;

	uint32_t min_buf_len = sizeof(struct bma4xx_encoded_data);
	struct bma4xx_encoded_data *edata;
	uint8_t *buf;
	uint32_t buf_len;
	int rc;

	/* Get the buffer for the frame, it may be allocated dynamically by the rtio context */
	rc = rtio_sqe_rx_buf(iodev_sqe, min_buf_len, min_buf_len, &buf, &buf_len);
	if (rc != 0) {
		LOG_ERR("Failed to get a read buffer of size %u bytes", min_buf_len);
		rtio_iodev_sqe_err(iodev_sqe, rc);
		return rc;
	}

	/* Prepare response */
	edata = (struct bma4xx_encoded_data *)buf;
	edata->header.is_fifo = false;
	edata->header.accel_fs = bma4xx->accel_fs_range;
	edata->header.timestamp = k_ticks_to_ns_floor64(k_uptime_ticks());
	edata->has_accel = 0;
	edata->has_temp = 0;

	/* Determine what channels we need to fetch */
	for (int i = 0; i < num_channels; i++) {
		switch (channels[i]) {
		case SENSOR_CHAN_ALL:
			edata->has_accel = 1;
#ifdef CONFIG_BMA4XX_TEMPERATURE
			edata->has_temp = 1;
#endif /* CONFIG_BMA4XX_TEMPERATURE */
			break;
		case SENSOR_CHAN_ACCEL_X:
		case SENSOR_CHAN_ACCEL_Y:
		case SENSOR_CHAN_ACCEL_Z:
		case SENSOR_CHAN_ACCEL_XYZ:
			edata->has_accel = 1;
			break;
#ifdef CONFIG_BMA4XX_TEMPERATURE
		case SENSOR_CHAN_DIE_TEMP:
			edata->has_temp = 1;
			break;
#endif /* CONFIG_BMA4XX_TEMPERATURE */
		default:
			LOG_ERR("Requested unsupported channel ID %d", channels[i]);
			return -ENOTSUP;
		}
	}

	if (edata->has_accel) {
		rc = bma4xx_sample_fetch(dev, &edata->accel_xyz[0], &edata->accel_xyz[1],
					 &edata->accel_xyz[2]);
		if (rc != 0) {
			LOG_ERR("Failed to fetch accel samples");
			rtio_iodev_sqe_err(iodev_sqe, rc);
			return rc;
		}
	}

#ifdef CONFIG_BMA4XX_TEMPERATURE
	if (edata->has_temp) {
		rc = bma4xx_temp_fetch(dev, &edata->temp);
		if (rc != 0) {
			LOG_ERR("Failed to fetch temp sample");
			rtio_iodev_sqe_err(iodev_sqe, rc);
			return rc;
		}
	}
#endif /* CONFIG_BMA4XX_TEMPERATURE */

	rtio_iodev_sqe_ok(iodev_sqe, 0);

	return 0;
}

static int bma4xx_submit(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe)
{
	const struct sensor_read_config *cfg = iodev_sqe->sqe.iodev->data;

	if (!cfg->is_streaming) {
		return bma4xx_submit_one_shot(dev, iodev_sqe);
	}
	/* TODO: Add streaming support */

	return -ENOTSUP;
}

/*
 * RTIO streaming / interrupt support
 */

#ifdef CONFIG_BMA4XX_STREAMING

static void bma4xx_lock(const struct device *dev)
{
	struct bma4xx_data *data = dev->data;

	k_mutex_lock(&data->mutex, K_FOREVER);
}

static void bma4xx_unlock(const struct device *dev)
{
	struct bma4xx_data *data = dev->data;

	k_mutex_unlock(&data->mutex);
}

static void bma4xx_work_handler(struct k_work *work)
{
	struct bma4xx_data *data = CONTAINER_OF(work, struct bma4xx_data, work);

	bma4xx_lock(data->dev);

	if (data->data_ready_handler != NULL) {
		data->data_ready_handler(data->dev, data->data_ready_trigger);
	}

	bma4xx_unlock(data->dev);
}

static void bma4xx_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct bma4xx_data *data = CONTAINER_OF(cb, struct bma4xx_data, gpio_cb);

	ARG_UNUSED(dev);
	ARG_UNUSED(pins);

	k_work_submit(&data->work);

	icm42688_fifo_event(data->dev);
}

static int bma4xx_interrupt_init(const struct device *dev)
{
	struct bma4xx_data *data = dev->data;
	const struct bma4xx_config *cfg = dev->config;
	int res = 0;

	if (!cfg->gpio_int1.port) {
		LOG_ERR("Streaming enabled but no interrupt gpio supplied");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->gpio_int1)) {
		LOG_ERR("GPIO for interrupt not ready");
		return -ENODEV;
	}

	data->dev = dev;
	gpio_pin_configure_dt(&cfg->gpio_int1, GPIO_INPUT);
	gpio_init_callback(&data->gpio_cb, bma4xx_gpio_callback, BIT(cfg->gpio_int1.pin));
	res = gpio_add_callback(cfg->gpio_int1.port, &data->gpio_cb);

	if (res < 0) {
		LOG_ERR("Failed to set gpio callback: %d", res);
		return res;
	}

	k_mutex_init(&data->mutex);

	data->work.handler = icm42688_work_handler;

	return gpio_pin_interrupt_configure_dt(&cfg->gpio_int1, GPIO_INT_EDGE_TO_ACTIVE);
}

static int bma4xx_trigger_update_interrupts(const struct device *dev, uint8_t interrupts)
{
	struct bma4xx_data *data = dev->data;
	int res;

	/* Turn on the interrupt output */
	res = bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_INT1_IO_CTRL, BMA4XX_BIT_INT1_OUT_EN,
					 BMA4XX_BIT_INT1_OUT_EN);
	if (res != 0) {
		return res;
	}

	/* Enable chosen interrupts on INT1 pin */
	return bma4xx->hw_ops->update_reg(dev, BMA4XX_REG_INT_MAP_DATA, BMA4XX_MASK_INT1,
					  interrupts);
}

#endif /* CONFIG_BMA4XX_STREAMING */

/*
 * RTIO 
 */

/*
 * RTIO decoder
 */

static int bma4xx_decoder_get_frame_count(const uint8_t *buffer, enum sensor_channel channel,
					  size_t channel_idx, uint16_t *frame_count)
{
	const struct bma4xx_encoded_data *edata = (const struct bma4xx_encoded_data *)buffer;
	const struct bma4xx_decoder_header *header = &edata->header;

	if (channel_idx != 0) {
		return -ENOTSUP;
	}

	if (!header->is_fifo) {
		switch (channel) {
		case SENSOR_CHAN_ACCEL_X:
		case SENSOR_CHAN_ACCEL_Y:
		case SENSOR_CHAN_ACCEL_Z:
		case SENSOR_CHAN_ACCEL_XYZ:
			*frame_count = edata->has_accel ? 1 : 0;
			return 0;
		case SENSOR_CHAN_DIE_TEMP:
			*frame_count = edata->has_temp ? 1 : 0;
			return 0;
		default:
			return -ENOTSUP;
		}
		return 0;
	}

	/* FIFO (streaming) mode operation is not yet supported */
	return -ENOTSUP;
}

static int bma4xx_decoder_get_size_info(enum sensor_channel channel, size_t *base_size,
					size_t *frame_size)
{
	switch (channel) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_XYZ:
		*base_size = sizeof(struct sensor_three_axis_data);
		*frame_size = sizeof(struct sensor_three_axis_sample_data);
		return 0;
	case SENSOR_CHAN_DIE_TEMP:
		*base_size = sizeof(struct sensor_q31_data);
		*frame_size = sizeof(struct sensor_q31_sample_data);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int bma4xx_get_shift(enum sensor_channel channel, uint8_t accel_fs, int8_t *shift)
{
	switch (channel) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_XYZ:
		switch (accel_fs) {
		case BMA4XX_RANGE_2G:
			/* 2 G's = 19.62 m/s^2. Use shift of 5 (+/-32) */
			*shift = 5;
			return 0;
		case BMA4XX_RANGE_4G:
			*shift = 6;
			return 0;
		case BMA4XX_RANGE_8G:
			*shift = 7;
			return 0;
		case BMA4XX_RANGE_16G:
			*shift = 8;
			return 0;
		default:
			return -EINVAL;
		}
	case SENSOR_CHAN_DIE_TEMP:
		*shift = BMA4XX_TEMP_SHIFT;
		return 0;
	default:
		return -EINVAL;
	}
}

static void bma4xx_convert_raw_accel_to_q31(uint8_t accel_fs, int16_t raw_val, q31_t *out)
{
	/* Raw readings are 12-bit signed ints left-aligned in a 16-bit container. Divide by 16
	 * (arithmetic right shift by 4) to scale these into the correct range and properly handle
	 * the sign extension.
	 */
	raw_val /= 16;

	int8_t shift;
	int lsb_per_g;

	switch (accel_fs) {
	case BMA4XX_RANGE_2G:
		lsb_per_g = 1024;
		break;
	case BMA4XX_RANGE_4G:
		lsb_per_g = 512;
		break;
	case BMA4XX_RANGE_8G:
		lsb_per_g = 256;
		break;
	case BMA4XX_RANGE_16G:
		lsb_per_g = 128;
		break;
	default:
		__ASSERT(0, "Invalid full-scale value");
	}

	if (bma4xx_get_shift(SENSOR_CHAN_ACCEL_XYZ, accel_fs, &shift)) {
		__ASSERT(0, "Error obtaining shift");
	}

	/* Use SENSOR_G and lsb_per_g to convert reg value into micro-m/s^2. Then re-scale into a
	 * Q-number with given shift by multiplying by (2^31 / (1<<shift)) and dividing by 1000000
	 * to turn micro-m/s^2 to m/s^2.
	 */
	int64_t intermediate = (SENSOR_G / lsb_per_g) * ((raw_val * ((int64_t)INT32_MAX + 1)) /
							 ((1 << shift) * INT64_C(1000000)));

	*out = CLAMP(intermediate, INT32_MIN, INT32_MAX);
}

#ifdef CONFIG_BMA4XX_TEMPERATURE
/**
 * @brief Convert the 8-bit temp register value into a Q31 celsius value
 */
static void bma4xx_convert_raw_temp_to_q31(int8_t raw_val, q31_t *out)
{
	/* Value of 0 equals 23 degrees C. Each bit count equals 1 degree C */

	int64_t intermediate =
		((int64_t)raw_val + 23) * ((int64_t)INT32_MAX + 1) / (1 << BMA4XX_TEMP_SHIFT);

	*out = CLAMP(intermediate, INT32_MIN, INT32_MAX);
}
#endif /* CONFIG_BMA4XX_TEMPERATURE */

static int bma4xx_one_shot_decode(const uint8_t *buffer, enum sensor_channel channel,
				  size_t channel_idx, uint32_t *fit, uint16_t max_count,
				  void *data_out)
{
	const struct bma4xx_encoded_data *edata = (const struct bma4xx_encoded_data *)buffer;
	const struct bma4xx_decoder_header *header = &edata->header;
	int rc;

	if (*fit != 0) {
		return 0;
	}
	if (max_count == 0 || channel_idx != 0) {
		return -EINVAL;
	}

	switch (channel) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_XYZ: {
		if (!edata->has_accel) {
			return -ENODATA;
		}

		struct sensor_three_axis_data *out = (struct sensor_three_axis_data *)data_out;

		out->header.base_timestamp_ns = edata->header.timestamp;
		out->header.reading_count = 1;
		rc = bma4xx_get_shift(SENSOR_CHAN_ACCEL_XYZ, header->accel_fs, &out->shift);
		if (rc != 0) {
			return -EINVAL;
		}

		bma4xx_convert_raw_accel_to_q31(header->accel_fs, edata->accel_xyz[0],
						&out->readings[0].x);
		bma4xx_convert_raw_accel_to_q31(header->accel_fs, edata->accel_xyz[1],
						&out->readings[0].y);
		bma4xx_convert_raw_accel_to_q31(header->accel_fs, edata->accel_xyz[2],
						&out->readings[0].z);

		*fit = 1;
		return 1;
	}
#ifdef CONFIG_BMA4XX_TEMPERATURE
	case SENSOR_CHAN_DIE_TEMP: {
		if (!edata->has_temp) {
			return -ENODATA;
		}

		struct sensor_q31_data *out = (struct sensor_q31_data *)data_out;

		out->header.base_timestamp_ns = edata->header.timestamp;
		out->header.reading_count = 1;
		rc = bma4xx_get_shift(SENSOR_CHAN_DIE_TEMP, 0, &out->shift);
		if (rc != 0) {
			return -EINVAL;
		}

		bma4xx_convert_raw_temp_to_q31(edata->temp, &out->readings[0].temperature);

		*fit = 1;
		return 1;
	}
#endif /* CONFIG_BMA4XX_TEMPERATURE */
	default:
		return -EINVAL;
	}
}

static int bma4xx_decoder_decode(const uint8_t *buffer, enum sensor_channel channel,
				 size_t channel_idx, uint32_t *fit, uint16_t max_count,
				 void *data_out)
{
	const struct bma4xx_decoder_header *header = (const struct bma4xx_decoder_header *)buffer;

	if (header->is_fifo) {
		/* FIFO (streaming) mode operation is not yet supported */
		return -ENOTSUP;
	}

	return bma4xx_one_shot_decode(buffer, channel, channel_idx, fit, max_count, data_out);
}

SENSOR_DECODER_API_DT_DEFINE() = {
	.get_frame_count = bma4xx_decoder_get_frame_count,
	.get_size_info = bma4xx_decoder_get_size_info,
	.decode = bma4xx_decoder_decode,
};

static int bma4xx_get_decoder(const struct device *dev, const struct sensor_decoder_api **decoder)
{
	ARG_UNUSED(dev);
	*decoder = &SENSOR_DECODER_NAME();

	return 0;
}

/*
 * Sensor driver API
 */

static const struct sensor_driver_api bma4xx_driver_api = {
	.attr_set = bma4xx_attr_set,
	.submit = bma4xx_submit,
	.get_decoder = bma4xx_get_decoder,
};

/*
 * Device instantiation macros
 */

/* Initializes a struct bma4xx_config for an instance on a SPI bus.
 * SPI operation is not currently supported.
 */

#define BMA4XX_CONFIG_SPI(inst)                                                                    \
	{                                                                                          \
		.bus_cfg.spi = SPI_DT_SPEC_INST_GET(inst, 0, 0), .bus_init = &bma_spi_init,        \
	}

/* Initializes a struct bma4xx_config for an instance on an I2C bus. */
#define BMA4XX_CONFIG_I2C(inst)                                                                    \
	{                                                                                          \
		.bus_cfg.i2c = I2C_DT_SPEC_INST_GET(inst), .bus_init = &bma4xx_i2c_init,           \
	}

/*
 * Main instantiation macro, which selects the correct bus-specific
 * instantiation macros for the instance.
 */
#define BMA4XX_DEFINE(inst)                                                                        \
	static struct bma4xx_data bma4xx_data_##inst;                                              \
	static const struct bma4xx_config bma4xx_config_##inst = COND_CODE_1(                      \
		DT_INST_ON_BUS(inst, spi), (BMA4XX_CONFIG_SPI(inst)), (BMA4XX_CONFIG_I2C(inst)));  \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, bma4xx_chip_init, NULL, &bma4xx_data_##inst,            \
				     &bma4xx_config_##inst, POST_KERNEL,                           \
				     CONFIG_SENSOR_INIT_PRIORITY, &bma4xx_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BMA4XX_DEFINE)
