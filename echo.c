#include <uv.h>
#include "cmacs.h"

uv_tcp_t server;

static void
on_write (uv_write_t *req, int status)
{
  free (req->data - sizeof (uv_buf_t));
  free (req);
}

static void
on_read (uv_stream_t *client, ssize_t nread, uv_buf_t const *buf)
{
  if (nread < 0)
    {
      uv_close ((void *)client, null);
      free (buf->base - sizeof (uv_buf_t));
      free (client);
      return;
    }
  uv_write_t *req = malloc$ (sizeof (uv_write_t));
  req->data = buf->base;
  uv_buf_t *wbuf = req->data - sizeof (uv_buf_t);
  wbuf->len = nread;
  uv_write (req, client, wbuf, 1, on_write);
}

static void
on_ralloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  uv_buf_t *mem = malloc$ (sizeof (uv_buf_t), siz);
  buf->base = (void *)mem + sizeof (uv_buf_t);
  buf->len = siz;
  mem->base = buf->base;
  mem->len = buf->len;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status < 0)
    return;
  uv_tcp_t *client = malloc$ (sizeof (uv_tcp_t));
  uv_tcp_init (srv->loop, client);
  if (uv_accept (srv, (void *)client) == 0)
    uv_read_start ((void *)client, on_ralloc, on_read);
  else
    {
      uv_close ((void *)client, null);
      free (client);
    }
}

int
main (int argc, char *argv[])
{
  if (argc < 3)
    return 1;
  signal (SIGPIPE, SIG_IGN);
  auto loop = uv_default_loop ();
  uv_tcp_init (loop, &server);
  struct sockaddr_in addr;
  uv_ip4_addr (argv[1], atoi (argv[2]), &addr);
  uv_tcp_bind (&server, (void *)&addr, 0);
  if (uv_listen ((void *)&server, 1024, on_connection) < 0)
    return 1;
  return uv_run (loop, UV_RUN_DEFAULT);
}
