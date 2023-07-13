/*
 * Copyright (c) 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/sensor.h>

#include <stdint.h>

/**
 * @brief Collection of function pointers implementing a common backend API for sensor emulators
 */
struct emul_sensor_backend_api {
	/** Sets a given fractional value for a given sensor channel. */
	int (*set_channel)(const struct emul *emul, enum sensor_channel ch, q31_t value,
			   uint8_t shift);
	/** Retrieve a range of sensor values to use with test. */
	int (*get_sample_range)(const struct emul *emul, enum sensor_channel ch, q31_t *lower,
				q31_t *upper, q31_t *epsilon, int8_t *shift);
};

static inline bool emul_sensor_backend_is_supported(const struct emul *emul)
{
	return emul && emul->backend_api;
}

static inline int emul_sensor_backend_set_channel(const struct emul *emul, enum sensor_channel ch,
						  q31_t value, uint8_t shift)
{
	if (!emul || !emul->backend_api) {
		return -ENOTSUP;
	}

	struct emul_sensor_backend_api *api = (struct emul_sensor_backend_api *)emul->backend_api;

	if (api->set_channel) {
		return api->set_channel(emul, ch, value, shift);
	}
	return -ENOTSUP;
}

static inline int emul_sensor_backend_get_sample_range(const struct emul *emul,
						       enum sensor_channel ch, q31_t *lower,
						       q31_t *upper, q31_t *epsilon, int8_t *shift)
{
	if (!emul || !emul->backend_api) {
		return -ENOTSUP;
	}

	struct emul_sensor_backend_api *api = (struct emul_sensor_backend_api *)emul->backend_api;

	if (api->get_sample_range) {
		return api->get_sample_range(emul, ch, lower, upper, epsilon, shift);
	}
	return -ENOTSUP;
}