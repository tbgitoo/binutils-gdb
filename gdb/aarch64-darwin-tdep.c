/* Darwin support for GDB, the GNU debugger.
   Copyright (C) 1997-2025 Free Software Foundation, Inc.

   Contributed by Can Acar.
   Based on i386-darwin-tdep.c

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "frame.h"
#include "inferior.h"
#include "regcache.h"
#include "osabi.h"
#include "aarch64-tdep.h"
#include "solib-darwin.h"
#include "dwarf2/frame.h"


static void
aarch64_darwin_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  aarch64_gdbarch_tdep *tdep = gdbarch_tdep<aarch64_gdbarch_tdep> (gdbarch);
  // TODO...
  set_gdbarch_make_solib_ops (gdbarch, make_darwin_solib_ops);
}

static enum gdb_osabi
aarch64_mach_o_osabi_sniffer (bfd *abfd)
{
  if (!bfd_check_format (abfd, bfd_object))
    return GDB_OSABI_UNKNOWN;

  if (bfd_get_arch (abfd) == bfd_arch_aarch64)
    return GDB_OSABI_DARWIN;

  return GDB_OSABI_UNKNOWN;
}

INIT_GDB_FILE (aarch64_darwin_tdep)
{
  gdbarch_register_osabi_sniffer (bfd_arch_unknown, bfd_target_mach_o_flavour,
                                  aarch64_mach_o_osabi_sniffer);
  gdbarch_register_osabi (bfd_arch_aarch64, bfd_mach_aarch64, GDB_OSABI_DARWIN,
                          aarch64_darwin_init_abi);
}
