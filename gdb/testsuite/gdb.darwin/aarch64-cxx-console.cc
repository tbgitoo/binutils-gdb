/* Console-input inferior for native AArch64 Darwin debugging.  */

#include <iostream>

int
main ()
{
  int typed = 0;

  if (!(std::cin >> typed))
    return 2;

  return typed;
}
