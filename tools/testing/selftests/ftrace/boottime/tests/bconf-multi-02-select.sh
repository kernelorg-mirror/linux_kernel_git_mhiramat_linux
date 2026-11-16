#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# CMDLINE: bootconfig=base,dbg
# Check bootconfig=<id1>,<id2> selects and merges configs with := override
TRACEDIR="/sys/kernel/tracing"

if [ -f /proc/bootconfig ] && grep -q "dump_bconf" /proc/cmdline 2>/dev/null; then
	echo "=== /proc/bootconfig ==="
	cat /proc/bootconfig
	echo "========================"
fi

if [ ! -d "$TRACEDIR/instances/base_inst" ]; then
	echo "FAIL: instance base_inst does not exist"
	exit 1
fi

if [ ! -d "$TRACEDIR/instances/dbg_inst" ]; then
	echo "FAIL: instance dbg_inst does not exist"
	exit 1
fi

if [ -d "$TRACEDIR/instances/alt_inst" ]; then
	echo "FAIL: unselected instance alt_inst should not exist"
	exit 1
fi

BUFSIZE=$(cat "$TRACEDIR/instances/base_inst/buffer_size_kb")
if [ "$BUFSIZE" -lt 2048 ] 2>/dev/null; then
	echo "FAIL: base_inst buffer_size_kb is $BUFSIZE, expected >= 2048"
	exit 1
fi

if ! grep -q "bconf_test_arg=from_dbg" /proc/cmdline; then
	echo "FAIL: overridden kernel.bconf_test_arg not in /proc/cmdline"
	exit 1
fi

echo "PASS: bconf-multi-02-select"
exit 0
