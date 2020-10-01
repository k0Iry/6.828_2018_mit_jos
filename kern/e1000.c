#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>
#include <inc/string.h>

// LAB 6: Your driver code here

// divided by 4 for use as uint32_t[] indices.

// TX
#define E1000_TDBAL    (0x03800/4)  /* TX Descriptor Base Address Low - RW */
#define E1000_TDBAH    (0x03804/4)  /* TX Descriptor Base Address High - RW */
#define E1000_TDLEN    (0x03808/4)  /* TX Descriptor Length - RW */
#define E1000_TDH      (0x03810/4)  /* TX Descriptor Head - RW */
#define E1000_TDT      (0x03818/4)  /* TX Descripotr Tail - RW */

/* Transmit Control */
#define E1000_TCTL     (0x00400/4)  /* TX Control - RW */
    #define E1000_TCTL_EN     0x00000002    /* enable tx */
    #define E1000_TCTL_PSP    0x00000008    /* pad short packets */
    #define E1000_TCTL_COLD   0x003ff000    /* collision distance */

#define E1000_TIPG     (0x00410/4)  /* TX Inter-packet gap -RW */

#define E1000_TXD_CMD_RS     0x08000000 /* Report Status */
#define E1000_TXD_CMD_EOP    0x01000000 /* End of Packet */
#define E1000_TXD_STAT_DD    0x00000001 /* Descriptor Done */

// RX
#define E1000_RDBAL    (0x02800/4)  /* RX Descriptor Base Address Low - RW */
#define E1000_RDBAH    (0x02804/4)  /* RX Descriptor Base Address High - RW */
#define E1000_RDLEN    (0x02808/4)  /* RX Descriptor Length - RW */
#define E1000_RDH      (0x02810/4)  /* RX Descriptor Head - RW */
#define E1000_RDT      (0x02818/4)  /* RX Descriptor Tail - RW */

#define E1000_MTA      (0x05200/4)  /* Multicast Table Array - RW Array */
#define E1000_RAL      (0x05400/4)  /* Receive Address Low - RW Array */
#define E1000_RAH      (0x05404/4)  /* Receive Address High - RW Array */
#define E1000_RAH_AV  0x80000000        /* Receive descriptor valid */
#define E1000_IMS      (0x000D0/4)  /* Interrupt Mask Set - RW */

#define E1000_RXD_STAT_DD       0x01    /* Descriptor Done */
#define E1000_RXD_STAT_EOP      0x02    /* End of Packet */

/* Receive Control */
#define E1000_RCTL     (0x00100/4)  /* RX Control - RW */
    #define E1000_RCTL_EN             0x00000002    /* enable */
    #define E1000_RCTL_SZ_2048        0x00000000    /* rx buffer size 2048 */
    // #define E1000_RCTL_LPE            0x00000020    /* long packet enable */
    #define E1000_RCTL_LBM_NO         0x00000000    /* no loopback mode */
    #define E1000_RCTL_SECRC          0x04000000    /* Strip Ethernet CRC */
    #define E1000_RCTL_BAM            0x00008000    /* broadcast enable */

#define MTU 1518

// register descriptor layout, 64 descriptors, each with 16 byte
// 8 descriptors within a group, total 8 groups, 1KB ring buffer.
// needs to be 16-byte aligned
struct tx_desc
{
	uint64_t addr;  // only fill low 32 bits
	uint16_t length;
	uint8_t cso;
	uint8_t cmd;
	uint8_t status;
	uint8_t css;
	uint16_t special;
} __attribute__ ((aligned (16)));

/* Receive Descriptor */
struct rx_desc {
    uint64_t addr; /* Address of the descriptor's data buffer */
    uint16_t length;     /* Length of data DMAed into data buffer */
    uint16_t csum;       /* Packet checksum */
    uint8_t status;      /* Descriptor status */
    uint8_t errors;      /* Descriptor Errors */
    uint16_t special;
} __attribute__ ((aligned (16)));

struct tx_desc tdesc[64];  // TDESC ring buffer, max 64
struct rx_desc rdesc[128]; // RDESC ring buffer, min 128

static void init_tx()
{
    // allocating buffer space for each TDESC, a PAGE can hold 2 MTU
    size_t tdesc_length = sizeof(tdesc) / sizeof(struct tx_desc);
    for (int i = 0; i < tdesc_length; i+=2)
    {
        tdesc[i].addr = page2pa(page_alloc(ALLOC_ZERO));
        tdesc[i].cmd |= (E1000_TXD_CMD_RS >> 24); // set RS bit to report status of each descriptor
        tdesc[i].status |= E1000_TXD_STAT_DD; // enable DD bit by default, clear when transmitting
        
        // assign another half to the second one
        tdesc[i + 1].addr = tdesc[i].addr + PGSIZE / 2;
        tdesc[i + 1].cmd |= (E1000_TXD_CMD_RS >> 24);
        tdesc[i + 1].status |= E1000_TXD_STAT_DD;
    }

    // perform initialization in Chapter 14.5, for TX
    e1000_bar0[E1000_TDBAH] = 0;     // high 32 bit is cleared
    e1000_bar0[E1000_TDBAL] = PADDR(tdesc);     // base address as descriptor array's physical address
    e1000_bar0[E1000_TDLEN ] = sizeof(tdesc);    // total 1024 bytes
    e1000_bar0[E1000_TDH] = 0x0;        // initialized as 0, Hardware is responsible to update this
    e1000_bar0[E1000_TDT] = 0x0;        // initialized as 0, Software is responsible to update this
    e1000_bar0[E1000_TCTL] = ((0x40 << 12) & E1000_TCTL_COLD) | E1000_TCTL_PSP | E1000_TCTL_EN;    // enable TX, full-duplex operation
    e1000_bar0[E1000_TIPG] = 10;        // IEEE 802.3 standard IPG

    /* self test with overflow and transmission*/
    // char *sample = "hello, world, let's transmit something\n";
    // for (int i = 0; i < 142; i++)
    //     e1000_transmit(sample, strlen(sample));
}

static void init_rx()
{
    size_t rdesc_length = sizeof(rdesc) / sizeof(struct rx_desc);
    for (int i = 0; i < rdesc_length; i+=2)
    {
        rdesc[i].addr = page2pa(page_alloc(ALLOC_ZERO));
        rdesc[i + 1].addr = rdesc[i].addr + PGSIZE / 2;
    }
    
    e1000_bar0[E1000_RDBAH] = 0;
    e1000_bar0[E1000_RDBAL] = PADDR(rdesc);
    e1000_bar0[E1000_RDLEN] = sizeof(rdesc);
    e1000_bar0[E1000_RDH] = 0;
    // To prevent the index registers to wrap around, the OS always leaves one RX descriptor unused
    e1000_bar0[E1000_RDT] = rdesc_length - 1;
    // MAC address of QEMU's ethernet card
    e1000_bar0[E1000_RAL] = 0x12005452;
    *(uint16_t *)(e1000_bar0 + E1000_RAH) = 0x5634;
    e1000_bar0[E1000_RAH] |= E1000_RAH_AV;
    // 128 bit of MTA init to 0b
    e1000_bar0[E1000_MTA] = 0;
    e1000_bar0[E1000_MTA + 1] = 0;
    e1000_bar0[E1000_MTA + 2] = 0;
    e1000_bar0[E1000_MTA + 3] = 0;
    // not enable IRQ for now
    e1000_bar0[E1000_IMS] = 0;
    e1000_bar0[E1000_RCTL] = E1000_RCTL_EN | E1000_RCTL_SECRC | E1000_RCTL_SZ_2048 | E1000_RCTL_BAM;
}

//  wrap pci_func_enable function
int pci_func_attach(struct pci_func *pcif)
{
    pci_func_enable(pcif);

    init_tx();
    init_rx();

    return 0;
}

/*
 * transmit a packet
 * 
 * return number of bytes transmitted
 * return 0 if this packet needs re-transmission
*/
size_t e1000_transmit(const void *buffer, size_t size)
{
    uint32_t current = e1000_bar0[E1000_TDT];
    if (tdesc[current].status & E1000_TXD_STAT_DD)
    {
        tdesc[current].status &= ~E1000_TXD_STAT_DD;
        void *addr = (void *)KADDR((uint32_t)tdesc[current].addr);
        size_t length = MIN(size, MTU);
        memcpy(addr, buffer, length);
        tdesc[current].cmd |= (E1000_TXD_CMD_EOP >> 24);      // End of Packet
        tdesc[current].length = length;
        // update tail pointer, inform network card
        uint32_t next = current + 1;
        e1000_bar0[E1000_TDT] = next % (sizeof(tdesc) / sizeof(struct tx_desc));
        return length;
    }
    // require for re-transmission
    cprintf("lost packet 0x%x\n", buffer);
    return 0;
}

/*
 * receive a packet
 *
 * return number of bytes from oldest buffer
 * return 0 if this packet needs to wait
 * return -1 if this packet is too long (more space)
*/
size_t e1000_receive(void *buffer, size_t size)
{
    // to receive packet, it should start beyond RDT
    uint32_t current = (e1000_bar0[E1000_RDT] + 1) % (sizeof(rdesc) / sizeof(struct rx_desc));
    if (!(rdesc[current].status & E1000_RXD_STAT_DD))
    {
        // RTH == RTD: buffer is empty, stop receiving
        return 0;
    }

    uint32_t length = rdesc[current].length;
    if (size < length)
        return -1;
    memcpy(buffer, (const void*)(KADDR((uint32_t)rdesc[current].addr)), length);
    // zero the status byte in the descriptor to make it ready for reuse by hardware
    rdesc[current].status &= ~E1000_RXD_STAT_DD;
    // update tail, letting card know there is one more RDESC allocated for use
    e1000_bar0[E1000_RDT] = current;
    
    return length;
}