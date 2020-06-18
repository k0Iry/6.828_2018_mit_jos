/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>

#include <kern/picirq.h>


###################################################################
# exceptions/interrupts
###################################################################

# https://stackoverflow.com/questions/48159005/for-loop-macro-preprocessor-for-assembly-files-in-gcc/48170044#48170044
.altmacro

.macro irq_stubX trapno, noec
    irq\trapno:
.if noec == $1
    pushl $0
.endif
    pushl $\trapno
    jmp _alltraps
.endm

.macro irq_insertX trapno, noec
	.section .text
	.align 2;
    irq_stubX \trapno, \noec
    .section .data	# construct a function poiner array
    .long irq\trapno
.endm

/*
 * Lab 3: Your code here for generating entry points for the different traps.
 */
.section .data
.globl default_handlers;
default_handlers:
.set i,0
.rept 256
.if i >= 18 || i <= 7 || i == 15 || i == 16
    irq_insertX %i, $1
.else
     irq_insertX %i, $0
.endif
     .set i, i+1
.endr

.text
.globl sysenter_handler
.type sysenter_handler, @function
.align 2
# bypass trap to reach syscall directly
sysenter_handler:
pushl %edi
pushl %ebx
pushl %ecx
pushl %edx
pushl %eax
# only support 4 syscall arguments
call syscall
# movl $0x174, %ecx
# movl $0, %edx
# movl $(GD_UT), %eax	/* no need, because of continuity, GD_UT will be found by adding 16(0x10) to GD_KT */
# wrmsr
movl %esi, %edx
movl %ebp, %ecx
sysexit

/*
 * Lab 3: Your code here for _alltraps
 */
.globl _alltraps;
_alltraps:
# push all rest registers on stack
pushl %ds
pushl %es
pushal

# Set up data segments.
movw $(GD_KD), %ax
movw %ax, %ds
movw %ax, %es

# Call trap(tf), where tf=%esp
pushl %esp
call trap

# trap needs return back to user space
# pop registers and return from interrupt (iret)

