/* Register-store inferior for native AArch64 Darwin debugging.  */

int
finish_here (void)
{
  return 0;
}

int
main (void)
{
  int answer = 7;

  answer = 9;
  return answer;
}
