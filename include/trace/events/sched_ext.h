/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM sched_ext

#if !defined(_TRACE_SCHED_EXT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCHED_EXT_H

#include <linux/tracepoint.h>

TRACE_EVENT(sched_ext_dump,

	TP_PROTO(const char *line),

	TP_ARGS(line),

	TP_STRUCT__entry(
		__string(line, line)
	),

	TP_fast_assign(
		__assign_str(line);
	),

	TP_printk("%s",
		__get_str(line)
	)
);

TRACE_EVENT(sched_ext_event,
	    TP_PROTO(const char *name, __s64 delta),
	    TP_ARGS(name, delta),

	TP_STRUCT__entry(
		__string(name, name)
		__field(	__s64,		delta		)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->delta		= delta;
	),

	TP_printk("name %s delta %lld",
		  __get_str(name), __entry->delta
	)
);

#endif /* _TRACE_SCHED_EXT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
