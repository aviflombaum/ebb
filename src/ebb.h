/* The Ebb Web Server
 * Copyright (c) 2008 Ry Dahl. This software is released under the MIT 
 * License. See README file for details.
 */
#ifndef ebb_h
#define ebb_h
#define EV_STANDALONE 1
#include <ev.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <glib.h>
#include "parser.h"

typedef struct ebb_server ebb_server;
typedef struct ebb_client ebb_client;
#define EBB_VERSION "0.2.0"
#define EBB_BUFFERSIZE (1024 * (80 + 33))
#define EBB_MAX_CLIENTS 1024
#define EBB_TIMEOUT 30.0
#define EBB_MAX_ENV 500
#define EBB_TCP_COMMON          \
  unsigned open : 1;            \
  int fd;                       \
  struct sockaddr_in sockaddr;

/*** Ebb Client ***/
void ebb_client_close(ebb_client*);
/* user MUST call this function on each client passed by the request_cb */
void ebb_client_release(ebb_client*);
int ebb_client_read(ebb_client *client, char *buffer, int length);
void ebb_client_write_status(ebb_client*, int status, const char *reason_phrase);
void ebb_client_write_header(ebb_client*, const char *field, const char *value);
void ebb_client_write_body(ebb_client*, const char *data, int length);
/* int ebb_client_should_keep_alive(ebb_client*); */

struct ebb_env_item {
  int type;
  const char *field;
  int field_length;
  const char *value;
  int value_length;
};

struct ebb_client {
  EBB_TCP_COMMON
  char *ip;
  unsigned int in_use : 1;
  
  ebb_server *server;
  http_parser parser;
  
  char *request_buffer;
  ev_io read_watcher;
  size_t read;
  
  char *body_head;
  size_t body_head_len;
  
  ev_io write_watcher;
  GString *response_buffer;
  size_t written;
  
  ev_timer timeout_watcher;
  unsigned int keep_alive : 1;
  unsigned int status_written : 1;
  unsigned int headers_written : 1;
  unsigned int body_written : 1;
  
  /* the ENV structure */
  int env_size;
  struct ebb_env_item env[EBB_MAX_ENV];
};

/*** Ebb Server ***/

typedef void (*ebb_request_cb)(ebb_client*, void*);

ebb_server* ebb_server_alloc(void);
void ebb_server_free(ebb_server*);
void ebb_server_init( ebb_server *server
                    , struct ev_loop *loop
                    , ebb_request_cb request_cb
                    , void *request_cb_data
                    );
int ebb_server_listen_on_fd(ebb_server*, const int sfd);
int ebb_server_listen_on_port(ebb_server*, const int port);
int ebb_server_listen_on_unix_socket(ebb_server*, const char *socketpath);
void ebb_server_unlisten(ebb_server*);
int ebb_server_clients_in_use_p(ebb_server*);

struct ebb_server {
  EBB_TCP_COMMON
  char *port;
  char *socketpath;
  ev_io request_watcher;
  ebb_client clients[EBB_MAX_CLIENTS];
  struct ev_loop *loop;
  void *request_cb_data;
  ebb_request_cb request_cb;
};

#endif
