// SPDX-License-Identifier: GPL-2.0
/*
 * Monitoring metrics trace probe
 * Copyright 2024 Google LLC.
 */

#define pr_fmt(fmt)	"trace_mprobe: " fmt

#include <linux/ftrace.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/rculist.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/timer.h>
#include <linux/trace_events.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <linux/cpumask.h>
#include <linux/kernel_stat.h>

#include <asm/setup.h>  /* for COMMAND_LINE_SIZE */

#include "trace.h"
#include "trace_dynevent.h"
#include "trace_probe.h"
#include "trace_probe_tmpl.h"
#include "trace_probe_kernel.h"

#define MPROBE_EVENT_SYSTEM "mprobes"

enum mprobe_datasrc {
	MP_DS_STAT = 0,
	MP_DS_MEMINFO,
	MP_DS_END,
};

static const char *mprobe_datasrc_str[MP_DS_END] = {
	"stat",
	"meminfo",
};

struct trace_mprobe {
	struct dyn_event	devent;
	struct delayed_work	work;

	enum mprobe_datasrc	dsrc;
	unsigned long		interval;
	u64			last_jiffies_64;

	struct trace_probe	tp;
};

static unsigned long update_next_expire(struct trace_mprobe *mp)
{
	u64 interval = msecs_to_jiffies(mp->interval);
	u64 next = mp->last_jiffies_64 + interval;
	u64 now = get_jiffies_64();

	while (next < now)
		next += interval;

	mp->last_jiffies_64 = next - interval;

	return (unsigned long)(next - now);
}

/* Reserve event buffer and return the data storage address. */
static void *trace_mprobe_reserve_buffer(struct trace_mprobe *mp,
					 struct trace_event_file *trace_file,
					 struct trace_event_buffer *fbuffer)
{
	struct mprobe_trace_entry_head *entry;

	entry = trace_event_buffer_reserve(fbuffer, trace_file,
					   sizeof(*entry) + mp->tp.size);
	if (!entry)
		return NULL;

	fbuffer->regs = NULL;
	entry = fbuffer->entry = ring_buffer_event_data(fbuffer->event);
	entry->dsrc = mp->dsrc;

	return entry + 1;
}

/* Commit event buffer. */
static void trace_mprobe_commit_buffer(struct trace_event_buffer *fbuffer)
{
	trace_event_buffer_commit(fbuffer);
}

/* Process fetch-insn code */
static inline int
process_fetch_insn(struct fetch_insn *code, void *rec, void *edata,
		   void *dest, void *base)
{
	unsigned long val;
	int ret;

retry:
	/* 1st stage: get value from context */
	switch (code->op) {
	case FETCH_OP_DADDR:
		val = (unsigned long)rec;
		break;
	case FETCH_NOP_SYMBOL:	/* Ignore a place holder */
		code++;
		goto retry;
	default:
		ret = process_common_fetch_insn(code, &val);
		if (ret < 0)
			return ret;
	}
	code++;

	return process_fetch_insn_bottom(code, val, dest, base);
}



static void monitor_stat(struct trace_mprobe *mp, struct trace_event_file *trace_file);
static int parse_stat_arg(char *argv, ssize_t *size,
			  struct probe_arg *parg,
			  struct traceprobe_parse_context *ctx);

/*
 * Monitoring worker
 */

static void trace_mprobe_call_monitor_func(struct trace_mprobe *mp,
				      struct trace_event_file *trace_file)
{
	struct trace_event_call *call = trace_probe_event_call(&mp->tp);

	if (WARN_ON_ONCE(call != trace_file->event_call))
		return;

	if (trace_trigger_soft_disabled(trace_file))
		return;

	switch(mp->dsrc) {
	case MP_DS_STAT:
		monitor_stat(mp, trace_file);
		break;
	default:
		return;
	}
}

static void trace_mprobe_monitor_work(struct work_struct *work)
{
	struct delayed_work *dwork = container_of(work, struct delayed_work, work);
	struct trace_mprobe *mp = container_of(dwork, struct trace_mprobe, work);
	struct event_file_link *link;

	rcu_read_lock();
	trace_probe_for_each_link_rcu(link, &mp->tp)
		trace_mprobe_call_monitor_func(mp, link->file);
	rcu_read_unlock();

	schedule_delayed_work(&mp->work, update_next_expire(mp));
}

static int trace_mprobe_create(const char *raw_command);
static int trace_mprobe_show(struct seq_file *m, struct dyn_event *ev);
static int trace_mprobe_release(struct dyn_event *ev);
static bool trace_mprobe_is_busy(struct dyn_event *ev);
static bool trace_mprobe_match(const char *system, const char *event,
			int argc, const char **argv, struct dyn_event *ev);

static struct dyn_event_operations trace_mprobe_ops = {
	.create = trace_mprobe_create,
	.show = trace_mprobe_show,
	.is_busy = trace_mprobe_is_busy,
	.free = trace_mprobe_release,
	.match = trace_mprobe_match,
};

static bool is_trace_mprobe(struct dyn_event *ev)
{
	return ev->ops == &trace_mprobe_ops;
}

static struct trace_mprobe *to_trace_mprobe(struct dyn_event *ev)
{
	return container_of(ev, struct trace_mprobe, devent);
}

#define for_each_trace_mprobe(pos, dpos)	\
	for_each_dyn_event(dpos)		\
		if (is_trace_mprobe(dpos) && (pos = to_trace_mprobe(dpos)))


static struct trace_mprobe *find_trace_mprobe(const char *event,
					      const char *group)
{
	struct dyn_event *pos;
	struct trace_mprobe *mp;

	for_each_trace_mprobe(mp, pos)
		if (!strcmp(trace_probe_name(&mp->tp), event) &&
		    !strcmp(trace_probe_group_name(&mp->tp), group))
			return mp;
	return NULL;
}

static bool trace_mprobe_is_busy(struct dyn_event *ev)
{
	struct trace_mprobe *mp = to_trace_mprobe(ev);

	return trace_probe_is_enabled(&mp->tp);
}

static bool trace_mprobe_match(const char *system, const char *event,
			int argc, const char **argv, struct dyn_event *ev)
{
	struct trace_mprobe *mp = to_trace_mprobe(ev);

	return (event[0] == '\0' ||
		!strcmp(trace_probe_name(&mp->tp), event)) &&
		(!system || !strcmp(trace_probe_group_name(&mp->tp), system)) &&
		trace_probe_match_command_args(&mp->tp, argc, argv);
}

static void free_trace_mprobe(struct trace_mprobe *mp)
{
	if (mp) {
		trace_probe_cleanup(&mp->tp);
		kfree(mp);
	}
}

static struct trace_mprobe *alloc_trace_mprobe(const char *group,
						const char *event,
						enum mprobe_datasrc dsrc,
						unsigned long interval,
						int nargs)
{
	struct trace_mprobe *mp;
	int ret;

	mp = kzalloc(struct_size(mp, tp.args, nargs), GFP_KERNEL);
	if (!mp)
		return ERR_PTR(-ENOMEM);

	ret = trace_probe_init(&mp->tp, event, group, false, nargs);
	if (ret < 0) {
		free_trace_mprobe(mp);
		return ERR_PTR(ret);
	}

	mp->dsrc = dsrc;
	mp->interval = interval;
	INIT_DELAYED_WORK(&mp->work, trace_mprobe_monitor_work);

	dyn_event_init(&mp->devent, &trace_mprobe_ops);
	return mp;
}

static void __enable_trace_mprobe(struct trace_mprobe *mp)
{
	/* The first event should run soon. */
	mp->last_jiffies_64 = get_jiffies_64() - msecs_to_jiffies(mp->interval);
	schedule_delayed_work(&mp->work, 0);
}

static void __disable_trace_mprobe(struct trace_mprobe *mp)
{
	cancel_delayed_work(&mp->work);
}

static int enable_trace_mprobe(struct trace_event_call *call,
			       struct trace_event_file *file)
{
	struct trace_mprobe *mp;
	struct trace_probe *tp;
	bool enabled;
	int ret = 0;

	if (!file)
		return -EOPNOTSUPP;

	tp = trace_probe_primary_from_call(call);
	if (WARN_ON_ONCE(!tp))
		return -ENODEV;
	enabled = trace_probe_is_enabled(tp);

	/* This also changes "enabled" state */
	ret = trace_probe_add_file(tp, file);
	if (ret)
		return ret;

	if (!enabled) {
		list_for_each_entry(mp, trace_probe_probe_list(tp), tp.list)
			__enable_trace_mprobe(mp);
	}

	return 0;
}

static int disable_trace_mprobe(struct trace_event_call *call,
				struct trace_event_file *file)
{
	struct trace_mprobe *mp;
	struct trace_probe *tp;

	if (!file)
		return -EOPNOTSUPP;

	tp = trace_probe_primary_from_call(call);
	if (WARN_ON_ONCE(!tp))
		return -ENODEV;

	if (!trace_probe_get_file_link(tp, file))
		return -ENOENT;

	if (trace_probe_has_single_file(tp)) {
		list_for_each_entry(mp, trace_probe_probe_list(tp), tp.list)
			__disable_trace_mprobe(mp);
		trace_probe_clear_flag(tp, TP_FLAG_TRACE);
	}
	trace_probe_remove_file(tp, file);

	return 0;
}

static int mprobe_register(struct trace_event_call *call,
			   enum trace_reg type, void *data)
{
	struct trace_event_file *file = data;

	switch (type) {
	case TRACE_REG_REGISTER:
		return enable_trace_mprobe(call, file);
	case TRACE_REG_UNREGISTER:
		return disable_trace_mprobe(call, file);
	default:
		return 0;
	}
}

/* Event entry printers */
static enum print_line_t
print_mprobe_event(struct trace_iterator *iter, int flags,
		   struct trace_event *event)
{
	struct mprobe_trace_entry_head *field;
	struct trace_seq *s = &iter->seq;
	struct trace_probe *tp;

	field = (struct mprobe_trace_entry_head *)iter->ent;
	tp = trace_probe_primary_from_call(
		container_of(event, struct trace_event_call, event));
	if (WARN_ON_ONCE(!tp))
		goto out;

	trace_seq_printf(s, "%s: (dsrc:%d)", trace_probe_name(tp),
			field->dsrc);

	trace_probe_print_args(s, tp->args, tp->nr_args,
			     (u8 *)&field[1], field);
	trace_seq_putc(s, '\n');

 out:
	return trace_handle_return(s);
}

static int mprobe_event_define_fields(struct trace_event_call *event_call)
{
	int ret;
	struct mprobe_trace_entry_head field;
	struct trace_probe *tp;

	tp = trace_probe_primary_from_call(event_call);
	if (WARN_ON_ONCE(!tp))
		return -ENOENT;

	DEFINE_FIELD(int, dsrc, FIELD_STRING_DATASRC, 0);

	return traceprobe_define_arg_fields(event_call, sizeof(field), tp);
}

static struct trace_event_functions mprobe_funcs = {
	.trace	= print_mprobe_event,
};

static struct trace_event_fields mprobe_fields_array[] = {
	{ .type = TRACE_FUNCTION_TYPE,
	  .define_fields = mprobe_event_define_fields, },
	{}
};

static int register_mprobe_event(struct trace_mprobe *mp)
{
	struct trace_event_call *call = trace_probe_event_call(&mp->tp);

	call->event.funcs = &mprobe_funcs;
	call->class->fields_array = mprobe_fields_array;
	call->flags = TRACE_EVENT_FL_DYNAMIC;
	call->class->reg = mprobe_register;

	return trace_probe_register_event_call(&mp->tp);
}

static int register_trace_mprobe(struct trace_mprobe *mp)
{
	struct trace_mprobe *old_mp;
	int ret = 0;

	mutex_lock(&event_mutex);

	old_mp = find_trace_mprobe(trace_probe_name(&mp->tp),
				trace_probe_group_name(&mp->tp));
	if (old_mp) {
		ret = -EEXIST;
		goto out;
	}

	ret = register_mprobe_event(mp);
	if (ret)
		goto out;

	dyn_event_add(&mp->devent, trace_probe_event_call(&mp->tp));
out:
	mutex_unlock(&event_mutex);
	return ret;
}

static int parse_datasource(const char *cmd, enum mprobe_datasrc *dsrc,
			    unsigned long *interval)
{
	int i, len = 0;

	for (i = 0; i < MP_DS_END; i++) {
		len = str_has_prefix(cmd, mprobe_datasrc_str[i]);
		if (len && cmd[len] == '/')
			break;
	}
	if (i == MP_DS_END) {
		trace_probe_log_err(0, BAD_DATASRC);
		return -EINVAL;
	}
	*dsrc = i;

	if (kstrtoul(&cmd[len + 1], 0, interval) < 0) {
		trace_probe_log_err(len + 1, BAD_INTERVAL);
		return -EINVAL;
	}

	return 0;
}

static int trace_mprobe_parse_arg(char *argv, ssize_t *size,
				  struct probe_arg *parg,
				  struct traceprobe_parse_context *ctx)
{
	struct trace_mprobe *mp = container_of(ctx->tp, struct trace_mprobe, tp);

	switch (mp->dsrc) {
	case MP_DS_STAT:
		return parse_stat_arg(argv, size, parg, ctx);
	default:
		trace_probe_log_err(0, BAD_DATASRC);
	}
	return -EINVAL;
}

static int __trace_mprobe_create(int argc, const char *argv[])
{
	/*
	 * Add mprobe
	 *	m[:[GRP/]EVENT] DSRC/INTERVAL [ARG=]MONARG [[ARG=]MONARG...]
	 *
	 * DSRC:
	 *  cpustat - per CPU statistics of CPU usage. The events will be
	 *            sent for each CPU.
	 *  stat - CPU and IRQ statistics. The event will be sent once.
	 *  meminfo - Memory usage. The event will be sent once.
	 *
	 * MONARG:
	 *  for cpustat and stat:
	 *   user, nice, sys, idle, iowait, irq, sirq, steal, guest, gnice
	 *  for stat:
	 *   intr, ctxt, fork, running, blocked
	 *  for meminfo:
	 *   total,free,avail,buffer,cached,swapc,active,
	 *   swaptotal,swapfree,dirty,mapped,shmem
	 */
	const char *event = NULL, *group = MPROBE_EVENT_SYSTEM;
	struct trace_mprobe *mp = NULL;
	char gbuf[MAX_EVENT_NAME_LEN];
	enum mprobe_datasrc dsrc = MP_DS_END;
	unsigned long interval = 0;
	int i, ret;
	struct traceprobe_parse_context ctx = {
		.flags = TPARG_FL_KERNEL | TPARG_FL_MPROBE,
		.parse_arg = trace_mprobe_parse_arg,
	};

	if (argv[0][0] != 'm' || argc < 3)
		return -ECANCELED;

	trace_probe_log_init("trace_mprobe", argc, argv);

	/*
	 * Parse data source at first because this may be used for generating
	 * event name.
	 */
	trace_probe_log_set_index(1);
	ret = parse_datasource(argv[1], &dsrc, &interval);
	if (ret)
		return ret;

	trace_probe_log_set_index(0);
	if (argv[0][1] == '\0') {
		/* Generate event name from data source and interval */
		ret = sprintf(gbuf, "%s_%lu", mprobe_datasrc_str[dsrc], interval);
		if (ret < 0 || ret >= MAX_EVENT_NAME_LEN - 1)
			return -ENOMEM;
		event = gbuf;
	} else if (argv[0][1] == ':') {
		event = &argv[0][2];
		ret = traceprobe_parse_event_name(&event, &group, gbuf,
						  event - argv[0]);
		if (ret < 0)
			return ret;
	} else {
		trace_probe_log_err(1, NO_EVENT_NAME);
		return -EINVAL;
	}

	argc -= 2; argv += 2;
	mp = alloc_trace_mprobe(group, event, dsrc, interval, argc);
	if (!mp)
		return -ENOMEM;

	ctx.tp = &mp->tp;
	for (i = 0; i < argc && i < MAX_TRACE_ARGS; i++) {
		trace_probe_log_set_index(i + 2);
		ret = traceprobe_parse_probe_arg(&mp->tp, i, argv[i], &ctx);
		if (ret < 0) {
			free_trace_mprobe(mp);
			mp = NULL;
			return ret;
		}
	}

	ret = register_trace_mprobe(mp);
	if (ret) {
		trace_probe_log_set_index(0);
		trace_probe_log_err(0, EVENT_EXIST);
	}
	return ret;
}

static int trace_mprobe_create(const char *raw_command)
{
	return trace_probe_create(raw_command, __trace_mprobe_create);
}

static int trace_mprobe_release(struct dyn_event *ev)
{
	struct trace_mprobe *mp = to_trace_mprobe(ev);

	free_trace_mprobe(mp);
	return 0;
}

static int trace_mprobe_show(struct seq_file *m, struct dyn_event *ev)
{
	struct trace_mprobe *mp = to_trace_mprobe(ev);
	int i;

	seq_printf(m, "m %s/%lu", mprobe_datasrc_str[mp->dsrc], mp->interval);

	for (i = 0; i < mp->tp.nr_args; i++)
		seq_printf(m, " %s=%s", mp->tp.args[i].name, mp->tp.args[i].comm);
	seq_putc(m, '\n');
	return 0;
}

/*
 * Monitor /proc/stat worker
 */
static void monitor_stat(struct trace_mprobe *mp, struct trace_event_file *trace_file)
{
	struct trace_event_buffer fbuffer;
	struct kernel_cpustat total = {};
	void *buf;
	int i;

	buf = trace_mprobe_reserve_buffer(mp, trace_file, &fbuffer);
	if (!buf)
		return;

	for_each_possible_cpu(i) {
		struct kernel_cpustat kcpustat;
		u64 *cpustat = kcpustat.cpustat;

		kcpustat_cpu_fetch(&kcpustat, i);

		total.cpustat[CPUTIME_USER] += cpustat[CPUTIME_USER];
		total.cpustat[CPUTIME_NICE] += cpustat[CPUTIME_NICE];
		total.cpustat[CPUTIME_SYSTEM] += cpustat[CPUTIME_SYSTEM];
		total.cpustat[CPUTIME_IDLE] += get_idle_time(&kcpustat, i);
		total.cpustat[CPUTIME_IOWAIT] += get_iowait_time(&kcpustat, i);// TODO: expose
		total.cpustat[CPUTIME_IRQ] += cpustat[CPUTIME_IRQ];
		total.cpustat[CPUTIME_SOFTIRQ] += cpustat[CPUTIME_SOFTIRQ];
		total.cpustat[CPUTIME_STEAL] += cpustat[CPUTIME_STEAL];
		total.cpustat[CPUTIME_GUEST] += cpustat[CPUTIME_GUEST];
		total.cpustat[CPUTIME_GUEST_NICE] += cpustat[CPUTIME_GUEST_NICE];
	}

	store_trace_args(buf, &mp->tp, &total.cpustat, NULL,
			 sizeof(struct mprobe_trace_entry_head), 0);
	trace_mprobe_commit_buffer(&fbuffer);
}

static struct {
	const char * const name;
	int index;
} kstat_array[] = {
	{"user",	CPUTIME_USER},
	{"nice",	CPUTIME_NICE},
	{"sys",		CPUTIME_SYSTEM},
	{"idle",	CPUTIME_IDLE},
	{"iowait",	CPUTIME_IOWAIT},
	{"irq",		CPUTIME_IRQ},
	{"softirq",	CPUTIME_SOFTIRQ},
	{"steal",	CPUTIME_STEAL},
	{"guest",	CPUTIME_GUEST},
	{"guest_nice",	CPUTIME_GUEST_NICE},
};

static int parse_stat_arg(char *argv, ssize_t *size,
			  struct probe_arg *parg,
			  struct traceprobe_parse_context *ctx)
{
	struct fetch_insn *code;
	unsigned long offset;
	int i;

	for (i = 0; i < ARRAY_SIZE(kstat_array); i++) {
		if (!strcmp(kstat_array[i].name, argv)) {
			offset = kstat_array[i].index * sizeof(u64);
			break;
		}
	}
	if (i == ARRAY_SIZE(kstat_array)) {
		trace_probe_log_err(offset, BAD_MON_ARG);
		return -ENOENT;
	}

	/*
	 * stat uses kernel_stat. So it is u64 array. For 32 bit arch, it gets
	 * corrresponding entry address and store it.
	 */
	code = kcalloc(3, sizeof(*code), GFP_KERNEL);
	if (!code)
		return -ENOMEM;

	code[0].op = FETCH_OP_DADDR;
	code[1].op = FETCH_OP_ST_MEM;
	code[1].offset = offset;
	code[1].size = sizeof(u64);
	code[2].op = FETCH_OP_END;

	parg->type = traceprobe_find_fetch_type("u64");
	parg->code = code;

	return 0;
}

/*
 * Register dynevent at core_initcall. This allows kernel to setup mprobe
 * events in postcore_initcall without tracefs.
 */
static __init int init_mprobe_trace_early(void)
{
	int ret;

	ret = dyn_event_register(&trace_mprobe_ops);
	if (ret)
		return ret;

	return 0;
}
core_initcall(init_mprobe_trace_early);