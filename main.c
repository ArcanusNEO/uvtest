#include <grimoire.h>
#include <uv.h>
#include <llhttp.h>

#define H1 "HTTP/1.1"
#define H1_EOL "\r\n"

#define H1_CODE_100 "100 Continue"
#define H1_CODE_101 "101 Switching Protocols"
#define H1_CODE_102 "102 Processing"
#define H1_CODE_103 "103 Early Hints"
#define H1_CODE_200 "200 OK"
#define H1_CODE_201 "201 Created"
#define H1_CODE_202 "202 Accepted"
#define H1_CODE_203 "203 Non-Authoritative Information"
#define H1_CODE_204 "204 No Content"
#define H1_CODE_205 "205 Reset Content"
#define H1_CODE_206 "206 Partial Content"
#define H1_CODE_207 "207 Multi-Status"
#define H1_CODE_208 "208 Already Reported"
#define H1_CODE_226 "226 IM Used"
#define H1_CODE_300 "300 Multiple Choices"
#define H1_CODE_301 "301 Moved Permanently"
#define H1_CODE_302 "302 Found"
#define H1_CODE_303 "303 See Other"
#define H1_CODE_304 "304 Not Modified"
#define H1_CODE_305 "305 Use Proxy"
#define H1_CODE_307 "307 Temporary Redirect"
#define H1_CODE_308 "308 Permanent Redirect"
#define H1_CODE_400 "400 Bad Request"
#define H1_CODE_401 "401 Unauthorized"
#define H1_CODE_402 "402 Payment Required"
#define H1_CODE_403 "403 Forbidden"
#define H1_CODE_404 "404 Not Found"
#define H1_CODE_405 "405 Method Not Allowed"
#define H1_CODE_406 "406 Not Acceptable"
#define H1_CODE_407 "407 Proxy Authentication Required"
#define H1_CODE_408 "408 Request Timeout"
#define H1_CODE_409 "409 Conflict"
#define H1_CODE_410 "410 Gone"
#define H1_CODE_411 "411 Length Required"
#define H1_CODE_412 "412 Precondition Failed"
#define H1_CODE_413 "413 Content Too Large"
#define H1_CODE_414 "414 URI Too Long"
#define H1_CODE_415 "415 Unsupported Media Type"
#define H1_CODE_416 "416 Range Not Satisfiable"
#define H1_CODE_417 "417 Expectation Failed"
#define H1_CODE_418 "418 I'm a teapot"
#define H1_CODE_421 "421 Misdirected Request"
#define H1_CODE_422 "422 Unprocessable Content"
#define H1_CODE_423 "423 Locked"
#define H1_CODE_424 "424 Failed Dependency"
#define H1_CODE_425 "425 Too Early"
#define H1_CODE_426 "426 Upgrade Required"
#define H1_CODE_428 "428 Precondition Required"
#define H1_CODE_429 "429 Too Many Requests"
#define H1_CODE_431 "431 Request Header Fields Too Large"
#define H1_CODE_451 "451 Unavailable For Legal Reasons"
#define H1_CODE_500 "500 Internal Server Error"
#define H1_CODE_501 "501 Not Implemented"
#define H1_CODE_502 "502 Bad Gateway"
#define H1_CODE_503 "503 Service Unavailable"
#define H1_CODE_504 "504 Gateway Timeout"
#define H1_CODE_505 "505 HTTP Version Not Supported"
#define H1_CODE_506 "506 Variant Also Negotiates"
#define H1_CODE_507 "507 Insufficient Storage"
#define H1_CODE_508 "508 Loop Detected"
#define H1_CODE_510 "510 Not Extended"

#define H1_400                                                          \
  "HTTP/1.1 " H1_CODE_400 "\r\n"                                              \
  "Connection: close\r\n"                                                     \
  "Content-Length: 11\r\n"                                                    \
  "\r\n"                                                                      \
  "Bad Request"
#define H1_SERVER "Server: %s\r\n"
#define H1_CONNECTION "Connection: %s\r\n"
#define H1_CONTENT_LENGTH "Content-Length: %zu\r\n"

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

int
response (struct h1_client *client, char *header, byte *content, usz length)
{
  int keep_alive = !!llhttp_should_keep_alive (&client->parser);
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
  uv_write (&client->write_request, (uv_stream_t *)client,
            &client->write_buffer, 1, null);
  return keep_alive;
}

static void
handle_http_request (struct h1_client *client)
{
  bsto *body = client->body ? client->body : &(bsto){ 0 };
  char header[] = H1_CODE_200 H1_EOL;

  if (response (client, header, body->store, body->size))
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
  usz siz = client->body ? client->body->size : 0;
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
  struct h1_client *client = calloc$ (sizeof (*client));
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
