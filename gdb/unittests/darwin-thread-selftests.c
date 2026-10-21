/* Delayed Darwin Mach-thread reconcile, for GDB.

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

#include "gdbsupport/selftest.h"

#if defined (__APPLE__) && GDB_SELF_TEST

#include "gdbsupport/scope-exit.h"
#include "gdbthread.h"
#include "inferior.h"
#include "progspace.h"
#include "gdbarch.h"
#include "process-stratum-target.h"
/* After the GDB thread type.  mach/thread_act.h declares a function
   named thread_info, which hides the class if this comes first.  */
#include "darwin-nat.h"

namespace selftests {
namespace darwin_threads {

/* Ports are not real Mach rights.  check_new_threads must be using the
   selftest hook, which does not deallocate them.  */
constexpr thread_t main_port = 0x1111;
constexpr thread_t helper_port = 0x2222;
constexpr int fake_pid = 424242;

/* A Mach port can sit in darwin_inferior::threads and still be absent
   from the live ptid map.  The fast path compares port names only, so
   that disagreement survives a reconcile that still returns the port.
   The remove path is the first one that looks the port up.  Before
   a8a35dc1a76 that lookup used find_thread and then delete_thread on a
   null pointer.  */

static void
test_delayed_reconcile ()
{
  inferior *inf = nullptr;
  program_space *pspace = nullptr;
  darwin_thread_info *main_pti = nullptr;
  darwin_thread_info *helper_pti = nullptr;
  scoped_restore_current_thread restore_thread;

  SCOPE_EXIT
    {
      darwin_selftest_clear_kernel_threads ();
      switch_to_no_thread ();

      auto release_pti = [&] (darwin_thread_info *pti)
	{
	  if (pti == nullptr)
	    return;
	  if (inf != nullptr && inf->priv != nullptr)
	    {
	      darwin_inferior *dinf = get_darwin_inferior (inf);
	      for (darwin_thread_t *&slot : dinf->threads)
		if (slot == pti)
		  {
		    delete slot;
		    slot = nullptr;
		    return;
		  }
	    }
	  delete pti;
	};

      release_pti (main_pti);
      release_pti (helper_pti);
      main_pti = nullptr;
      helper_pti = nullptr;

      if (inf != nullptr)
	{
	  if (inf->priv != nullptr)
	    get_darwin_inferior (inf)->threads.clear ();
	  delete_inferior (inf);
	  inf = nullptr;
	  pspace = nullptr;
	}
      else
	delete pspace;
    };

  pspace = new program_space (maybe_new_address_space ());
  inf = add_inferior_silent (fake_pid);
  inf->pspace = pspace;
  inf->aspace = pspace->aspace;
  gdbarch_info info;
  inf->set_arch (gdbarch_find_by_info (info));
  SELF_CHECK (inf->arch () != nullptr);

  target_ops *native = get_native_target ();
  SELF_CHECK (native != nullptr);
  inf->push_target (native);
  inf->priv.reset (new darwin_inferior);

  process_stratum_target *proc = as_process_stratum_target (native);
  class thread_info *main_thr
    = add_thread_silent (proc, ptid_t (fake_pid, 0, main_port));
  class thread_info *helper_thr
    = add_thread_silent (proc, ptid_t (fake_pid, 0, helper_port));
  SELF_CHECK (main_thr != nullptr);
  SELF_CHECK (helper_thr != nullptr);

  main_pti = new darwin_thread_info;
  main_pti->gdb_port = main_port;
  main_pti->msg_state = DARWIN_RUNNING;
  helper_pti = new darwin_thread_info;
  helper_pti->gdb_port = helper_port;
  helper_pti->msg_state = DARWIN_RUNNING;

  darwin_inferior *darwin_inf = get_darwin_inferior (inf);
  darwin_inf->threads.push_back (main_pti);
  darwin_inf->threads.push_back (helper_pti);

  /* Selected, so set_thread_exited drops the live-map entry but leaves
     the thread_info object in the inferior (it is not deletable).  */
  switch_to_thread_no_regs (helper_thr);
  set_thread_exited (helper_thr, {}, true);

  SELF_CHECK (helper_thr->state () == THREAD_EXITED);
  SELF_CHECK (inf->find_thread (ptid_t (fake_pid, 0, helper_port)) == nullptr);
  SELF_CHECK (inf->find_thread (ptid_t (fake_pid, 0, main_port)) == main_thr);

  bool helper_still_listed = false;
  for (class thread_info &tp : inf->threads ())
    if (tp.ptid.pid () == fake_pid && tp.ptid.tid () == helper_port)
      helper_still_listed = true;
  SELF_CHECK (helper_still_listed);

  auto *darwin
    = gdb::checked_static_cast<darwin_nat_target *> (native);

  /* The kernel still reports the port.  The fast path must not notice
     that the live map has already dropped it.  */
  const thread_t still_there[] = { main_port, helper_port };
  darwin_selftest_set_kernel_threads (still_there, 2);
  darwin->selftest_check_new_threads (inf);

  SELF_CHECK (darwin_inf->threads.size () == 2);
  SELF_CHECK (darwin_inf->threads[0]->gdb_port == main_port);
  SELF_CHECK (darwin_inf->threads[1]->gdb_port == helper_port);
  SELF_CHECK (inf->find_thread (ptid_t (fake_pid, 0, helper_port)) == nullptr);
  SELF_CHECK (inf->find_thread (ptid_t (fake_pid, 0, main_port)) == main_thr);
  SELF_CHECK (helper_thr->state () == THREAD_EXITED);

  /* A later stop no longer returns the port.  This is the remove path.  */
  const thread_t main_only[] = { main_port };
  darwin_selftest_set_kernel_threads (main_only, 1);
  darwin->selftest_check_new_threads (inf);

  SELF_CHECK (darwin_inf->threads.size () == 1);
  SELF_CHECK (darwin_inf->threads[0]->gdb_port == main_port);
  SELF_CHECK (darwin_inf->threads[0] == main_pti);
  SELF_CHECK (inf->find_thread (ptid_t (fake_pid, 0, main_port)) == main_thr);
  SELF_CHECK (inf->find_thread (ptid_t (fake_pid, 0, helper_port)) == nullptr);
}

} /* namespace darwin_threads */
} /* namespace selftests */

#endif /* __APPLE__ && GDB_SELF_TEST */

INIT_GDB_FILE (darwin_thread_selftests)
{
#if defined (__APPLE__) && GDB_SELF_TEST
  selftests::register_test
    ("darwin_delayed_thread",
     selftests::darwin_threads::test_delayed_reconcile);
#endif
}
