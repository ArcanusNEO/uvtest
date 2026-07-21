#pragma once
#ifndef HTTP_H
#define HTTP_H

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
#define H1_CODE_306 "306 Switch Proxy"
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
#define H1_SERVER "Server: %s\r\n"
#define H1_CONNECTION "Connection: %s\r\n"
#define H1_CONTENT_LENGTH "Content-Length: %zu\r\n"
#define H1_CONTENT_TYPE "Content-Type: %s\r\n"

struct http_client
{
  uv_tcp_t tcp_handle;
  llhttp_t parser;
  struct lsnod response_queue;
  bsto *body;
  bool closing : 1;
};

struct http_response
{
  struct http_client *client;
  struct lsnod list_entry;
  uv_write_t write_request;
  uv_buf_t write_buffer;
  bool keep_alive : 1;
  char buffer[0];
};

int http_listen (struct sockaddr const *addr, long threads);

#endif /* HTTP_H */
