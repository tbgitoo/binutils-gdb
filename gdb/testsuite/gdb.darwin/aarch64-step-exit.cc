/* Step off the last line of main.  The return lands in dyld.  */

int
main ()
{
  int a = 5;
  int b = 7;

  if (a < b)
    b = a;

  return 0;
}
