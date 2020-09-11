// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	if ((err & FEC_WR) != FEC_WR)
	{
		panic("Access to addr 0x%x is not writing, err code %d\n", addr, err);
	}
	if ((uvpt[PGNUM(addr)] & PTE_COW) != PTE_COW)
	{
		panic("Fault address 0x%x not marked as Copy-on-Write", addr);
	}

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
	// envid_t envid = sys_getenvid();
	// cprintf("fault addr 0x%x, envid %x\n", addr, envid);
	addr = ROUNDDOWN(addr, PGSIZE);	// page-size aligned

	if ((r = sys_page_alloc(0, PFTEMP, PTE_P | PTE_U | PTE_W)) != 0)
		panic("sys_page_alloc, %e", r);
	memmove((void *)PFTEMP, addr, PGSIZE);
	// remap the addr with newly allocated writable page
	if ((r = sys_page_map(0, PFTEMP, 0, addr, PTE_P | PTE_U | PTE_W)) != 0)
		panic("sys_page_map, %e, fault addr 0x%x", r, addr);
	if ((r = sys_page_unmap(0, PFTEMP)) != 0)
		panic("sys_page_unmap, %e", r);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Answer of above exercise question:
// *If the beginning it is CoW, and then it faults, marked as PTE_W, then
// we'll corrupt the child's copy when this page is being written by parent
// after this, and the 'snapshot' misfunction*
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.
	int perm = PTE_P | PTE_U;	// at least PTE_P and PTE_U

	int is_wr = (uvpt[pn] & PTE_W) == PTE_W;
	int is_cow = (uvpt[pn] & PTE_COW) == PTE_COW;
	int is_shared = (uvpt[pn] & PTE_SHARE);

	void *addr = (void *)(pn * PGSIZE);
	if ((is_wr || is_cow) && !is_shared)
	{
		// create new mapping
		if ((r = sys_page_map(0, addr, envid, addr, perm | PTE_COW)) != 0)
			panic("sys_page_map, %e", r);
		if ((r = sys_page_map(0, addr, 0, addr, perm | PTE_COW)) != 0)
			panic("sys_page_map, %e", r);
	}
	else
	{
		if (is_shared)
			perm = PTE_SYSCALL & uvpt[pn];
		// only remap child without PTE_COW
		if ((r = sys_page_map(0, addr, envid, addr, perm)) != 0)
			panic("sys_page_map, %e", r);
	}
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 4: Your code here.
	int r;
	uint8_t *addr;	// uint_8 * easier for calculation...
	set_pgfault_handler(pgfault);
	envid_t child = sys_exofork();
	if (child < 0)
		panic("sys_exofork, %e", child);
	if (child == 0)
	{
		// I am child, fix the 'thisenv'
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}

	// creating exception stack for child to run with,
	// child inherits 'pgfault' handler globally from parent
	// but still needs install 'pgfault_upcall' for child otherwise
	// when child is up and running, it will page fault on its runtime stack
	// because the child's pgdir doesn't map any page for (USTACKTOP - PGSIZE)
	// set_pgfault_handler(pgfault);

	if ((r = sys_page_alloc(child, (void *)(UXSTACKTOP - PGSIZE), PTE_P | PTE_U | PTE_W)) != 0)
		panic("sys_page_alloc, %e", r);
	extern void _pgfault_upcall(void);
	if ((r = sys_env_set_pgfault_upcall(child, _pgfault_upcall)) != 0)
		panic("sys_env_set_pgfault_upcall, %e", r);

	for (addr = 0; addr < (uint8_t *)(UTOP - PGSIZE); addr += PGSIZE)
	{
		if ((uvpd[PDX(addr)] & PTE_P) == PTE_P && (uvpt[PGNUM(addr)] & PTE_P) == PTE_P)
		{
			duppage(child, PGNUM(addr));
		}
	}

	// Start the child environment running
	if ((r = sys_env_set_status(child, ENV_RUNNABLE)) < 0)
		panic("sys_env_set_status: %e", r);

	return child;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
