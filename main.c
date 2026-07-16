#include "http.h"

int
main (int argc, char *argv[])
{
  if (argc < 3)
    goto usage;
  auto port = atol (argv[2]);
  if (port < 1 || port > 65535)
    {
      cerr ("invalid port:", argv[2]);
      return 1;
    }
  struct sockaddr_storage addr;
  if (uv_ip4_addr (argv[1], port, (struct sockaddr_in *)&addr)
      && uv_ip6_addr (argv[1], port, (struct sockaddr_in6 *)&addr))
    {
      cerr ("invalid host:", argv[1]);
      return 1;
    }
  unsigned threads = 0;
  if (argc >= 4)
    {
      auto n = atol (argv[3]);
      if (n < 0)
        {
          cerr ("invalid thread count:", argv[3]);
          return 1;
        }
      threads = n;
    }
  return http_listen_mt ((struct sockaddr *)&addr, threads);
usage:
  cerr ("usage:", argv[0], "<host> <port> [threads]");
  return 1;
}
