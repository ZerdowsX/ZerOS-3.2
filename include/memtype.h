#ifndef NUGGET_MEMTYPE_H
#define NUGGET_MEMTYPE_H
#include "types.h"

/* Marks the given physical range as write-combining via PAT (not global
   MTRRs), so writes to it - like the framebuffer - don't pay the cost of
   fully uncached access. Returns false and changes nothing if the range
   doesn't fit our simple identity-mapped setup. Worst case on failure is
   a hang requiring a reboot, never physical damage - CPUs don't have a
   "burn the silicon" failure mode for a memory-type misconfiguration. */
bool memtype_set_write_combining(uint64_t phys_addr, uint64_t len);

#endif
