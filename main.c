#include <uv.h>
#include <llhttp.h>
#include <grimoire.h>

#define H1 "HTTP/1.1"
#define H1_EOL "\r\n"
#define H1_400                                                                \
  "HTTP/1.1 400 Bad Request\r\n"                                              \
  "Content-Length: 11\r\n"                                                    \
  "Connection: close\r\n"                                                     \
  "\r\n"                                                                      \
  "Bad Request"
#define H1_SRV "Server: Apache\r\n"

uv_tcp_t server;

struct h1_client
{
  uv_tcp_t tcp_handle;
  uv_write_t write_request;
  uv_buf_t write_buffer;
  llhttp_t parser;
  llhttp_settings_t settings;
  bsto *body;
};

static void
free_http (struct h1_client *client)
{
  if (!client)
    return;
  free (client->body);
  client->body = null;
}

static void
free_client (struct h1_client *client)
{
  if (!client)
    return;
  free (client->body);
  free (client);
}

static void
on_read (uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf)
{
  if (nread <= 0)
    {
      free (buf->base);
      if (nread < 0)
        uv_close ((uv_handle_t *)stream, (uv_close_cb)free_client);
      return;
    }
  auto client = (struct h1_client *)stream;
  if (llhttp_execute (&client->parser, buf->base, nread) != HPE_OK)
    {
      client->write_buffer.base = H1_400;
      client->write_buffer.len = sizeof (H1_400) - 1;
      uv_write (&client->write_request, stream, &client->write_buffer, 1,
                null);
      uv_close ((uv_handle_t *)stream, (uv_close_cb)free_client);
    }
  free (buf->base);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = malloc$ (siz);
  buf->len = siz;
}

void
response (struct h1_client *client, char *header, usz length, byte *content)
{
  usz siz = strlen (header);
  usz tot = sizeof (H1) + siz + sizeof (H1_SRV) + length + 128;
  client->write_buffer.len = tot;
  char *res = client->write_buffer.base = malloc$ (tot);
  memcpy (res, H1, sizeof (H1) - 1);
  res += sizeof (H1) - 1;
  *res++ = ' ';
  memcpy (res, header, siz);
  res += siz;
  memcpy (res, H1_SRV, sizeof (H1_SRV) - 1);
  res += sizeof (H1_SRV) - 1;
  char cl[] = "Content-Length: ";
  /* memcpy (res, "Content-Length: "); */
}

static void
handle_http_request (struct h1_client *client)
{
  int keep_alive = llhttp_should_keep_alive (&client->parser);

  if (keep_alive)
    {
      llhttp_init (&client->parser, HTTP_BOTH, &client->settings);
      client->parser.data = client;
      free_http (client);
      return;
    }
  uv_close ((uv_handle_t *)client, (uv_close_cb)free_client);
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
