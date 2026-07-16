#include "http.h"
static llhttp_settings_t llhttp_settings;
static char *H1_400 = "HTTP/1.1 " H1_CODE_400 "\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 11\r\n"
                      "\r\n"
                      "Bad Request";

static void
free_request (struct http_client *client)
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
  free_request (client);
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
    return HPE_USER;
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
  for (char *buffer = buf->base;;)
    {
      switch (llhttp_execute (&client->parser, buffer, nread))
        {
        case HPE_OK:;
          break;
        case HPE_PAUSED:;
          close_client (client);
          break;
        case HPE_PAUSED_UPGRADE:;
          llhttp_resume_after_upgrade (&client->parser);
          auto off = llhttp_get_error_pos (&client->parser) - buffer;
          if (off < nread)
            {
              buffer += off;
              nread -= off;
              continue;
            }
          break;
        default:;
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
          break;
        }
      break;
    }
  free (buf->base);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = null;
  while (siz && !(buf->base = malloc (siz)))
    siz >>= 1;
  buf->len = siz;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status)
    return;
  struct http_client *client = calloc (1, sizeof (*client));
  if (!client)
    {
      uv_tcp_t *closer = null;
      while (!closer)
        {
          sched_yield ();
          closer = malloc (sizeof (*closer));
        }
      uv_tcp_init (srv->loop, closer);
      if (uv_accept (srv, (uv_stream_t *)closer))
        return uv_close ((uv_handle_t *)closer, (uv_close_cb)free);
      uv_tcp_close_reset (closer, (uv_close_cb)free);
      return;
    }
  uv_tcp_init (srv->loop, &client->tcp_handle);
  if (uv_accept (srv, (uv_stream_t *)client))
    return uv_close ((uv_handle_t *)client, (uv_close_cb)free);
  llhttp_init (&client->parser, HTTP_REQUEST, &llhttp_settings);
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

static int
serve (uv_loop_t *loop, struct sockaddr const *addr, unsigned flags)
{
  uv_tcp_t server;
  uv_tcp_init_ex (loop, &server, addr->sa_family);
  if (addr->sa_family == AF_INET6)
    {
      uv_os_fd_t fd;
      if (!uv_fileno ((uv_handle_t *)&server, &fd))
        setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){ 0 }, sizeof (int));
    }
  if (uv_tcp_bind (&server, addr, flags))
    return 1;
  if (uv_listen ((uv_stream_t *)&server, 16384, on_connection))
    return 1;
  return uv_run (loop, UV_RUN_DEFAULT);
}

struct worker
{
  pthread_t thread;
  uv_loop_t loop;
  struct sockaddr const *addr;
  int result;
};

static void *
worker_main (void *arg)
{
  struct worker *w = arg;
  w->result = serve (&w->loop, w->addr, UV_TCP_REUSEPORT);
  pthread_exit (null);
}

int
http_listen (struct sockaddr const *addr, long threads)
{
  signal (SIGPIPE, SIG_IGN);
  init_static ();
  if (threads <= 0)
    threads = uv_available_parallelism () + threads;
  if (threads <= 1)
    return serve (uv_default_loop (), addr, 0);
  struct worker *w = calloc (threads, sizeof (*w));
  if (!w)
    return 1;
  unsigned started;
  for (started = 0; started < threads; ++started)
    {
      if (uv_loop_init (&w[started].loop))
        break;
      w[started].addr = addr;
      if (pthread_create (&w[started].thread, null, worker_main, &w[started]))
        {
          uv_loop_close (&w[started].loop);
          break;
        }
    }
  if (started == 0)
    {
      free (w);
      return 1;
    }
  int result = 0;
  for (unsigned i = 0; i < started; ++i)
    {
      pthread_join (w[i].thread, null);
      uv_loop_close (&w[i].loop);
      if (w[i].result)
        result = w[i].result;
    }
  free (w);
  return result;
}
