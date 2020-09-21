#include <string.h>
#include <openssl/pem.h>
#include <openssl/engine.h>

#include <log.h>

#include <picotls.h>
#include <picotcpls.h>
#include "picotls/openssl.h"

#include "convert_tcpls.h"

const char * cert = "hello";
const char * cert_key = "hello";

static ptls_context_t *ctx;
static tcpls_t *tcpls;
static struct cli_data cli_data;
static list_t *tcpls_con_l;

static int handle_mpjoin(int socket, uint8_t *connid, uint8_t *cookie, uint32_t transportid, void *cbdata) ;
static int handle_connection_event(tcpls_event_t event, int socket, int transportid, void *cbdata) ;
static int handle_stream_event(tcpls_t *tcpls, tcpls_event_t event,
    streamid_t streamid, int transportid, void *cbdata);
static int handle_client_connection_event(tcpls_event_t event, int socket, int transportid, void *cbdata) ;
static int handle_client_stream_event(tcpls_t *tcpls, tcpls_event_t event, streamid_t streamid,
    int transportid, void *cbdata);
    
static int handle_connection_event(tcpls_event_t event, int socket, int transportid, void *cbdata) {
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con;
  assert(conntcpls);
  switch(event){
    case CONN_OPENED:
      log_debug("CONN OPENED %d:%d:%d\n", socket, event, transportid);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if (con->sd == socket) {
          con->transportid = transportid;
          con->state = CONNECTED;
          break;
        }
      }     
      break;
    case CONN_CLOSED:
      log_debug("CONN CLOSED %d:%d\n",socket, event);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if (con->sd == socket) {
          list_remove(conntcpls, con);
          log_debug("CONN CLOSED REMOVE %d:%d\n",socket, event);
          break;
        }
      }
      break;
    default:
      break;
  }
  return 0;
}

static int handle_client_connection_event(tcpls_event_t event, int socket, int transportid, void *cbdata) {
  struct cli_data *data = (struct cli_data*) cbdata;
  connect_info_t *con = NULL;
  for(int i = 0; i < tcpls->connect_infos->size; i++){
    con = list_get(tcpls->connect_infos, i);
    if(con->socket == socket){
      break;
    }
  }
  switch (event) {
    case CONN_CLOSED:
      log_debug("Received a CONN_CLOSED; removing the socket (%d) (%d) \n", socket, transportid);
      list_remove(data->socklist, &socket); 
      break;
    case CONN_OPENED:
      log_debug("Received a CONN_OPENED; adding the socket %d\n", socket);
      list_add(data->socklist, &socket);
      log_debug("Received a CONN_OPENED; adding the socket %d:%p\n", socket, tcpls);
      break;
    default: break;
  }
  return 0;
}
    
static int handle_client_stream_event(tcpls_t *tcpls, tcpls_event_t event, streamid_t streamid,
    int transportid, void *cbdata) {
  struct cli_data *data = (struct cli_data*) cbdata;
  switch (event) {
    case STREAM_OPENED:
      log_debug("Handling stream_opened callback %p:%d\n", tcpls, transportid);
      list_add(data->streamlist, &streamid);
      break;
    case STREAM_CLOSED:
      log_debug("Handling stream_closed callback %p:%d\n", tcpls, transportid);
      list_remove(data->streamlist, &streamid);
      break;
    default: break;
  }
  return 0;
}

static int handle_stream_event(tcpls_t *tcpls, tcpls_event_t event,
  streamid_t streamid, int transportid, void *cbdata) {
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con;
  assert(conntcpls);
  switch(event){
    case STREAM_OPENED:
      log_debug("STREAM OPENED %d %d %d (%d)\n", streamid, event, transportid, tcpls->streams->size);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if (con->tcpls == tcpls && con->transportid == transportid) {
          log_debug("Setting streamid %u as wants to write %d %d\n", streamid, transportid, con->sd);
          con->streamid = streamid;
          con->is_primary = 1;
          con->wants_to_write = 1;
        }
      }
      break;
    case STREAM_CLOSED:
      log_debug("STREAM CLOSED %d %d %d\n", streamid, event, transportid);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if ( con->tcpls == tcpls && con->transportid == transportid) {
          log_debug("We're stopping to write on the connection linked to transportid %d %d\n", transportid, con->sd);
          con->is_primary = 0;
          con->wants_to_write = 0;
        }
      }
      break;
    default:
      break;
  }
  return 0;
}

static int load_private_key(ptls_context_t *ctx, const char *fn){
  static ptls_openssl_sign_certificate_t sc;
  FILE *fp;
  EVP_PKEY *pkey;
  if ((fp = fopen(fn, "rb")) == NULL) {
    log_debug("failed to open file:%s:%s\n", fn, strerror(errno));
    return(-1);
  }
  pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  fclose(fp);
  if (pkey == NULL) {
    log_debug("failed to read private key from file:%s\n", fn);
    return(-1);
  }
  ptls_openssl_init_sign_certificate(&sc, pkey);
  EVP_PKEY_free(pkey);
  ctx->sign_certificate = &sc.super;
  return(0);
}

static ptls_context_t *set_tcpls_ctx_options(int is_server){
  if(ctx)
    goto done;
  ERR_load_crypto_strings();
  OpenSSL_add_all_algorithms();
  ctx = (ptls_context_t *)malloc(sizeof(*ctx));
  memset(ctx, 0, sizeof(ptls_context_t));
  if (ptls_load_certificates(ctx, (char *)cert) != 0)
    log_debug("failed to load certificate:%s:%s\n", cert, strerror(errno));
  load_private_key(ctx, (char*)cert_key);
    
  ctx->support_tcpls_options = 1;
  ctx->random_bytes = ptls_openssl_random_bytes;
  ctx->key_exchanges = ptls_openssl_key_exchanges;
  ctx->cipher_suites = ptls_openssl_cipher_suites;
  ctx->get_time = &ptls_get_time;
  if(!is_server){
    ctx->send_change_cipher_spec = 1;
    list_t *socklist = new_list(sizeof(int), 2);
    list_t *streamlist = new_list(sizeof(tcpls_stream_t), 2);
    cli_data.socklist = socklist;
    cli_data.streamlist = streamlist;
    ctx->cb_data = &cli_data;
    ctx->stream_event_cb = &handle_client_stream_event;
    ctx->connection_event_cb = &handle_client_connection_event;
  }else{
    ctx->stream_event_cb = &handle_stream_event;  
    ctx->connection_event_cb = &handle_connection_event;  
  }
done:
  return ctx;
}


static int handle_mpjoin(int socket, uint8_t *connid, uint8_t *cookie, uint32_t transportid, void *cbdata) {
  int i, j;
  log_debug("\n\n\nstart mpjoin haha %d %d %d %d %p\n", socket, *connid, *cookie, transportid, cbdata);
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con, *con2;
  assert(conntcpls);
  for(i = 0; i<conntcpls->size; i++){
    con = list_get(conntcpls, i);
    if(!memcmp(con->tcpls->connid, connid, CONNID)){
      log_debug("start mpjoin found %d:%p:%d\n", *con->tcpls->connid, con->tcpls, con->sd);
      for(j = 0; j < conntcpls->size; j++){
        con2 = list_get(conntcpls, j);
        log_debug("start mpjoin 1 found %d:%p:%d\n", *con->tcpls->connid, con2->tcpls, con2->sd);
        if(con2->sd == socket){
          con2->tcpls = con->tcpls;
          if(memcmp(con2->tcpls, con->tcpls, sizeof(tcpls_t)))
            log_debug("ils sont bien diff2rents\n");
        }
        log_debug("start mpjoin 2 found %d:%p:%d\n", *con->tcpls->connid, con2->tcpls, con2->sd); 
      }
      return tcpls_accept(con->tcpls, socket, cookie, transportid);
    }
  }
  return -1;
}

int do_tcpls_handshake(int sd){
  int i, ret = -1, found = 0;
  struct tcpls_con *con = NULL;
  for(i = 0; i < tcpls_con_l->size ; i++){
    con = list_get(tcpls_con_l, i);
      if(con->sd == sd){
        found = 1 ;
        break;
      }
  }
  ptls_handshake_properties_t prop = {NULL};
  memset(&prop, 0, sizeof(prop));
  prop.socket = sd;
  prop.received_mpjoin_to_process = &handle_mpjoin;
  log_debug("TCPLS:TLS con %p:%p:%d\n", con->tcpls, con->tcpls->tls, con->tcpls->tls->state);
  if ((ret = tcpls_handshake(con->tcpls->tls, &prop)) != 0) {
    if (ret == PTLS_ERROR_HANDSHAKE_IS_MPJOIN) {
      if(found && con->tcpls){
        log_debug("1 handshake conn: %d:%d:\n", con->sd, con->tcpls->streams->size); 
        log_debug("2 handshake conn: %d:%d:\n", con->sd, con->tcpls->streams->size);
      }
      return ret;
    }
    log_debug("tcpls_handshake failed with ret (%d)\n", ret);
  }
  return ret;
}

int _tcpls_init(int is_server){
  log_debug("Initialization of tcpls %d\n", is_server);
  set_tcpls_ctx_options(is_server);
  if(!is_server)
    tcpls = tcpls_new(ctx, is_server);
  return 0;
}

