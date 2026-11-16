#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# CMDLINE: bootconfig bootconfig=
# Check explicit empty bootconfig= disables earlier bootconfig selection
TRACEDIR="/sys/kernel/tracing"

if [ -d "$TRACEDIR/instances/cleared_inst" ]; then
	echo "FAIL: cleared_inst should not exist when bootconfig= clears selection"
	exit 1
fi

if [ -s /proc/bootconfig ]; then
	echo "FAIL: /proc/bootconfig should be empty"
	cat /proc/bootconfig
	exit 1
fi

echo "PASS: bconf-multi-04-clear"
exit 0
