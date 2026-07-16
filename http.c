#include "http.h"
#if _P_PLATFORM_ == (_P_UNIX_ + 0)
#include <sched.h>
#endif
static llhttp_settings_t llhttp_settings;
static char *H1_400 = "HTTP/1.1 " H1_CODE_400 "\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 11\r\n"
                      "\r\n"
                      "Bad Request";

static void
free_http (struct http_client *client)
{
  if (!client)
    return;
  free (client->body);
  client->body = null;
}

static void
free_client (struct http_client *client)
{
  if (!client)
    return;
  while (client->response_queue.next != &client->response_queue)
    {
      struct http_response *response = container_of (
          client->response_queue.next, struct http_response, list_entry);
      list$ (rem) (&response->list_entry);
      free (response);
    }
  free (client->body);
  free (client);
}

static void
close_client (struct http_client *client)
{
  if (client->closing)
    return;
  client->closing = true;
  uv_close ((uv_handle_t *)client, (uv_close_cb)free_client);
}

static void write_next (struct http_client *client);

static void
on_write (uv_write_t *request, int status)
{
  struct http_response *response
      = container_of (request, struct http_response, write_request);
  struct http_client *client = response->client;
  bool keep_alive = response->keep_alive;
  list$ (rem) (&response->list_entry);
  free (response);
  if (status || !keep_alive)
    close_client (client);
  else
    write_next (client);
}

static void
write_next (struct http_client *client)
{
  if (client->response_queue.next == &client->response_queue)
    return;
  struct http_response *response = container_of (
      client->response_queue.next, struct http_response, list_entry);
  int wr = uv_write (&response->write_request, (uv_stream_t *)client,
                     &response->write_buffer, 1, on_write);
  if (wr)
    on_write (&response->write_request, -wr);
}

static void
enqueue_response (struct http_client *client, struct http_response *response)
{
  response->client = client;
  bool idle = client->response_queue.next == &client->response_queue;
  list$ (ins) (&response->list_entry, client->response_queue.prev,
               &client->response_queue);
  if (idle)
    write_next (client);
}

static int
http_response (struct http_client *client, char *header, byte *content,
               usz length)
{
  usz hsiz = strlen (header);
  usz bufsiz = sizeof (H1) + hsiz + sizeof (H1_CONNECTION)
               + umax$ (sizeof ("close"), sizeof ("keep-alive"))
               + sizeof (H1_CONTENT_LENGTH) + sizeof (quote$ (SIZE_MAX))
               + sizeof (H1_EOL) + length;
  struct http_response *r = malloc (sizeof (*r) + bufsiz);
  if (!r)
    {
      free_http (client);
      return HPE_USER;
    }
  char *cur = r->write_buffer.base = r->buffer;
  r->keep_alive = llhttp_should_keep_alive (&client->parser);
  memcpy (cur, H1, sizeof (H1) - 1);
  cur += sizeof (H1) - 1;
  *cur++ = ' ';
  memcpy (cur, header, hsiz);
  cur += hsiz;
  cur += sprintf (cur, H1_CONNECTION, r->keep_alive ? "keep-alive" : "close");
  cur += sprintf (cur, H1_CONTENT_LENGTH, length);
  memcpy (cur, H1_EOL, sizeof (H1_EOL) - 1);
  cur += sizeof (H1_EOL) - 1;
  memcpy (cur, content, length);
  cur += length;
  r->write_buffer.len = cur - r->write_buffer.base;
  free_http (client);
  enqueue_response (client, r);
  return HPE_OK;
}

static int
on_message_complete (llhttp_t *parser)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  bsto *body = client->body ? client->body : &(bsto){ 0 };
  char header[] = H1_CODE_200 H1_EOL;
  return http_response (client, header, body->store, body->size);
}

static int
on_body (llhttp_t *parser, char const *at, usz len)
{
  if (len == 0)
    return HPE_OK;
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  usz siz = client->body ? client->body->size : 0;
  client->body = rebin$ (client->body, siz + len);
  if (!client->body || client->body->size != siz + len)
    return HPE_USER;
  memcpy (client->body->store + siz, at, len);
  return HPE_OK;
}

static void
on_read (uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf)
{
  auto client = (struct http_client *)stream;
  if (nread <= 0)
    {
      free (buf->base);
      if (nread < 0)
        close_client (client);
      return;
    }
  if (llhttp_execute (&client->parser, buf->base, nread) != HPE_OK)
    {
      uv_read_stop (stream);
      struct http_response *response = malloc (sizeof (*response));
      if (response)
        {
          response->keep_alive = false;
          response->write_buffer.base = H1_400;
          response->write_buffer.len = strlen (H1_400);
          enqueue_response (client, response);
        }
      else
        close_client (client);
    }
  free (buf->base);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = null;
  while (siz && !(buf->base = malloc (siz)))
    siz /= 2;
  buf->len = siz;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status < 0)
    return;
  struct http_client *client = calloc (1, sizeof (*client));
  if (!client)
    {
      uv_tcp_t *closer = null;
      while (!closer)
        {
          // FIXME
          sched_yield ();
          closer = malloc (sizeof (*closer));
        }
      uv_tcp_init (srv->loop, closer);
      uv_accept (srv, (uv_stream_t *)closer);
      uv_tcp_close_reset (closer, (uv_close_cb)free);
      return;
    }
  uv_tcp_init (srv->loop, &client->tcp_handle);
  if (uv_accept (srv, (uv_stream_t *)client) < 0)
    return uv_close ((uv_handle_t *)client, (uv_close_cb)free);
  llhttp_init (&client->parser, HTTP_BOTH, &llhttp_settings);
  client->response_queue.next = client->response_queue.prev
      = &client->response_queue;
  uv_read_start ((uv_stream_t *)client, on_read_alloc, on_read);
}

static void
init_static ()
{
  static bool inited;
  if (inited)
    return;
  llhttp_settings_init (&llhttp_settings);
  llhttp_settings.on_body = on_body;
  llhttp_settings.on_message_complete = on_message_complete;
  inited = true;
}

int
http_listen (char const *host, unsigned short port)
{
  signal (SIGPIPE, SIG_IGN);
  init_static ();
  auto loop = uv_default_loop ();
  uv_tcp_t server;
  uv_tcp_init (loop, &server);
  struct sockaddr_in addr;
  uv_ip4_addr (host, port, &addr);
  uv_tcp_bind (&server, (struct sockaddr *)&addr, 0);
  if (uv_listen ((uv_stream_t *)&server, 16384, on_connection) < 0)
    return 1;
  return uv_run (loop, UV_RUN_DEFAULT);
}
