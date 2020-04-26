/*
 * Copyright (C) 2008-2010 coresystems GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "DirectHW.h"

static int intel_setup_io(struct pci_access *a __attribute__((unused)))
{
	return (iopl(3) < 0) ? 0 : 1;
}

static void intel_cleanup_io(struct pci_access *a __attribute__((unused)))
{
	iopl(0);
}

#define intel_outb(x, y) outb(x, y)
#define intel_outw(x, y) outw(x, y)
#define intel_outl(x, y) outl(x, y)
#define intel_inb(x) inb(x)
#define intel_inw(x) inw(x)
#define intel_inl(x) inl(x)

static inline void intel_io_lock(void)
{
}

static inline void intel_io_unlock(void)
{
}
