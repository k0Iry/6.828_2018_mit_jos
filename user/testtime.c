#include <inc/lib.h>
#include <inc/x86.h>

void
sleep(int sec)
{
	unsigned now = sys_time_msec();
	unsigned end = now + sec * 1000;

	if ((int)now < 0 && (int)now > -MAXERROR)
		panic("sys_time_msec: %e", (int)now);
	if (end < now)
		panic("sleep: wrap");

	while (sys_time_msec() < end)
		sys_yield();
}

void
umain(int argc, char **argv)
{
	int i;

	// Wait for the console to calm down
	// since I enabled disk interrupt, so count down will not be
	// continous sometimes, so increase the yielding times
	for (i = 0; i < 1000; i++)
		sys_yield();

	cprintf("starting count down: ");
	for (i = 5; i >= 0; i--) {
		cprintf("%d ", i);
		sleep(1);
	}
	cprintf("\n");
	breakpoint();
}
