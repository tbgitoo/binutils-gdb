/* Inferior stdout unbuffering for IDE debug sessions.
   Copyright (C) 2026 Free Software Foundation, Inc.

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

#include "breakpoint.h"
#include "cli/cli-cmds.h"
#include "cli/cli-script.h"
#include "command.h"
#include "extract-store-integer.h"
#include "gdbarch.h"
#include "gdbcore.h"
#include "inferior.h"
#include "minsyms.h"
#include "objfiles.h"
#include "symtab.h"
#include "target.h"
#include "terminal.h"
#include "value.h"

#include <cstdint>

/* When true, run/start arrange a one-shot stop at main that makes the
   inferior's C stdout unbuffered before returning control to the IDE.
   Default off so a plain shell gdb never injects.  */
bool inferior_stdio_unbuffered = false;

static void
show_inferior_stdio_unbuffered (struct ui_file *file, int from_tty,
				struct cmd_list_element *c, const char *value)
{
  gdb_printf (file,
	      _("Unbuffering inferior stdout at main is %s.\n"),
	      value);
}

/* Darwin libc FILE layout offsets (arm64 / x86_64 macOS SDK; sizeof FILE == 152).
   Confirmed against _stdio.h and a live setvbuf(_IONBF) dump.  */
enum : CORE_ADDR
{
  DARWIN_FILE_OFF_P = 0,
  DARWIN_FILE_OFF_W = 12,
  DARWIN_FILE_OFF_FLAGS = 16,
  DARWIN_FILE_OFF_BF_BASE = 24,
  DARWIN_FILE_OFF_BF_SIZE = 32,
  DARWIN_FILE_OFF_LBFSIZE = 40,
  DARWIN_FILE_OFF_NBUF = 119
};

/* Flag bits from Darwin/FreeBSD _stdio.h.  */
enum : uint16_t
{
  DARWIN_SLBF = 0x0001,
  DARWIN_SNBF = 0x0002,
  DARWIN_SMBF = 0x0080,
  DARWIN_SOPT = 0x0400,
  DARWIN_SOFF = 0x1000,
  DARWIN_SNPT = 0x0800,
  DARWIN_SEOF = 0x0020
};

/* Probe notes (macOS arm64, CommandLineTools / Xcode SDK, companion GDB):
   - `#define stdout __stdoutp` and `extern FILE *__stdoutp` — symbol holds FILE*.
   - `#define _IONBF 2`.
   - `call_function_by_hand` / inferior `call setvbuf(...)` fails: AArch64 Darwin
     rejects PC stores ("Changing the program counter is not supported").
   Therefore replicate Darwin setvbuf(fp, NULL, _IONBF, 0) FILE state via
   target memory (FreeBSD/Apple setvbuf.c `nbf:` path), without flushing or
   freeing the prior buffer.  */

static bool
write_inferior_u16 (CORE_ADDR addr, uint16_t v)
{
  gdb_byte buf[2];
  store_unsigned_integer (buf, 2, gdbarch_byte_order (current_inferior ()->arch ()),
			  v);
  return target_write_memory (addr, buf, 2) == 0;
}

static bool
write_inferior_u32 (CORE_ADDR addr, uint32_t v)
{
  gdb_byte buf[4];
  store_unsigned_integer (buf, 4, gdbarch_byte_order (current_inferior ()->arch ()),
			  v);
  return target_write_memory (addr, buf, 4) == 0;
}

static bool
write_inferior_addr (CORE_ADDR addr, CORE_ADDR v)
{
  struct gdbarch *arch = current_inferior ()->arch ();
  int len = gdbarch_ptr_bit (arch) / 8;
  gdb_byte buf[8];

  gdb_assert (len <= (int) sizeof (buf));
  store_unsigned_integer (buf, len, gdbarch_byte_order (arch), v);
  return target_write_memory (addr, buf, len) == 0;
}

static bool
read_inferior_addr (CORE_ADDR addr, CORE_ADDR *out)
{
  struct gdbarch *arch = current_inferior ()->arch ();
  int len = gdbarch_ptr_bit (arch) / 8;
  gdb_byte buf[8];

  gdb_assert (len <= (int) sizeof (buf));
  if (target_read_memory (addr, buf, len) != 0)
    return false;
  *out = extract_unsigned_integer (buf, len, gdbarch_byte_order (arch));
  return true;
}

static bool
read_inferior_u16 (CORE_ADDR addr, uint16_t *out)
{
  gdb_byte buf[2];
  if (target_read_memory (addr, buf, 2) != 0)
    return false;
  *out = extract_unsigned_integer (buf, 2,
				   gdbarch_byte_order (current_inferior ()->arch ()));
  return true;
}

/* Apply Darwin setvbuf(_IONBF) effect to the FILE pointed at by __stdoutp.  */

static void
unbuffer_inferior_stdout_command (const char *args, int from_tty)
{
  bound_minimal_symbol msym
    = lookup_minimal_symbol (current_program_space, "__stdoutp");
  if (msym.minsym == nullptr)
    {
      warning (_("inferior-stdio-unbuffered: no __stdoutp in the inferior"));
      return;
    }

  CORE_ADDR stdoutp_sym = msym.value_address ();
  CORE_ADDR file_ptr;
  if (!read_inferior_addr (stdoutp_sym, &file_ptr) || file_ptr == 0)
    {
      warning (_("inferior-stdio-unbuffered: cannot read __stdoutp"));
      return;
    }

  uint16_t flags;
  if (!read_inferior_u16 (file_ptr + DARWIN_FILE_OFF_FLAGS, &flags))
    {
      warning (_("inferior-stdio-unbuffered: cannot read stdout _flags"));
      return;
    }

  /* Match Apple/FreeBSD setvbuf _IONBF path (skip __sflush / free).  */
  flags &= (uint16_t) ~(DARWIN_SLBF | DARWIN_SNBF | DARWIN_SMBF
			| DARWIN_SOPT | DARWIN_SOFF | DARWIN_SNPT | DARWIN_SEOF);
  flags |= DARWIN_SNBF;

  CORE_ADDR nbuf = file_ptr + DARWIN_FILE_OFF_NBUF;

  if (!write_inferior_u16 (file_ptr + DARWIN_FILE_OFF_FLAGS, flags)
      || !write_inferior_u32 (file_ptr + DARWIN_FILE_OFF_W, 0)
      || !write_inferior_u32 (file_ptr + DARWIN_FILE_OFF_LBFSIZE, 0)
      || !write_inferior_addr (file_ptr + DARWIN_FILE_OFF_P, nbuf)
      || !write_inferior_addr (file_ptr + DARWIN_FILE_OFF_BF_BASE, nbuf)
      || !write_inferior_u32 (file_ptr + DARWIN_FILE_OFF_BF_SIZE, 1))
    {
      warning (_("inferior-stdio-unbuffered: failed to update stdout FILE"));
      return;
    }
}

static void
attach_unbuffer_actions (struct breakpoint *b, bool auto_continue)
{
  /* For `run` (auto_continue): silent + unbuffer + continue so the IDE never
     sees this internal main stop.  For `start` / break-on-entry: do NOT mark
     silent — Code::Blocks parses the normal "Breakpoint N, main () at …"
     stop line to fill the cursor; silencing it left the IDE at "In ()".  */
  if (auto_continue)
    breakpoint_set_silent (b, 1);

  command_line *cmd
    = new command_line (simple_control,
			xstrdup ("_inferior_stdio_unbuffer_stdout"));
  /* A continue_control node has no command text.  The breakpoint
     command checker calls strstr on that text and crashes.  "continue"
     is an ordinary breakpoint command.  */
  if (auto_continue)
    cmd->next = new command_line (simple_control, xstrdup ("continue"));

  breakpoint_set_commands (b,
			   counted_command_line (cmd, command_lines_deleter ()));
}

/* STOP_AT_MAIN true: `start` already planted a temporary main breakpoint;
   attach unbuffer and leave stopped (visible stop).  False: plant our own
   temporary main breakpoint, unbuffer, then continue.  */

void
maybe_arm_inferior_stdio_unbuffered (bool stop_at_main)
{
  if (!inferior_stdio_unbuffered)
    return;

  try
    {
      if (!stop_at_main)
	{
	  std::string arg
	    = string_printf ("-qualified %s inferior %d",
			     main_name (),
			     current_inferior ()->num);
	  tbreak_command (arg.c_str (), 0);
	}

      LONGEST num = 0;
      if (!get_internalvar_integer (lookup_internalvar ("bpnum"), &num)
	  || num <= 0)
	{
	  warning (_("inferior-stdio-unbuffered: no main breakpoint to arm"));
	  return;
	}

      breakpoint *b = get_breakpoint ((int) num);
      if (b == nullptr)
	{
	  warning (_("inferior-stdio-unbuffered: breakpoint %s missing"),
		   plongest (num));
	  return;
	}

      attach_unbuffer_actions (b, !stop_at_main);
    }
  catch (const gdb_exception_error &ex)
    {
      warning (_("inferior-stdio-unbuffered: %s"), ex.what ());
    }
}

INIT_GDB_FILE (inferior_stdio_unbuffered)
{
  add_setshow_boolean_cmd ("inferior-stdio-unbuffered", class_run,
			   &inferior_stdio_unbuffered, _("\
Set whether GDB unbuffers the inferior's stdout at main."), _("\
Show whether GDB unbuffers the inferior's stdout at main."), _("\
When on, run and start install a one-shot action at main that makes the\n\
inferior's C stdout unbuffered (Darwin: replicate setvbuf(_IONBF) on the\n\
FILE behind __stdoutp).  Intended for IDE Debug sessions so cout/printf\n\
without a newline appear while stepping.  Default is off."),
			   nullptr,
			   show_inferior_stdio_unbuffered,
			   &setlist, &showlist);

  /* Internal helper used only from the one-shot breakpoint commands.  */
  add_cmd ("_inferior_stdio_unbuffer_stdout", class_obscure,
	   unbuffer_inferior_stdout_command, _("\
Internal: unbuffer inferior stdout (Darwin FILE _IONBF state)."),
	   &cmdlist);
}
