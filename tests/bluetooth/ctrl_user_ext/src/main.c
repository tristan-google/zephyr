/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <sys/printk.h>
#include <ztest.h>

#include <bluetooth/bluetooth.h>

ZTEST_SUITE(test_bluetooth, NULL, NULL, NULL, NULL, NULL);

ZTEST(test_bluetooth, test_ctrl_user_ext)
{
	zassert_false(bt_enable(NULL), "Bluetooth ctrl_user_ext failed");
}
