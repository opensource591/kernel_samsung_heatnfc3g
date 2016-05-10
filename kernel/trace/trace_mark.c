/**
 * COPYRIGHT (C)  SAMSUNG Electronics CO., LTD (Suwon, Korea). 2009
 * All rights are reserved. Reproduction and redistiribution in whole or
 * in part is prohibited without the written consent of the copyright owner.
 */

/**
 * Trace kmem
 *
 * @autorh kumhyun.cho@samsung.com
 * @since 2014.02.14
 */

#define CREATE_TRACE_POINTS
#include <trace/events/mark.h>

void trace_mark(
		const char* event,
		unsigned long ppid,
		const char* name,
		unsigned long value,
		const char* category) {
	trace_tracing_mark_write(event, ppid, name, value, category);
}
EXPORT_SYMBOL(trace_mark);

void trace_mark_int(
		unsigned long ppid,
		const char* name,
		unsigned long value,
		const char* category) {
	trace_mark("C", ppid, name, value, category);
}
EXPORT_SYMBOL(trace_mark_int);

void trace_mark_begin(
		unsigned long ppid,
		const char* title,
		const char* thread) {
	trace_mark("B", ppid, title, 0 /* not used */, thread);
}
EXPORT_SYMBOL(trace_mark_begin);

void trace_mark_end(unsigned char* args) {
	trace_mark("E", 0 /* not used */, "" /* not used */, 0 /* not used */, args);
}
EXPORT_SYMBOL(trace_mark_end);

void trace_mark_start(
		unsigned long ppid,
		const char* name,
		unsigned long cookie,
		const char* thread) {
	trace_mark("S", ppid, name, cookie, thread);
}
EXPORT_SYMBOL(trace_mark_start);

void trace_mark_finish(
		unsigned char* name,
		unsigned char* cookie) {
	trace_mark("F", 0 /* not used */, name, 0, cookie);
}
EXPORT_SYMBOL(trace_mark_finish); 
