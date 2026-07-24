#include "http.h"
static llhttp_settings_t llhttp_settings;
static char *H1_400 = "HTTP/1.1 " HTTP_CODE_400 "\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 11\r\n"
                      "\r\n"
                      "Bad Request";
static char *H1_404 = "HTTP/1.1 " HTTP_CODE_404 "\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 9\r\n"
                      "\r\n"
                      "Not Found";

static void
free_request (struct http_client *client)
{
  if (!client)
    return;
  free (client->body);
  client->body = null;
  /* keep hdr_buf / hdr_arr allocations for reuse on keep-alive; just reset
     the logical sizes and per-request accumulation state. */
  if (client->hdr_buf)
    client->hdr_buf->size = 0;
  client->hdr_count = 0;
  client->hdr_cur = (struct http_header){ 0 };
  client->hdr_state = HDR_NONE;
}

/* append len bytes to the client's header byte buffer, returning the offset
   at which they were written, or SIZE_MAX on allocation failure. */
static usz
hdr_buf_append (struct http_client *client, char const *at, usz len)
{
  usz off = client->hdr_buf ? client->hdr_buf->size : 0;
  bsto *nb = rebin$ (client->hdr_buf, off + len);
  if (!nb || nb->size != off + len)
    {
      client->hdr_buf = nb ? nb : client->hdr_buf;
      return SIZE_MAX;
    }
  client->hdr_buf = nb;
  memcpy (nb->store + off, at, len);
  return off;
}

/* push the fully-accumulated current header onto the ordered array. */
static int
hdr_record (struct http_client *client)
{
  usz n = client->hdr_count;
  bsto *na = rebin$ (client->hdr_arr, (n + 1) * sizeof (struct http_header));
  if (!na || na->size != (n + 1) * sizeof (struct http_header))
    {
      client->hdr_arr = na ? na : client->hdr_arr;
      return HPE_USER;
    }
  client->hdr_arr = na;
  ((struct http_header *)na->store)[n] = client->hdr_cur;
  client->hdr_count = n + 1;
  client->hdr_cur = (struct http_header){ 0 };
  return HPE_OK;
}

static int
on_header_field (llhttp_t *parser, char const *at, usz len)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  /* a new field after a value means the previous pair is complete. */
  if (client->hdr_state == HDR_VALUE)
    {
      if (hdr_record (client) != HPE_OK)
        return HPE_USER;
    }
  usz off = hdr_buf_append (client, at, len);
  if (off == SIZE_MAX)
    return HPE_USER;
  if (client->hdr_state != HDR_FIELD)
    {
      client->hdr_cur.name_off = off;
      client->hdr_cur.name_len = 0;
      client->hdr_state = HDR_FIELD;
    }
  client->hdr_cur.name_len += len;
  return HPE_OK;
}

static int
on_header_value (llhttp_t *parser, char const *at, usz len)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  usz off = hdr_buf_append (client, at, len);
  if (off == SIZE_MAX)
    return HPE_USER;
  if (client->hdr_state != HDR_VALUE)
    {
      client->hdr_cur.value_off = off;
      client->hdr_cur.value_len = 0;
      client->hdr_state = HDR_VALUE;
    }
  client->hdr_cur.value_len += len;
  return HPE_OK;
}

/* case-insensitive field-name comparison; ties broken by length then by the
   raw value order so the sort is a total, stable ordering. */
static int
hdr_cmp (void const *a, void const *b, void *arg)
{
  struct http_client *client = arg;
  struct http_header const *x = a, *y = b;
  byte const *base = client->hdr_buf->store;
  byte const *xn = base + x->name_off, *yn = base + y->name_off;
  usz n = umin$ (x->name_len, y->name_len);
  for (usz i = 0; i < n; ++i)
    {
      int cx = tolower (xn[i]), cy = tolower (yn[i]);
      if (cx != cy)
        return cx - cy;
    }
  if (x->name_len != y->name_len)
    return x->name_len < y->name_len ? -1 : 1;
  return (x->name_off > y->name_off) - (x->name_off < y->name_off);
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
  free (client->hdr_buf);
  free (client->hdr_arr);
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
  usz bufsiz = sizeof (H1) + 1 + hsiz + sizeof (H1_CONNECTION)
               + umax$ (sizeof ("keep-alive"), sizeof ("close"))
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
on_headers_complete (llhttp_t *parser)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  /* flush the last accumulated header (it has no trailing field to trigger
     the record in on_header_field). */
  if (client->hdr_state == HDR_VALUE)
    {
      if (hdr_record (client) != HPE_OK)
        return HPE_USER;
    }
  client->hdr_state = HDR_NONE;
  if (client->hdr_count > 1)
    qsort_r (client->hdr_arr->store, client->hdr_count,
             sizeof (struct http_header), hdr_cmp, client);
  return HPE_OK;
}

static int
on_message_complete (llhttp_t *parser)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  bsto *body = client->body ? client->body : &(bsto){ 0 };
  char header[] = HTTP_CODE_200 H1_EOL;

  /* echo the sorted headers followed by the request body, so the parse
     result is observable.  build into a scratch binstore. */
  struct http_header *hdrs = http_headers (client);
  bsto *out = null;
  for (usz i = 0; i < client->hdr_count; ++i)
    {
      bslc name = http_header_name (client, &hdrs[i]);
      bslc value = http_header_value (client, &hdrs[i]);
      usz off = out ? out->size : 0;
      usz add = name.size + 2 + value.size + 1; /* "name: value\n" */
      bsto *no = rebin$ (out, off + add);
      if (!no || no->size != off + add)
        {
          free (no ? no : out);
          return HPE_USER;
        }
      out = no;
      byte *cur = out->store + off;
      memcpy (cur, name.slice, name.size);
      cur += name.size;
      *cur++ = ':';
      *cur++ = ' ';
      memcpy (cur, value.slice, value.size);
      cur += value.size;
      *cur++ = '\n';
    }
  /* append the body after the headers. */
  if (body->size)
    {
      usz off = out ? out->size : 0;
      bsto *no = rebin$ (out, off + body->size);
      if (!no || no->size != off + body->size)
        {
          free (no ? no : out);
          return HPE_USER;
        }
      out = no;
      memcpy (out->store + off, body->store, body->size);
    }

  bsto *payload = out ? out : &(bsto){ 0 };
  int rc = http_response (client, header, payload->store, payload->size);
  free (out);
  return rc;
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
  llhttp_settings.on_header_field = on_header_field;
  llhttp_settings.on_header_value = on_header_value;
  llhttp_settings.on_headers_complete = on_headers_complete;
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
  if (uv_tcp_bind (&server, addr, flags)
      || uv_listen ((uv_stream_t *)&server, 16384, on_connection))
    {
      uv_close ((uv_handle_t *)&server, null);
      uv_run (loop, UV_RUN_DEFAULT);
      return 1;
    }
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
