#include <uv.h>
#include <llhttp.h>
#include "yyjson.h"
#include <grimoire.h>

uv_tcp_t server;

struct http_client
{
  uv_tcp_t tcp_handle;
  uv_write_t write_request;
  uv_buf_t write_buffer;
  llhttp_t parser;
  llhttp_settings_t settings;
  bsto *header;
  bsto *body;
  bsto *response;
};

static void
on_write (uv_write_t *req, int status)
{
  uv_buf_t *buf = (void *)req + sizeof (uv_write_t);
  free (buf->base);
  free (req);
}

static void
on_read (uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf)
{
  if (nread <= 0)
    {
      free (buf->base);
      if (nread < 0)
        uv_close ((uv_handle_t *)stream, (uv_close_cb)free);
      return;
    }

  uv_write_t *wreq = malloc$ (sizeof (uv_write_t), sizeof (uv_buf_t));
  uv_buf_t *wbuf = (void *)wreq + sizeof (uv_write_t);
  wbuf->base = buf->base;
  wbuf->len = nread;
  uv_write (wreq, stream, wbuf, 1, on_write);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = malloc$ (siz);
  buf->len = siz;
}

static void
handle_http_request (struct http_client *client)
{
}

static int
on_headers_complete (llhttp_t *parser)
{
  return HPE_OK;
}

static int
on_body (llhttp_t *parser, char const *at, usz len)
{
  struct http_client *client = parser->data;
  usz siz = client->body->size;
  client->body = rebin$ (client->body, siz + len);
  clogger (ASSERT, client->body->size == siz + len);
  memcpy (client->body->store + siz, at, len);
  return HPE_OK;
}

static int
on_message_complete (llhttp_t *parser)
{
  struct http_client *client = parser->data;
  handle_http_request (client);
  return HPE_OK;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status < 0)
    return;
  struct http_client *client = calloc$ (sizeof (client));
  uv_tcp_init (srv->loop, &client->tcp_handle);
  client->tcp_handle.data = client;
  if (uv_accept (srv, (uv_stream_t *)client) < 0)
    return uv_close ((uv_handle_t *)client, (uv_close_cb)free);

  llhttp_settings_init (&client->settings);
  client->settings.on_headers_complete = on_headers_complete;
  client->settings.on_body = on_body;
  client->settings.on_message_complete = on_message_complete;
  llhttp_init (&client->parser, HTTP_BOTH, &client->settings);
  client->parser.data = client;

  uv_read_start ((uv_stream_t *)client, on_read_alloc, on_read);
}

int
main (int argc, char *argv[])
{
  signal (SIGPIPE, SIG_IGN);
  if (argc < 3)
    return 1;
  auto loop = uv_default_loop ();
  uv_tcp_init (loop, &server);
  struct sockaddr_in addr;
  uv_ip4_addr (argv[1], atoi (argv[2]), &addr);
  uv_tcp_bind (&server, (struct sockaddr *)&addr, 0);
  if (uv_listen ((uv_stream_t *)&server, 1024, on_connection) < 0)
    return 1;
  return uv_run (loop, UV_RUN_DEFAULT);
}
