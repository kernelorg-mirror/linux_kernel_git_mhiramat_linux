/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_TRACE_REMOTE_H
#define _LINUX_TRACE_REMOTE_H

#include <linux/dcache.h>
#include <linux/ring_buffer.h>
#include <linux/trace_remote_event.h>

/**
 * struct trace_remote_callbacks - Callbacks used by Tracefs to control the remote
 * @flags:		Flags controlling remote behavior (e.g. TRACE_REMOTE_FL_AUTOREMOVE)
 * @init:		Called once the remote has been registered. Allows the
 *			caller to extend the Tracefs remote directory
 * @destroy:		Optional callback called when the remote is unregistered
 * @load_trace_buffer:  Called before Tracefs accesses the trace buffer for the first
 *			time. Must return a &trace_buffer_desc
 *			(most likely filled with trace_remote_alloc_buffer())
 * @unload_trace_buffer:
 *			Called once Tracefs has no use for the trace buffer
 *			(most likely call trace_remote_free_buffer())
 * @enable_tracing:	Optional. Called on Tracefs tracing_on. It is expected
 *			from the remote to allow writing. If NULL, tracing_on
 *			writes return -ENODEV.
 * @swap_reader_page:	Called when Tracefs consumes a new page from a
 *			ring-buffer. It is expected from the remote to isolate a
 *			new reader-page from the @cpu ring-buffer.
 * @reset:		Called on `echo 0 > trace`. It is expected from the
 *			remote to reset all ring-buffer pages.
 * @print_event:	Optional callback to format and print an event into the
 *			trace sequence. If provided, overrides default
 *			remote_event printing. @len is the length of @evt data.
 *			Must return 0 on success, or a negative errno on error.
 *			Overflow of the trace_seq is detected by the caller via
 *			trace_seq_has_overflowed().
 * @enable_event:	Called on events/event_name/enable. It is expected from
 *			the remote to allow the writing event @id.
 */
enum trace_remote_flags {
	TRACE_REMOTE_FL_AUTOREMOVE	= BIT(0),
};

struct trace_remote_callbacks {
	unsigned int	flags;
	int	(*init)(struct dentry *d, void *priv);
	void	(*destroy)(void *priv);
	struct trace_buffer_desc *(*load_trace_buffer)(unsigned long size, void *priv);
	void	(*unload_trace_buffer)(struct trace_buffer_desc *desc, void *priv);
	int	(*enable_tracing)(bool enable, void *priv);
	int	(*swap_reader_page)(unsigned int cpu, void *priv);
	int	(*reset)(unsigned int cpu, void *priv);
	int	(*print_event)(struct trace_seq *s, void *evt, int len, int cpu, u64 ts,
			       unsigned long lost_events, void *priv);
	int	(*enable_event)(unsigned short id, bool enable, void *priv);
};

#ifdef CONFIG_TRACE_REMOTE
int trace_remote_register(const char *name, const struct trace_remote_callbacks *cbs, void *priv,
			  struct remote_event *events, size_t nr_events);
int trace_remote_unregister(const char *name);
#else
static inline int trace_remote_register(const char *name,
					const struct trace_remote_callbacks *cbs,
					void *priv,
					struct remote_event *events, size_t nr_events)
{
	return -ENODEV;
}
static inline int trace_remote_unregister(const char *name)
{
	return -ENODEV;
}
#endif

int trace_remote_alloc_buffer(struct trace_buffer_desc *desc, size_t desc_size, size_t buffer_size,
			      const struct cpumask *cpumask);

void trace_remote_free_buffer(struct trace_buffer_desc *desc);

#endif
