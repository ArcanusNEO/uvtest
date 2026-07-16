#include "http.h"

int
main (int argc, char *argv[])
{
  if (argc < 3)
    goto usage;
  auto port = atol (argv[2]);
  if (port < 1 || port > 65535)
    goto usage;
  return http_listen (argv[1], port);
usage:
  cerr ("usage:", argv[0], "<host> <port>");
  return 1;
}
