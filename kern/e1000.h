#ifndef JOS_KERN_E1000_H
#define JOS_KERN_E1000_H

#include <inc/types.h>

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

#define E1000_STATUS   (0x00008/4)  /* Device Status - RO */

volatile uint32_t *e1000_bar0;      // memory mapped E1000 device registers

size_t e1000_transmit(const void *buffer, size_t size);
size_t e1000_receive(void *buffer, size_t size);


#endif  // SOL >= 6
