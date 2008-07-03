/* The Ebb Web Server
 * Copyright (c) 2008 Ry Dahl. This software is released under the MIT 
 * License. See README file for details.
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <netdb.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>

#define EV_STANDALONE 1
#include <ev.c>
#include <glib.h>

#include "parser.h"
#include "ebb.h"

#define min(a,b) (a < b ? a : b)
#define ramp(a) (a > 0 ? a : 0)

static void client_init(ebb_client *client);

static void set_nonblock(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  assert(0 <= fcntl(fd, F_SETFL, flags | O_NONBLOCK) && "Setting socket non-block failed!");
}


void env_add(ebb_client *client, const char *field, int flen, const char *value, int vlen)
{
  if(client->env_size >= EBB_MAX_ENV) {
    client->parser.overflow_error = TRUE;
    return;
  }
  struct ebb_env_item * const item = &client->env[client->env_size++];
  item->type = -1;
  item->field_length = flen;
  item->value_length = vlen;
  item->field = field;
  item->value = value;
}


void env_add_const(ebb_client *client, int type, const char *value, int vlen)
{
  if(client->env_size >= EBB_MAX_ENV) {
    client->parser.overflow_error = TRUE;
    return;
  }
  struct ebb_env_item * const item = &client->env[client->env_size++];
  item->type = type;
  item->field_length = -1;
  item->value_length = vlen;
  item->field = NULL;
  item->value = value;
}


void http_field_cb(void *data, const char *field, size_t flen, const char *value, size_t vlen)
{
  ebb_client *client = (ebb_client*)(data);
  assert(field != NULL);
  assert(value != NULL);
  env_add(client, field, flen, value, vlen);
}


void on_element(void *data, int type, const char *at, size_t length)
{
  ebb_client *client = (ebb_client*)(data);
  env_add_const(client, type, at, length);
}


static void dispatch(ebb_client *client)
{
  ebb_server *server = client->server;
  if(client->open == FALSE)
    return;
  client->in_use = TRUE;
  
  /* XXX decide if to use keep-alive or not? */
  
  server->request_cb(client, server->request_cb_data);
}


static void on_timeout(struct ev_loop *loop, ev_timer *watcher, int revents)
{
  ebb_client *client = (ebb_client*)(watcher->data);
  
  assert(client->server->loop == loop);
  assert(&(client->timeout_watcher) == watcher);
  
  ebb_client_close(client);
#ifdef DEBUG
  g_message("peer timed out");
#endif
}

#define client_finished_parsing http_parser_is_finished(&client->parser)
#define total_request_size (client->parser.content_length + client->parser.nread)

static void on_client_readable(struct ev_loop *loop, ev_io *watcher, int revents)
{
  ebb_client *client = (ebb_client*)(watcher->data);
  
  assert(client->in_use == FALSE);
  assert(client->open);
  assert(client->server->open);
  assert(client->server->loop == loop);
  assert(&client->read_watcher == watcher);
  
  ssize_t read = recv( client->fd
                     , client->request_buffer + client->read
                     , EBB_BUFFERSIZE - client->read
                     , 0
                     );
  if(read < 0) goto error;
  if(read == 0) goto error; /* XXX is this the right action to take for read==0 ? */
  client->read += read;
  ev_timer_again(loop, &client->timeout_watcher);
  
  // if(client->read == EBB_BUFFERSIZE) goto error;
  
  if(FALSE == client_finished_parsing) {
    http_parser_execute( &client->parser
                       , client->request_buffer
                       , client->read
                       , client->parser.nread
                       );
    if(http_parser_has_error(&client->parser)) goto error;
  }
  
  if(client_finished_parsing) {
    assert(client->read <= total_request_size);
    if(total_request_size == client->read || total_request_size > EBB_BUFFERSIZE) {
      client->body_head = client->request_buffer + client->parser.nread;
      client->body_head_len = client->read - client->parser.nread;
      ev_io_stop(loop, watcher);
      dispatch(client);
      return;
    }
  }
  return;
error:
#ifdef DEBUG
  if(read < 0) g_message("Error recving data: %s", strerror(errno));
#endif
  ebb_client_close(client);
}


static void on_client_writable(struct ev_loop *loop, ev_io *watcher, int revents)
{
  ebb_client *client = (ebb_client*)(watcher->data);
  ssize_t sent;
  
  if(client->status_written == FALSE || client->headers_written == FALSE) {
    g_message("no status or headers - closing connection.");
    goto error;
  }
  
  if(EV_ERROR & revents) {
    g_message("on_client_writable() got error event, closing peer");
    goto error;
  }
  
  //if(client->written != 0)
  //  g_debug("total written: %d", (int)(client->written));
  
  sent = send( client->fd
             , client->response_buffer->str + sizeof(gchar)*(client->written)
             , client->response_buffer->len - client->written
             , 0
             );
  if(sent < 0) {
#ifdef DEBUG
    g_message("Error writing: %s", strerror(errno));
#endif
    goto error;
  } else if(sent == 0) {
    /* is this the wrong thing to do? */
    g_message("Sent zero bytes? Closing connection");
    goto error;
  }
  client->written += sent;
  
  assert(client->written <= client->response_buffer->len);
  //g_message("wrote %d bytes. total: %d", (int)sent, (int)(client->written));
  
  ev_timer_again(loop, &client->timeout_watcher);
  
  if(client->written == client->response_buffer->len) {
    /* stop the write watcher. to be restarted by the next call to ebb_client_write_body
     * or if client->body_written is set (by using ebb_client_release) then
     * we close the connection
     */
    ev_io_stop(loop, watcher);
    if(client->body_written) {
      client->keep_alive ? client_init(client) : ebb_client_close(client);
    }
  }
  return;
error:
  ebb_client_close(client);
}


static void client_init(ebb_client *client)
{
  assert(client->in_use == FALSE);
  
  /* If the client is already open, reuse the fd, just reset all the parameters
   * this would happen in the case of a keep_alive request
   */
  if(!client->open) {
    /* DO SOCKET STUFF */
    socklen_t len = sizeof(struct sockaddr); 
    int fd = accept(client->server->fd, (struct sockaddr*)&(client->sockaddr), &len);
    if(fd < 0) {
      perror("accept()");
      return;
    }
    client->open = TRUE;
    client->fd = fd;
  }
  
  set_nonblock(client->fd);
  
  /* IP Address */
  if(client->server->port)
    client->ip = inet_ntoa(client->sockaddr.sin_addr);  
  
  /* INITIALIZE http_parser */
  http_parser_init(&client->parser);
  client->parser.data = client;
  client->parser.http_field = http_field_cb;
  client->parser.on_element = on_element;
  
  /* OTHER */
  client->env_size = 0;
  client->read =  0;
  if(client->request_buffer == NULL) {
    /* Only allocate the request_buffer once */
    client->request_buffer = (char*)malloc(EBB_BUFFERSIZE);
  }
  client->keep_alive = FALSE;
  client->status_written = client->headers_written = client->body_written = FALSE;
  client->written = 0;
  
  if(client->response_buffer != NULL)
    g_string_free(client->response_buffer, TRUE);
  client->response_buffer = g_string_new("");
  
  /* SETUP READ AND TIMEOUT WATCHERS */
  client->write_watcher.data = client;
  ev_init (&client->write_watcher, on_client_writable);
  ev_io_set (&client->write_watcher, client->fd, EV_WRITE | EV_ERROR);
  /* Note, do not start write_watcher until there is something to be written.
   * See ebb_client_write_body() */
  
  client->read_watcher.data = client;
  ev_init(&client->read_watcher, on_client_readable);
  ev_io_set(&client->read_watcher, client->fd, EV_READ | EV_ERROR);
  ev_io_start(client->server->loop, &client->read_watcher);
  
  client->timeout_watcher.data = client;  
  ev_timer_init(&client->timeout_watcher, on_timeout, EBB_TIMEOUT, 0);
  ev_timer_start(client->server->loop, &client->timeout_watcher);
}


static void on_request(struct ev_loop *loop, ev_io *watcher, int revents)
{
  ebb_server *server = (ebb_server*)(watcher->data);
  assert(server->open);
  assert(server->loop == loop);
  assert(&server->request_watcher == watcher);
  
  if(EV_ERROR & revents) {
    g_message("on_request() got error event, closing server.");
    ebb_server_unlisten(server);
    return;
  }
  /* Now we're going to initialize the client 
   * and set up her callbacks for read and write
   * the client won't get passed back to the user, however,
   * until the request is complete and parsed.
   */
  int i;
  ebb_client *client;
  /* Get next availible peer */
  for(i=0; i < EBB_MAX_CLIENTS; i++)
    if(!server->clients[i].in_use && !server->clients[i].open) {
      client = &(server->clients[i]);
      break;
    }
  if(client == NULL) {
    g_message("Too many peers. Refusing connections.");
    return;
  }
  
#ifdef DEBUG
  int count = 0;
  for(i = 0; i < EBB_MAX_CLIENTS; i++)
    if(server->clients[i].open) count += 1;
  g_debug("%d open connections", count);
#endif
  
  client_init(client);
}


ebb_server* ebb_server_alloc()
{
  ebb_server *server = g_new0(ebb_server, 1);
  return server;
}


void ebb_server_init( ebb_server *server
                    , struct ev_loop *loop
                    , ebb_request_cb request_cb
                    , void *request_cb_data
                    )
{
  int i;
  for(i=0; i < EBB_MAX_CLIENTS; i++) {
    server->clients[i].request_buffer = NULL;
    server->clients[i].response_buffer = NULL;
    server->clients[i].open = FALSE;
    server->clients[i].in_use = FALSE;
    server->clients[i].server = server;
  }
  
  server->request_cb = request_cb;
  server->request_cb_data = request_cb_data;
  server->loop = loop;
  server->open = FALSE;
  server->fd = -1;
  return;
error:
  ebb_server_free(server);
  return;
}


void ebb_server_free(ebb_server *server)
{
  ebb_server_unlisten(server);
  
  if(server->port)
    free(server->port);
  if(server->socketpath)
    free(server->socketpath);
  free(server);
}


void ebb_server_unlisten(ebb_server *server)
{
  if(server->open) {
    int i;
    ebb_client *client;
    ev_io_stop(server->loop, &server->request_watcher);
    close(server->fd);
    if(server->socketpath) {
      unlink(server->socketpath);
      server->socketpath = NULL;
    }
    if(server->port) {
      free(server->port);
      server->port = NULL;
    }
    server->open = FALSE;
  }
}


int ebb_server_listen_on_fd(ebb_server *server, const int sfd)
{
  if (listen(sfd, EBB_MAX_CLIENTS) < 0) {
    perror("listen()");
    return -1;
  }
  
  set_nonblock(sfd); /* XXX: superfluous? */
  
  server->fd = sfd;
  assert(server->port == NULL);
  assert(server->socketpath == NULL);
  assert(server->open == FALSE);
  server->open = TRUE;
  
  server->request_watcher.data = server;
  ev_init (&server->request_watcher, on_request);
  ev_io_set (&server->request_watcher, server->fd, EV_READ | EV_ERROR);
  ev_io_start (server->loop, &server->request_watcher);
  
  return server->fd;
}


int ebb_server_listen_on_port(ebb_server *server, const int port)
{
  int sfd = -1;
  struct linger ling = {0, 0};
  struct sockaddr_in addr;
  int flags = 1;
  
  if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket()");
    goto error;
  }
  
  flags = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
  setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
  setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
  setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
  
  /*
   * the memset call clears nonstandard fields in some impementations
   * that otherwise mess things up.
   */
  memset(&addr, 0, sizeof(addr));
  
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  
  if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind()");
    goto error;
  }
  
  int ret = ebb_server_listen_on_fd(server, sfd);
  if (ret >= 0) {
    assert(server->port == NULL);
    server->port = malloc(sizeof(char)*8); /* for easy access to the port */
    sprintf(server->port, "%d", port);
  }
  return ret;
error:
  if(sfd > 0) close(sfd);
  return -1;
}


int ebb_server_listen_on_unix_socket(ebb_server *server, const char *socketpath)
{
  int sfd = -1;
  struct linger ling = {0, 0};
  struct sockaddr_un addr;
  struct stat tstat;
  int flags =1;
  int old_umask = -1;
  int access_mask = 0777;
  
  if(( sfd = socket(AF_UNIX, SOCK_STREAM, 0) ) == -1) {
    perror("socket()");
    goto error;
  }
  
  /* Clean up a previous socket file if we left it around */
  if(lstat(socketpath, &tstat) == 0 && S_ISSOCK(tstat.st_mode)) {
    unlink(socketpath);
  }
  
  flags = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
  setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
  setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

  /*
   * the memset call clears nonstandard fields in some impementations
   * that otherwise mess things up.
   */
  memset(&addr, 0, sizeof(addr));
  
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, socketpath);
  old_umask = umask( ~(access_mask & 0777) );
  
  if(bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind()");
    goto error;
  }
  umask(old_umask);
  
  int ret = ebb_server_listen_on_fd(server, sfd);
  if (ret >= 0) {
    assert(server->socketpath == NULL);
    server->socketpath = strdup(socketpath);
  }
  return ret;
error:
  if(sfd > 0) close(sfd);
  return -1;
}


int ebb_server_clients_in_use_p(ebb_server *server)
{
  int i;
  for(i = 0; i < EBB_MAX_CLIENTS; i++)
    if(server->clients[i].in_use) return TRUE;
  return FALSE;
}


void ebb_client_release(ebb_client *client)
{
  assert(client->in_use);
  client->in_use = FALSE;
  
  if(client->headers_written == FALSE) {
    g_string_append(client->response_buffer, "\r\n");
    client->headers_written = TRUE;
  }
  client->body_written = TRUE;
  
  /* If the write_watcher isn't yet active, then start it. It could be that
   * we're streaming and the watcher has been stopped. In that case we 
   * start it again since we have more to write. */
  if(ev_is_active(&client->write_watcher) == FALSE) {
    set_nonblock(client->fd);
    ev_io_start(client->server->loop, &client->write_watcher);
  }
  
  if(client->written == client->response_buffer->len)
    ebb_client_close(client);
}


void ebb_client_close(ebb_client *client)
{
  if(client->open) {
    ev_io_stop(client->server->loop, &client->read_watcher);
    ev_io_stop(client->server->loop, &client->write_watcher);
    ev_timer_stop(client->server->loop, &client->timeout_watcher);
    
    client->ip = NULL;
    
    g_string_free(client->response_buffer, TRUE);
    client->response_buffer = NULL;
    
    close(client->fd);
    client->open = FALSE;
  }
}


void ebb_client_write_status(ebb_client *client, int status, const char *reason_phrase)
{
  assert(client->in_use);
  if(!client->open) return;
  assert(client->status_written == FALSE);
  g_string_append_printf( client->response_buffer
                        , "HTTP/1.1 %d %s\r\n"
                        , status
                        , reason_phrase
                        );
  client->status_written = TRUE;
}


void ebb_client_write_header(ebb_client *client, const char *field, const char *value)
{
  assert(client->in_use);
  if(!client->open) return;
  assert(client->status_written == TRUE);
  assert(client->headers_written == FALSE);
  
  if(strcmp(field, "Connection") == 0 && strcmp(value, "Keep-Alive") == 0) {
    client->keep_alive = TRUE;
  }
  g_string_append_printf( client->response_buffer
                        , "%s: %s\r\n"
                        , field
                        , value
                        );
}


void ebb_client_write_body(ebb_client *client, const char *data, int length)
{
  assert(client->in_use);
  if(!client->open) return;
  
  if(client->headers_written == FALSE) {
    g_string_append(client->response_buffer, "\r\n");
    client->headers_written = TRUE;
  }
  
  g_string_append_len(client->response_buffer, data, length);
  
  /* If the write_watcher isn't yet active, then start it. It could be that
   * we're streaming and the watcher has been stopped. In that case we 
   * start it again since we have more to write. */
  if(ev_is_active(&client->write_watcher) == FALSE) {
    set_nonblock(client->fd);
    ev_io_start(client->server->loop, &client->write_watcher);
  }
}

// int ebb_client_should_keep_alive(ebb_client*)
// {
//   /* TODO - return boolean  */
//     if env['HTTP_VERSION'] == 'HTTP/1.0' 
//       return true if env['HTTP_CONNECTION'] =~ /Keep-Alive/i
//     else
//       return true unless env['HTTP_CONNECTION'] =~ /close/i
//     end
//     false
// }
