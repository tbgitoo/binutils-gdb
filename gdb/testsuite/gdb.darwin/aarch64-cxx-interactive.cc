/* Interactive console inferior: std::cin plus an if/else branch.  */

#include <iostream>

int
main ()
{
  int value = 0;

  if (!(std::cin >> value))
    return 1;

  int branch = 0;

  if (value > 3)
    branch = 1;
  else
    branch = 2;

  return branch;
}
