/* Exercise a separately specified inferior tty on Darwin.  */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Claim NAME as this process's controlling terminal and wait.  The
   parent returns immediately so the test harness is not the session
   that owns the device.  */

static void
hold_tty (const char *name)
{
  pid_t child = fork ();

  if (child < 0)
    return;
  if (child > 0)
    _exit (0);

  if (setsid () < 0)
    _exit (2);

  int fd = open (name, O_RDWR);
  if (fd < 0)
    _exit (3);
  if (ioctl (fd, TIOCSCTTY, 0) < 0)
    _exit (4);
  if (tcsetpgrp (fd, getpid ()) < 0)
    _exit (5);

  dprintf (fd, "holding %ld\n", (long) getpid ());
  for (;;)
    pause ();
}

int
main (int argc, char **argv)
{
  int value = -1;
  int foreground;

  if (argc == 3 && strcmp (argv[1], "hold") == 0)
    {
      hold_tty (argv[2]);
      return 1;
    }

  foreground = tcgetpgrp (0);
  if (foreground >= 0 && foreground == (int) getpgrp ())
    puts ("ctty-ok");
  else
    puts ("ctty-none");

  puts ("Input b");
  fflush (stdout);
  if (scanf ("%d", &value) != 1)
    {
      puts ("scanf-failed");
      return 2;
    }
  printf ("got %d\n", value);
  return 0;
}
