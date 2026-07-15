#include "http.h"

int
main (int argc, char *argv[])
{
  if (argc < 3)
    return 1;
  return http_listen (argv[1], atol (argv[2]));
}
