#include "ns.h"

extern union Nsipc nsipcbuf;

void
output(envid_t ns_envid)
{
	binaryname = "ns_output";

	// LAB 6: Your code here:
	// 	- read a packet from the network server
	//	- send the packet to the device driver
	int32_t reqno;
	uint32_t whom;
	int perm;
	int r;

	while (1)
	{
		reqno = ipc_recv((int32_t *) &whom, (void *)&nsipcbuf, &perm);
		char *ptr = nsipcbuf.pkt.jp_data;
		size_t total = (size_t)nsipcbuf.pkt.jp_len;
		if (reqno == NSREQ_OUTPUT)
		{
		retry:
			while ((r = sys_send((const void*)ptr, total)) == 0) sys_yield();
			if (r < total)
			{
				ptr += r;
				total -= r;
				cprintf("Sent %d bytes, remaining %d bytes to send\n", r, total);
				goto retry;
			}
		}
	}
}
