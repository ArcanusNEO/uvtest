#include "http.h"

uv_tcp_t server;

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
on_write (uv_write_t *request, int status)
{
  struct h1_client *client
      = container_of (request, struct h1_client, write_request);
  free (client->write_buffer.base);
  if (status || !client->keep_alive)
    uv_close ((uv_handle_t *)client, (uv_close_cb)free_client);
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
      uv_read_stop (stream);
      client->write_buffer.base = strdup (H1_400);
      client->write_buffer.len = sizeof (H1_400) - 1;
      int wr = uv_write (&client->write_request, stream, &client->write_buffer,
                         1, on_write);
      if (wr)
        on_write (&client->write_request, -wr);
    }
  free (buf->base);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = malloc$ (siz);
  buf->len = siz;
}

int
response (struct h1_client *client, char *header, byte *content, usz length)
{
  int keep_alive = client->keep_alive
      = llhttp_should_keep_alive (&client->parser);
  usz hsiz = strlen (header);
  usz bufsiz = sizeof (H1) + hsiz + sizeof (H1_CONNECTION)
               + sizeof ("keep-alive") + sizeof (H1_CONTENT_LENGTH)
               + sizeof (quote$ (SIZE_MAX)) + sizeof (H1_EOL) + length;
  char *cur = client->write_buffer.base = malloc$ (bufsiz);
  memcpy (cur, H1, sizeof (H1) - 1);
  cur += sizeof (H1) - 1;
  *cur++ = ' ';
  memcpy (cur, header, hsiz);
  cur += hsiz;
  cur += sprintf (cur, H1_CONNECTION, keep_alive ? "keep-alive" : "close");
  cur += sprintf (cur, H1_CONTENT_LENGTH, length);
  memcpy (cur, H1_EOL, sizeof (H1_EOL) - 1);
  cur += sizeof (H1_EOL) - 1;
  memcpy (cur, content, length);
  cur += length;
  client->write_buffer.len = cur - client->write_buffer.base;
  int wr = uv_write (&client->write_request, (uv_stream_t *)client,
                     &client->write_buffer, 1, on_write);
  if (wr)
    {
      on_write (&client->write_request, -wr);
      return 0;
    }
  return keep_alive;
}

static void
handle_http_request (struct h1_client *client)
{
  bsto *body = client->body ? client->body : &(bsto){ 0 };
  char header[] = H1_CODE_200 H1_EOL;

  if (response (client, header, body->store, body->size))
    free_http (client);
}

static int
on_body (llhttp_t *parser, char const *at, usz len)
{
  if (len == 0)
    return HPE_OK;
  struct h1_client *client = container_of (parser, struct h1_client, parser);
  usz siz = client->body ? client->body->size : 0;
  client->body = rebin$ (client->body, siz + len);
  if (!client->body || client->body->size != siz + len)
    {
      // TODO
      return HPE_USER;
    }
  memcpy (client->body->store + siz, at, len);
  return HPE_OK;
}

static int
on_message_complete (llhttp_t *parser)
{
  struct h1_client *client = container_of (parser, struct h1_client, parser);
  handle_http_request (client);
  return HPE_OK;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status < 0)
    return;
  struct h1_client *client = calloc$ (sizeof (*client));
  uv_tcp_init (srv->loop, &client->tcp_handle);
  if (uv_accept (srv, (uv_stream_t *)client) < 0)
    return uv_close ((uv_handle_t *)client, (uv_close_cb)free);
  llhttp_settings_init (&client->settings);
  client->settings.on_body = on_body;
  client->settings.on_message_complete = on_message_complete;
  llhttp_init (&client->parser, HTTP_BOTH, &client->settings);
  uv_read_start ((uv_stream_t *)client, on_read_alloc, on_read);
}

int
http_listen (char const *host, unsigned short port)
{
  signal (SIGPIPE, SIG_IGN);
  auto loop = uv_default_loop ();
  uv_tcp_init (loop, &server);
  struct sockaddr_in addr;
  uv_ip4_addr (host, port, &addr);
  uv_tcp_bind (&server, (struct sockaddr *)&addr, 0);
  if (uv_listen ((uv_stream_t *)&server, 16384, on_connection) < 0)
    return 1;
  return uv_run (loop, UV_RUN_DEFAULT);
}
