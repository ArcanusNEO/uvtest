#include <uv.h>
#include <llhttp.h>
#include <grimoire.h>

#define H1 "HTTP/1.1"
#define H1_EOL "\r\n"

uv_tcp_t server;

struct h1_client
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
on_read (uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf)
{
  if (nread <= 0)
    {
      free (buf->base);
      if (nread < 0)
        uv_close ((uv_handle_t *)stream, (uv_close_cb)free);
      return;
    }
  struct h1_client *client = stream->data;
  if (llhttp_execute (&client->parser, buf->base, nread) != HPE_OK)
    {
      /* todo$ (); */
    }
  free (buf->base);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = malloc$ (siz);
  buf->len = siz;
}

static void
handle_http_request (struct h1_client *client)
{
  /* todo$ (); */
}

static int
on_headers_complete (llhttp_t *parser)
{
  return HPE_OK;
}

static int
on_body (llhttp_t *parser, char const *at, usz len)
{
  struct h1_client *client = parser->data;
  usz siz = client->body->size;
  client->body = rebin$ (client->body, siz + len);
  clogger (ASSERT, client->body->size == siz + len);
  memcpy (client->body->store + siz, at, len);
  return HPE_OK;
}

static int
on_message_complete (llhttp_t *parser)
{
  struct h1_client *client = parser->data;
  handle_http_request (client);
  return HPE_OK;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status < 0)
    return;
  struct h1_client *client = calloc$ (sizeof (client));
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
