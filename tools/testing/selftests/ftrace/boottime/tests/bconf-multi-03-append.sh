#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# CMDLINE: bootconfig=+dbg
# Check bootconfig=+id appends onto default (index 0) with += array append
TRACEDIR="/sys/kernel/tracing"

if [ -f /proc/bootconfig ] && grep -q "dump_bconf" /proc/cmdline 2>/dev/null; then
	echo "=== /proc/bootconfig ==="
	cat /proc/bootconfig
	echo "========================"
fi

if [ ! -d "$TRACEDIR/instances/app_inst" ]; then
	echo "FAIL: instance app_inst does not exist"
	exit 1
fi

EN1=$(cat "$TRACEDIR/instances/app_inst/events/sched/sched_switch/enable")
EN2=$(cat "$TRACEDIR/instances/app_inst/events/sched/sched_wakeup/enable")
if [ "$EN1" != "1" ] || [ "$EN2" != "1" ]; then
	echo "FAIL: expected both sched_switch ($EN1) and sched_wakeup ($EN2) enabled"
	exit 1
fi

if ! grep -q "bconf_append_key=base_val" /proc/cmdline || \
   ! grep -q "bconf_append_key=dbg_val" /proc/cmdline; then
	echo "FAIL: appended kernel.bconf_append_key values missing from /proc/cmdline"
	exit 1
fi

echo "PASS: bconf-multi-03-append"
exit 0
