/* Smoke test inferior for native AArch64 Darwin debugging.  */

int
main ()
{
  int answer = 41;
  answer = answer + 1;
  return answer;
}
