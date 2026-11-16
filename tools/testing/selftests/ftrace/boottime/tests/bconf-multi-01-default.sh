#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Check bare bootconfig loads only index 0 (default) when multiple configs exist
TRACEDIR="/sys/kernel/tracing"

if [ -f /proc/bootconfig ] && grep -q "dump_bconf" /proc/cmdline 2>/dev/null; then
	echo "=== /proc/bootconfig ==="
	cat /proc/bootconfig
	echo "========================"
fi

if [ ! -d "$TRACEDIR/instances/def_inst" ]; then
	echo "FAIL: default instance def_inst does not exist"
	exit 1
fi

ENABLE=$(cat "$TRACEDIR/instances/def_inst/events/sched/sched_switch/enable")
if [ "$ENABLE" != "1" ]; then
	echo "FAIL: def_inst sched_switch is not enabled ($ENABLE)"
	exit 1
fi

if [ -d "$TRACEDIR/instances/dbg_inst" ]; then
	echo "FAIL: unselected instance dbg_inst should not exist"
	exit 1
fi

echo "PASS: bconf-multi-01-default"
exit 0
