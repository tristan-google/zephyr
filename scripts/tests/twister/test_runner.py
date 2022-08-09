#!/usr/bin/env python3
# Copyright (c) 2022 Google LLC
#
# SPDX-License-Identifier: Apache-2.0
"""
Tests for runner.py classes
"""

import mock
import os
import sys
import pytest

from pathlib import Path

ZEPHYR_BASE = os.getenv("ZEPHYR_BASE")
sys.path.insert(0, os.path.join(ZEPHYR_BASE, "scripts/pylib/twister"))

from twisterlib.runner import ProjectBuilder

@mock.patch("os.path.exists")
def test_projectbuilder_cmake_assemble_args(m):
    # Causes the additional_overlay_path to be appended
    m.return_value = True

    class MockHandler:
        pass

    handler = MockHandler()
    handler.args = ["handler_arg1", "handler_arg2"]
    handler.ready = True

    assert(ProjectBuilder.cmake_assemble_args(
        [
            "basearg1", "CONF_FILE=a.conf;b.conf",
            "DTC_OVERLAY_FILE=w.overlay;x.overlay",
            "OVERLAY_CONFIG=overlay.conf"
        ],
        handler,
        ["c.conf", "d.conf"],
        ["extra_overlay.conf"],
        ["y.overlay", "z.overlay"],
        ["cmake1=foo", "cmake2=bar"],
        "/builddir/",
    ) == [
        "-Dcmake1=foo", "-Dcmake2=bar",
        "-Dbasearg1",
        "-Dhandler_arg1", "-Dhandler_arg2",
        "-DCONF_FILE=a.conf;b.conf;c.conf;d.conf",
        "-DDTC_OVERLAY_FILE=w.overlay;x.overlay;y.overlay;z.overlay",
        "-DOVERLAY_CONFIG=overlay.conf extra_overlay.conf /builddir/twister/testsuite_extra.conf",
    ])
