/*
 * Copyright (c) 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/emul_sensor.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/rtio/rtio.h>

/*
 * Set up an RTIO context that can be shared for all sensors
 */

static enum sensor_channel iodev_all_channels[SENSOR_CHAN_ALL];
static struct sensor_read_config iodev_read_config = {
	.sensor = NULL,
	.channels = iodev_all_channels,
	.count = 0,
	.max = SENSOR_CHAN_ALL,
};
RTIO_IODEV_DEFINE(iodev_read, &__sensor_iodev_api, &iodev_read_config);

/* Create the RTIO context to service the reading */
RTIO_DEFINE_WITH_MEMPOOL(sensor_read_rtio_ctx, 1, 1, 1, 64, 4);

/** Number of test points to use in the generic test. These are automatically generated. */
#define NUM_EXPECTED_VALS (5)

/**
 * @brief Helper function the carries out the generic sensor test for a given sensor device.
 *        Verifies that the device has a suitable emulator that implements the backend API and
 *        skips the test gracefully if not.
 */
static void run_generic_test(const struct device *dev)
{
	zassert_not_null(dev, "Cannot get device pointer. Is this driver properly instantiated?");

	const struct emul *emul = emul_get_binding(dev->name);

	/* Skip this sensor if there is no emulator loaded. */
	if (!emul) {
		ztest_test_skip();
	}

	/* Also skip if this emulator does not implement the backend API. */
	if (!emul_sensor_backend_is_supported(emul)) {
		ztest_test_skip();
	}

	/*
	 * Begin the actual test sequence
	 */

	static struct {
		bool supported;
		bool received;
		q31_t expected_values[NUM_EXPECTED_VALS];
		q31_t epsilon;
		int8_t expected_value_shift;
	} channel_table[SENSOR_CHAN_ALL];

	/* Discover supported channels on this device and fill out our sensor read request */
	for (enum sensor_channel ch = 0; ch < ARRAY_SIZE(channel_table); ch++) {
		if (SENSOR_CHANNEL_3_AXIS(ch)) {
			continue;
		}

		q31_t lower, upper;
		int8_t shift;

		if (emul_sensor_backend_get_sample_range(emul, ch, &lower, &upper,
							 &channel_table[ch].epsilon, &shift) == 0) {
			/* This channel is supported */
			channel_table[ch].supported = true;

			/* Add to the list of channels to read */
			iodev_all_channels[iodev_read_config.count++] = ch;

			/* Generate a set of NUM_EXPECTED_VALS test values */
			channel_table[ch].expected_value_shift = shift;
			for (size_t i = 0; i < NUM_EXPECTED_VALS; i++) {
				channel_table[ch].expected_values[i] =
					lower + (i * (upper - lower) / (NUM_EXPECTED_VALS - 1));
			}
		}
	}
	iodev_read_config.sensor = dev;

	/* Do a read of all channels for quantity NUM_EXPECTED_VALS rounds. */

	int rv;
	uint8_t *buf = NULL;
	uint32_t buf_len = 0;
	const struct sensor_decoder_api *decoder;
	sensor_frame_iterator_t fit;
	sensor_channel_iterator_t cit;
	enum sensor_channel channel;
	q31_t q;
	int8_t shift;
	int missing_channel_count;

	zassert_ok(sensor_get_decoder(dev, &decoder));

	for (size_t round = 0; round < NUM_EXPECTED_VALS; round++) {
		/* Reset received flag */
		for (size_t i = 0; i < ARRAY_SIZE(channel_table); i++) {
			channel_table[i].received = false;
		}

		/* Reset decoder state */
		fit = (sensor_frame_iterator_t){0};
		cit = (sensor_channel_iterator_t){0};

		/* Set this round's expected values in emul for every supported channel */
		for (size_t i = 0; i < iodev_read_config.count; i++) {
			enum sensor_channel ch = iodev_all_channels[i];

			zassert_ok(emul_sensor_backend_set_channel(
					   emul, ch, channel_table[ch].expected_values[round],
					   channel_table[ch].expected_value_shift),
				   "Cannot set value %08x on channel %d (round %d)",
				   channel_table[i].expected_values[round], ch, round);
		}

		/* Perform the actual sensor read */
		rv = sensor_read(&iodev_read, &sensor_read_rtio_ctx, NULL);
		zassert_ok(rv, "Got %d when reading sensor", rv);

		/* Wait for a CQE */
		struct rtio_cqe *cqe = rtio_cqe_consume_block(&sensor_read_rtio_ctx);

		/* Cache the data from the CQE */
		rtio_cqe_get_mempool_buffer(&sensor_read_rtio_ctx, cqe, &buf, &buf_len);

		/* Release the CQE */
		rtio_cqe_release(&sensor_read_rtio_ctx, cqe);

		/* Decode the buffer and verify all channels */
		while (decoder->decode(buf, &fit, &cit, &channel, &q, 1) > 0) {
			zassert_true(channel_table[channel].supported);
			zassert_false(channel_table[channel].received);
			channel_table[channel].received = true;

			zassert_ok(decoder->get_shift(buf, channel, &shift));

			/* Align everything to be a 64-bit Q32.32 number for comparison */
			int64_t expected_shifted = channel_table[channel].expected_values[round]
						   << channel_table[channel].expected_value_shift;
			int64_t actual_shifted = q << shift;
			int64_t epsilon_shifted = channel_table[channel].epsilon
						  << channel_table[channel].expected_value_shift;

			zassert_within(expected_shifted, actual_shifted, epsilon_shifted,
				       "Expected %lld, got %lld (shift=%d, ch=%d, round=%d)",
				       expected_shifted, actual_shifted, shift, channel, round);
		}

		/* Release the memory */
		rtio_release_buffer(&sensor_read_rtio_ctx, buf, buf_len);

		/* Ensure all supported channels were received */
		missing_channel_count = 0;
		for (size_t i = 0; i < ARRAY_SIZE(channel_table); i++) {
			if (channel_table[i].supported && !channel_table[i].received) {
				missing_channel_count++;
			}
		}

		zassert_equal(0, missing_channel_count);
	}
}

#define DECLARE_ZTEST_PER_DEVICE(n)                                                                \
	ZTEST(generic, test_##n)                                                                   \
	{                                                                                          \
		run_generic_test(DEVICE_DT_GET(n));                                                \
	}

/* Iterate through each of the emulated buses and create a test for each device. */
DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(test_i2c), DECLARE_ZTEST_PER_DEVICE)
DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(test_spi), DECLARE_ZTEST_PER_DEVICE)

ZTEST_SUITE(generic, NULL, NULL, NULL, NULL, NULL);
