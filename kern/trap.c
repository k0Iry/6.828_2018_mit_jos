#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>
#include <kern/time.h>
#include <kern/e1000.h>

static struct Taskstate ts;

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};

typedef void (*trap_handler)(void);

static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < ARRAY_SIZE(excnames))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}


void
trap_init(void)
{
	extern struct Segdesc gdt[];
	extern trap_handler default_handlers[];

	// LAB 3: Your code here.

	int vector = 0;
	for (; vector < 32; vector++)
	{
		// 0-31 for TRAPs
		if (vector == T_BRKPT)
		{
			// breakpoint exception should be able to trigger under CPL=3 (user mode)
			SETGATE(idt[vector], 0, GD_KT, default_handlers[vector], gdt[GD_UT >> 3].sd_dpl);
			continue;
		}
		// so we reset IF flag
		// e.g. when we enable IRQ for user environment, we first got trapped in irq_x,
		// and we don't clear IF, then we might got another irq_y when handling irq_x
		SETGATE(idt[vector], 0, GD_KT, default_handlers[vector], gdt[GD_KT >> 3].sd_dpl);
	}
	for (; vector < 256; vector++)
	{
		// 32 - 255 for IRQs (user defined)
		SETGATE(idt[vector], 0, GD_KT, default_handlers[vector], gdt[GD_UT >> 3].sd_dpl);
	}

	// Per-CPU setup 
	trap_init_percpu();
}

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu(void)
{
	// The example code here sets up the Task State Segment (TSS) and
	// the TSS descriptor for CPU 0. But it is incorrect if we are
	// running on other CPUs because each CPU has its own kernel stack.
	// Fix the code so that it works for all CPUs.
	//
	// Hints:
	//   - The macro "thiscpu" always refers to the current CPU's
	//     struct CpuInfo;
	//   - The ID of the current CPU is given by cpunum() or
	//     thiscpu->cpu_id;
	//   - Use "thiscpu->cpu_ts" as the TSS for the current CPU,
	//     rather than the global "ts" variable;
	//   - Use gdt[(GD_TSS0 >> 3) + i] for CPU i's TSS descriptor;
	//   - You mapped the per-CPU kernel stacks in mem_init_mp()
	//   - Initialize cpu_ts.ts_iomb to prevent unauthorized environments
	//     from doing IO (0 is not the correct value!)
	//
	// ltr sets a 'busy' flag in the TSS selector, so if you
	// accidentally load the same TSS on more than one CPU, you'll
	// get a triple fault.  If you set up an individual CPU's TSS
	// wrong, you may not get a fault until you try to return from
	// user space on that CPU.
	//
	// LAB 4: Your code here:
	uintptr_t kstacktop_percpu = (uintptr_t)percpu_kstacks[thiscpu->cpu_id] + KSTKSIZE;

	extern void sysenter_handler();
	wrmsr(MSR_IA32_SYSENTER_CS, GD_KT, 0);		// set (CPL = 0) CS & SS
	wrmsr(MSR_IA32_SYSENTER_EIP, (uint32_t)sysenter_handler, 0);		// the sysenter handler address
	wrmsr(MSR_IA32_SYSENTER_ESP, kstacktop_percpu, 0);	// the stack where we drop in when trapped into kernel

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	thiscpu->cpu_ts.ts_esp0 = kstacktop_percpu;
	thiscpu->cpu_ts.ts_ss0 = GD_KD;
	thiscpu->cpu_ts.ts_iomb = (uint16_t)0xFFFF;

	// Initialize the TSS slot of the gdt.
	gdt[(GD_TSS0 >> 3) + thiscpu->cpu_id] = SEG16(STS_T32A, (uint32_t) (&thiscpu->cpu_ts),
					sizeof(struct Taskstate) - 1, 0);
	gdt[(GD_TSS0 >> 3) + thiscpu->cpu_id].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0 + (thiscpu->cpu_id << 3)); // see env gdt init

	// Load the IDT
	lidt(&idt_pd);
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
			tf->tf_err & 4 ? "user" : "kernel",
			tf->tf_err & 2 ? "write" : "read",
			tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	if ((tf->tf_cs & 3) != 0) {
		cprintf("  esp  0x%08x\n", tf->tf_esp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions.
	// LAB 3: Your code here.
	if (tf->tf_trapno == T_PGFLT)
		page_fault_handler(tf);
	if (tf->tf_trapno == T_BRKPT)
	{
		// enable single-step mode for debugging,
		// a debug exception will be generated
		// after each instruction
		tf->tf_eflags |= 0x100;
		monitor(tf);
	}
	if (tf->tf_trapno == T_DEBUG)
		monitor(tf);
	if (tf->tf_trapno == T_SYSCALL)
	{
		tf->tf_regs.reg_eax = syscall(tf->tf_regs.reg_eax, tf->tf_regs.reg_edx, tf->tf_regs.reg_ecx,
				tf->tf_regs.reg_ebx, tf->tf_regs.reg_edi, tf->tf_regs.reg_esi);
		return;
	}

	// Handle spurious interrupts
	// The hardware sometimes raises these because of noise on the
	// IRQ line or other reasons. We don't care.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SPURIOUS) {
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		return;
	}

	// Handle clock interrupts. Don't forget to acknowledge the
	// interrupt using lapic_eoi() before calling the scheduler!
	// LAB 4: Your code here.
	if (tf->tf_trapno >= IRQ_OFFSET && tf->tf_trapno < IRQ_OFFSET + 16)
	{
		switch (tf->tf_trapno)
		{
		case IRQ_OFFSET + IRQ_TIMER:
			time_tick();
			lapic_eoi();
			sched_yield();
			break;

		case IRQ_OFFSET + IRQ_KBD:
			kbd_intr();
			lapic_eoi();
			return;

		case IRQ_OFFSET + IRQ_SERIAL:
			serial_intr();
			lapic_eoi();
			return;

		case IRQ_OFFSET + IRQ_IDE:
			for (int i = 0; i < NENV; i++)
			{
				if (envs[i].env_type == ENV_TYPE_FS)
				{
					// read a BLKSIZE from disk if needed then acknowledge the interrupt
					if (envs[i].op == 0)
					{
						lcr3(PADDR(envs[i].env_pgdir));
						insl(0x1F0, envs[i].chan, PGSIZE / 4);
						envs[i].chan = 0;
						lcr3(PADDR(kern_pgdir));
					}
					// OCW2: send non-specific EOI command to give driver an ACK
					// otherwise we won't receive the rest IDE interrupts followed
					outb(IO_PIC1, 0x20);
					outb(IO_PIC2, 0x20);
					// finally, make fs runnable
					if (envs[i].env_status == ENV_IDE_SLEEPING)
						envs[i].env_status = ENV_RUNNABLE;
					else
					{
						// shouldn't be here
						cprintf("status: %u\n", envs[i].env_status);
						print_trapframe(tf);
					}
					return;
				}
			}
		
		case IRQ_OFFSET + 11:
		    e1000_intr();
			lapic_eoi();
			return;
		
		default:
			break;
		}
	}

	// Add time tick increment to clock interrupts.
	// Be careful! In multiprocessors, clock interrupts are
	// triggered on every CPU.
	// LAB 6: Your code here.

	// Unexpected trap: The user process or the kernel has a bug.
	print_trapframe(tf);
	if (tf->tf_cs == GD_KT)
		panic("unhandled trap in kernel");
	else {
		env_destroy(curenv);
		return;
	}
}

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");

	// Halt the CPU if some other CPU has called panic()
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// Re-acqurie the big kernel lock if we were halted in
	// sched_yield()
	if (xchg(&thiscpu->cpu_status, CPU_STARTED) == CPU_HALTED)
		lock_kernel();
	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		// Acquire the big kernel lock before doing any
		// serious kernel work.
		// LAB 4: Your code here.
		lock_kernel();

		// Question 2: Why do we still need separate kernel stacks for each CPU?
		// Answer: 
		// 1. CPU 0 got interrupted into the kernel from user space, it will push tf_0 on single stack 
		// 2. CPU 1 got interrupted too, then tf_1 is pushed on stack, and wait for irq_0 return (CPU 0 holding the lock)
		// 3. irq_0 return, it will pop tf_1 out and try to restore user state, but what it should pop is tf_0
		// more: https://stackoverflow.com/a/13953815/6289529

		assert(curenv);

		// Garbage collect if current enviroment is a zombie
		if (curenv->env_status == ENV_DYING) {
			env_free(curenv);
			curenv = NULL;
			sched_yield();
		}

		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.

		// The reason for this copy: difference from xv6
		// in xv6, each process(env) has its own kernel stack, so each process(env) can
		// always rely on its own stack even during task switch, since task switch always switch kernel stack.

		// But in JOS here, each CPU got its own kernel stack instead of each process(env), process(env) only keeps a
		// *snapshot* of trapframe when it gets trapped into the kernel, so that kernel
		// can freely do task switch without worrying about the stack switch (we do switch always in env_run()).
		// If we don't do this copy, only keep the pointer, when a timer interrupt comes in, the kernel stack would be
		// replaced with another process's state, once we re-run the previous one, it will use the state from another
		// process(env), which definitely is wrong!
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}

	// Record that tf is the last real trapframe so
	// print_trapframe can print some additional information.
	last_tf = tf;

	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING)
		env_run(curenv);
	else
		sched_yield();
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

	// LAB 3: Your code here.
	if (tf->tf_cs == GD_KT)
	{
		print_trapframe(tf);
		panic("page fault happens in kernel mode");
	}

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// It is convenient for our code which returns from a page fault
	// (lib/pfentry.S) to have one word of scratch space at the top of the
	// trap-time stack; it allows us to more easily restore the eip/esp. In
	// the non-recursive case, we don't have to worry about this because
	// the top of the regular user stack is free.  In the recursive case,
	// this means we have to leave an extra word between the current top of
	// the exception stack and the new stack frame because the exception
	// stack _is_ the trap-time stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').

	// LAB 4: Your code here.
	if (curenv->env_pgfault_upcall == NULL)
		goto end;

	struct UTrapframe *utf = (struct UTrapframe *)UXSTACKTOP;
	if (ROUNDUP(tf->tf_esp, PGSIZE) == UXSTACKTOP)
	{
		// recursive user exception
		uint32_t *empty_word = (uint32_t *)(tf->tf_esp - 4);
		*empty_word = 0;
		utf = (struct UTrapframe *)empty_word;
	}
	utf -= 1; // reserve space for UTrapframe
	// before we actually make any writing, we check the memory
	//
	// Note: we are currently under kernel mode, if we fail below
	// memory access, we end up with a fault in kernel mode, which
	// definitely panic the kernel, so we have to be very careful,
	// we cannot check too early or too late
	user_mem_assert(curenv, utf, sizeof(struct UTrapframe), PTE_W);
	utf->utf_fault_va = fault_va;
	utf->utf_err = tf->tf_err;
	utf->utf_regs = tf->tf_regs;
	utf->utf_eip = tf->tf_eip;
	utf->utf_eflags = tf->tf_eflags;
	utf->utf_esp = tf->tf_esp;

	// page fault exception handler thread
	tf->tf_esp = (uintptr_t)&utf->utf_fault_va;
	tf->tf_eip = (uintptr_t)curenv->env_pgfault_upcall;
	env_run(curenv);
end:
	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

