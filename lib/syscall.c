// System call stubs.

#include <inc/syscall.h>
#include <inc/lib.h>

static inline int32_t
syscall(int num, int check, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	int32_t ret;

	// Generic system call: pass system call number in AX,
	// up to five parameters in DX, CX, BX, DI, SI.
	// Interrupt kernel with T_SYSCALL.
	//
	// The "volatile" tells the assembler not to optimize
	// this instruction away just because we don't use the
	// return value.
	//
	// The last clause tells the assembler that this can
	// potentially change the condition codes and arbitrary
	// memory locations.

	asm volatile("int %1\n"
		     : "=a" (ret)
		     : "i" (T_SYSCALL),
		       "a" (num),
		       "d" (a1),
		       "c" (a2),
		       "b" (a3),
		       "D" (a4),
		       "S" (a5)
		     : "cc", "memory");

	if(check && ret > 0)
		panic("syscall %d returned %d (> 0)", num, ret);

	return ret;
}

static inline int32_t
sysenter(int num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
	// fast system call: pass system call number in AX,
	// up to 4 parameters in DX, CX, BX, DI
	//
	// Interrupt kernel with MSR (CPL = 0).
	//
	// https://reverseengineering.stackexchange.com/questions/2869/how-to-use-sysenter-under-linux

	// we also need to lock kernel here

    int32_t ret;
	asm volatile(
		        "pushl %%ebp\n\t"
		        "movl  %%esp, %%ebp\n\t"
		        "leal  sysenter_ret%=, %%esi\n\t"
		        "sysenter\n\t"
		        "sysenter_ret%=:"
		        "popl %%ebp\n\t"
		        : "=a" (ret) :
		            "a" (num),
		            "d" (a1),
		            "c" (a2),
		            "b" (a3),
		            "D" (a4)
		        : "%esi", "memory", "cc");

	return ret;
}

void
sys_cputs(const char *s, size_t len)
{
	sysenter(SYS_cputs, (uint32_t)s, len, 0, 0);
}

int
sys_cgetc(void)
{
	return sysenter(SYS_cgetc, 0, 0, 0, 0);
}

int
sys_env_destroy(envid_t envid)
{
	return sysenter(SYS_env_destroy, envid, 0, 0, 0);
}

envid_t
sys_getenvid(void)
{
	return sysenter(SYS_getenvid, 0, 0, 0, 0);
}

void
sys_yield(void)
{
	// Question 4: Whenever the kernel switches from one environment to another, it must ensure the old environment's
	// registers are saved so they can be restored properly later. Why? Where does this happen?
	//
	// Answer: the kernel switches from one env to another by calling 'sys_yield', which will use 'int 0x30' to save
	// current env status in kernel stack, next time, it will use env_run to restore
	//
	// Note: this system call cannot be implemented by 'sysenter', since it needs to record any update of trapframe,
	// 'sysenter' will not be able to do so
	syscall(SYS_yield, 0, 0, 0, 0, 0, 0);
}

int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	return sysenter(SYS_page_alloc, envid, (uint32_t) va, perm, 0);
}

int
sys_page_map(envid_t srcenv, void *srcva, envid_t dstenv, void *dstva, int perm)
{
	// sysenter doesn't support 5 arguments
	return syscall(SYS_page_map, 1, srcenv, (uint32_t) srcva, dstenv, (uint32_t) dstva, perm);
}

int
sys_page_unmap(envid_t envid, void *va)
{
	return sysenter(SYS_page_unmap, envid, (uint32_t) va, 0, 0);
}

// sys_exofork is inlined in lib.h

int
sys_env_set_status(envid_t envid, int status)
{
	return sysenter(SYS_env_set_status, envid, status, 0, 0);
}

int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	return syscall(SYS_env_set_trapframe, 1, envid, (uint32_t) tf, 0, 0, 0);
}

int
sys_env_set_pgfault_upcall(envid_t envid, void *upcall)
{
	return sysenter(SYS_env_set_pgfault_upcall, envid, (uint32_t) upcall, 0, 0);
}

int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, int perm)
{
	return sysenter(SYS_ipc_try_send, envid, value, (uint32_t) srcva, perm);
}

int
sys_ipc_recv(void *dstva)
{
	// cannot use sysenter, because we need to access the trapframe
	return syscall(SYS_ipc_recv, 1, (uint32_t)dstva, 0, 0, 0, 0);
}

unsigned int
sys_time_msec(void)
{
	return (unsigned int) syscall(SYS_time_msec, 0, 0, 0, 0, 0, 0);
}
