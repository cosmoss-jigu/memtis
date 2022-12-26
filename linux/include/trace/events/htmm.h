/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM htmm

#if !defined(_TRACE_HTMM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HTMM_H

#include <linux/tracepoint.h>

TRACE_EVENT(access_info,

	TP_PROTO(unsigned int nr_access, unsigned int nr_util),

	TP_ARGS(nr_access, nr_util),

	TP_STRUCT__entry(
		__field(unsigned int,	nr_access)
		__field(unsigned int,	nr_util)
	),

	TP_fast_assign(
		__entry->nr_access = nr_access;
		__entry->nr_util = nr_util;
	),

	TP_printk("nr_access: %u nr_util: %u\n",
		__entry->nr_access, __entry->nr_util)
);

TRACE_EVENT(base_access_info,

	TP_PROTO(unsigned long addr, unsigned int clock, unsigned int nr_access, unsigned int nr_util),

	TP_ARGS(addr, clock, nr_access, nr_util),

	TP_STRUCT__entry(
		__field(unsigned long,	addr)
		__field(unsigned int,	clock)
		__field(unsigned int,	nr_access)
		__field(unsigned int,	nr_util)
	),

	TP_fast_assign(
		__entry->addr = addr;
		__entry->clock = clock;
		__entry->nr_access = nr_access;
		__entry->nr_util = nr_util;
	),

	TP_printk("addr: %lu clock: %u nr_access: %u nr_util: %u\n",
		__entry->addr, __entry->clock, __entry->nr_access, __entry->nr_util)
);

#endif

/* This part must be outside protection */
#include <trace/define_trace.h>
