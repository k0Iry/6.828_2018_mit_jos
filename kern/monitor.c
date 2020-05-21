// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>
#include <kern/pmap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display backtrace to current function call", mon_backtrace},
	{ "showmappings", "Display memory mappings in current active address space", mon_showmappings}
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	cprintf("Stack backtrace\n");

	uint32_t ebp = 0;
	uintptr_t eip = 0;
	asm volatile("movl %%ebp,%0" : "=r" (ebp)); // now should be after prologue

	while(ebp != 0) // in entry.S, ebp got initialized to 0, where we should stop
	{
		eip = (uintptr_t)*((uint32_t *)ebp + 1);
		struct Eipdebuginfo debuginfo = {NULL, 0, NULL, 0, 0,  0};
		if (debuginfo_eip(eip, &debuginfo) == -1)
			return -1;

		int fn_name_len = debuginfo.eip_fn_namelen;
		char fn_name[fn_name_len + 1];
		const char *eip_fn_name = debuginfo.eip_fn_name;
		for (int i = 0; i < fn_name_len; i ++)
		{
			fn_name[i] = eip_fn_name[i];
		}
		fn_name[fn_name_len] = '\0';
		cprintf("ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n\n  \t\t%s:%d: %s+%d\n",
			         ebp, eip, *((uint32_t *)ebp + 2), *((uint32_t *)ebp + 3), *((uint32_t *)ebp + 4), *((uint32_t *)ebp + 5), *((uint32_t *)ebp + 6),
			         debuginfo.eip_file, debuginfo.eip_line, fn_name, debuginfo.eip_fn_narg);
		ebp = *(uint32_t *)ebp;
	}

	return 0;
}

int mon_showmappings(int argc, char **argv, struct Trapframe *tf)
{
	if (argc != 3)
	{
		cprintf("usage: showmappings <low_addr> <high_addr>, addresses are virtual\n");
		return 1;
	}

	uintptr_t low_addr = (uintptr_t)strtol(argv[1], NULL, 16);
	uintptr_t high_addr = (uintptr_t)strtol(argv[2], NULL, 16);
	if (low_addr > high_addr)
		return 1;

	extern pde_t entry_pgdir[];
	cprintf("Show mappings between 0x%08x and 0x%08x\n", low_addr, high_addr);

	size_t range = (high_addr - low_addr) / PGSIZE;

	for (int i = 0; i <= range; i++)
	{
		pte_t *pgtbl_entry = NULL;
		const void *vir_addr = (const void *)(low_addr + i * PGSIZE);
		if (!(pgtbl_entry = pgdir_walk(entry_pgdir, vir_addr, 0)))
		{
			if (!(pgtbl_entry = pgdir_walk(kern_pgdir, vir_addr, 0)))
			{
				cprintf("Invalid mappings, perhaps accessing USER level, not supported yet\n");
				return 1;
			}
		}
		cprintf("\tVirtual address 0x%08x mapped to physical address 0x%08x\n", vir_addr, PTE_ADDR(*pgtbl_entry) + PGOFF(vir_addr));
	}

	return 0;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
