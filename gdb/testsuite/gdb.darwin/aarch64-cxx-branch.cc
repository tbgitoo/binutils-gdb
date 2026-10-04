/* Conditional-branch inferior for native AArch64 Darwin debugging.  */

int
main ()
{
  int value = 7;
  int taken = 0;

  if (value > 3)
    taken = 1;
  else
    taken = 2;

  return taken;
}
