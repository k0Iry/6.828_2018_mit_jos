/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>
#include <kern/time.h>
#include <kern/e1000.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.

	// LAB 3: Your code here.
	user_mem_assert(curenv, s, len, PTE_U);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

	// LAB 4: Your code here.
	struct Env *child = NULL;
	int ret = 0;
	if ((ret = env_alloc(&child, curenv->env_id)) != 0)
		return ret;
	child->env_status = ENV_NOT_RUNNABLE;
	child->env_tf = curenv->env_tf;
	child->env_tf.tf_regs.reg_eax = 0;

	return child->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	// LAB 4: Your code here.
	if (status != ENV_NOT_RUNNABLE && status != ENV_RUNNABLE)
		return -E_INVAL;

	struct Env *env = NULL;
	int ret = 0;
	if ((ret = envid2env(envid, &env, 1)) != 0)
		return ret;

	env->env_status = status;
	return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3), interrupts enabled, and IOPL of 0.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// LAB 5: Your code here.
	// Remember to check whether the user has supplied us with a good
	// address!
	int ret = 0;
	struct Env *env = NULL;
	if ((ret = envid2env(envid, &env, 1)) != 0)
		return ret;

	// copy the trapframe
	user_mem_assert(env, tf, sizeof(struct Trapframe), PTE_U);
	env->env_tf = *tf;
	env->env_tf.tf_cs = GD_UT | 3;
	env->env_tf.tf_eflags |= FL_IF;
	env->env_tf.tf_eflags &= ~FL_IOPL_MASK;
	return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	// LAB 4: Your code here.
	struct Env *env = NULL;
	if (envid2env(envid, &env, 1) != 0)
		return -E_BAD_ENV;
	env->env_pgfault_upcall = func;
	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!

	// LAB 4: Your code here.
	struct Env *env = NULL;
	if (envid2env(envid, &env, 1) != 0)
		return -E_BAD_ENV;
	if ((uintptr_t)va >= UTOP || (uintptr_t)va % PGSIZE)
		return -E_INVAL;
	if ((perm & PTE_P) != PTE_P || (perm & PTE_U) != PTE_U || (perm & ~PTE_SYSCALL) != 0)
		return -E_INVAL;

	struct PageInfo *page = page_alloc(ALLOC_ZERO);
	if (page == NULL)
		return -E_NO_MEM;
	if (page_insert(env->env_pgdir, page, va, perm) != 0)
	{
		page_free(page);
		return -E_NO_MEM;
	}

	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.
	struct Env *srcenv, *dstenv;
	if (envid2env(srcenvid, &srcenv, 1) != 0 || envid2env(dstenvid, &dstenv, 1) != 0)
		return -E_BAD_ENV;
	if ((uintptr_t)srcva >= UTOP || (uintptr_t)dstva >= UTOP || (uintptr_t)srcva % PGSIZE || (uintptr_t)dstva % PGSIZE)
		return -E_INVAL;

	pte_t *src_pgtbl_entry = NULL;
	struct PageInfo *src_page = page_lookup(srcenv->env_pgdir, srcva, &src_pgtbl_entry);
	if (src_page == NULL || (perm & PTE_P) != PTE_P || (perm & PTE_U) != PTE_U || (perm & ~PTE_SYSCALL) != 0)
		return -E_INVAL;
	if ((perm & PTE_W) == PTE_W && (*src_pgtbl_entry & PTE_W) != PTE_W)
		return -E_INVAL;
	return page_insert(dstenv->env_pgdir, src_page, dstva, perm);
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().

	// LAB 4: Your code here.
	struct Env *env = NULL;
	if (envid2env(envid, &env, 1) != 0)
		return -E_BAD_ENV;
	if ((uintptr_t)va >= UTOP || (uintptr_t)va % PGSIZE)
		return -E_INVAL;
	page_remove(env->env_pgdir, va);
	return 0;
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.

// to make ipc_send not in a loop, I think we need to implement a coroutine
// to speed-up kernel tasks, e.g. if envid is not ready for recv, we may need
// switch to scheduler and run other tasks, we can jump back later(but not in user
// space, so we need to persist kernel stack for later use)
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	// LAB 4: Your code here.
	struct Env *env = NULL;
	int r = 0;
	struct PageInfo *pp = NULL;
	pte_t *pgtbl_entry = NULL;
	if ((r = envid2env(envid, &env, 0)) != 0)
		return r;
	if (env->env_ipc_recving == 0)
		return -E_IPC_NOT_RECV;
	if ((uintptr_t)srcva < UTOP && (uintptr_t)srcva % PGSIZE)
		return -E_INVAL;
	if ((uintptr_t)srcva < UTOP && ((perm & PTE_P) != PTE_P || (perm & PTE_U) != PTE_U || (perm & ~PTE_SYSCALL) != 0))
		return -E_INVAL;
	if ((uintptr_t)srcva < UTOP && (pp = page_lookup(curenv->env_pgdir, srcva, &pgtbl_entry)) == NULL)
		return -E_INVAL;
	if ((uintptr_t)srcva < UTOP && (perm & PTE_W) && !(*pgtbl_entry & PTE_W))
		return -E_INVAL;

	env->env_ipc_perm = 0;
	if ((uintptr_t)srcva < UTOP && (uintptr_t)(env->env_ipc_dstva) < UTOP)
	{
		// send a page, install it in receiver's address space
		if ((r = page_insert(env->env_pgdir, pp, env->env_ipc_dstva, perm)) != 0)
			return r;
		env->env_ipc_perm = perm;	// update perm if there is actually a page being transferred
	}
	env->env_ipc_recving = 0;
	env->env_ipc_from = curenv->env_id;
	env->env_ipc_value = value;
	env->env_status = ENV_RUNNABLE;
	env->env_tf.tf_regs.reg_eax = 0;	// next time receiver being woken-up, it returns 0 and got the value or page it needs

	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	// LAB 4: Your code here.
	if ((uintptr_t)dstva < UTOP && (uintptr_t)dstva % PGSIZE)
		return -E_INVAL;
	curenv->env_ipc_recving = 1;	// ready for receiving something
	curenv->env_ipc_dstva = dstva;	// tell sender if we want a page

	// now we give up CPU
	curenv->env_status = ENV_NOT_RUNNABLE;
	sched_yield();

	return 0;
}

// Return the current time.
static int
sys_time_msec(void)
{
	// LAB 6: Your code here.
	return time_msec();
}

static void
sys_ide_sleep(void *chan, size_t nsecs, int op)
{
	int r;
	if (op == 0)
	{
		outb(0x1F7, nsecs > 1 ? 0xc4 : 0x20);	// CMD 0x20 means read sector
	}
	else
	{
		outb(0x1F7, nsecs > 1 ? 0xc5 : 0x30);	// CMD 0x30 means write sector
		outsl(0x1F0, chan, PGSIZE / 4);
	}
	curenv->chan = chan;
	curenv->env_status = ENV_IDE_SLEEPING;
	curenv->op = op;
	sched_yield();
}

static int
sys_send(const void *buffer, size_t length)
{
	user_mem_assert(curenv, buffer, length, PTE_U);
	return (int)e1000_transmit(buffer, length);
}

static int
sys_recv(void *buffer, size_t length)
{
	user_mem_assert(curenv, buffer, length, PTE_U);
	return (int)e1000_receive(buffer, length);
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	// LAB 3: Your code here.

	// panic("syscall not implemented");

	switch (syscallno) {
	case SYS_cputs:
		sys_cputs((const char *)a1, a2);
		return 0;
	case SYS_cgetc:
		return sys_cgetc();
	case SYS_getenvid:
		return sys_getenvid();
	case SYS_env_destroy:
		return sys_env_destroy(a1);
	case SYS_page_alloc:
		return sys_page_alloc(a1, (void *)a2, a3);
	case SYS_page_map:
		return sys_page_map(a1, (void *)a2, a3, (void *)a4, a5);
	case SYS_page_unmap:
		return sys_page_unmap(a1, (void *)a2);
	case SYS_exofork:
		return sys_exofork();
	case SYS_env_set_status:
		return sys_env_set_status(a1, a2);
	case SYS_env_set_trapframe:
		return sys_env_set_trapframe(a1, (struct Trapframe *)a2);
	case SYS_env_set_pgfault_upcall:
		return sys_env_set_pgfault_upcall(a1, (void *)a2);
	case SYS_yield:
		sys_yield();
	case SYS_ipc_try_send:
		return sys_ipc_try_send(a1, a2, (void *)a3, a4);
	case SYS_ipc_recv:
		return sys_ipc_recv((void *)a1);
	case SYS_time_msec:
		return sys_time_msec();
	case SYS_ide_sleep:
		sys_ide_sleep((void *)a1, a2, (int)a3);
	case SYS_send:
		return sys_send((const void*)a1, a2);
	case SYS_recv:
		return sys_recv((void *)a1, a2);
	case NSYSCALLS:
	default:
		return -E_INVAL;
	}
}

