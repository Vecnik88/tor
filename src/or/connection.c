/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

/********* START VARIABLES **********/

extern or_options_t options; /* command-line and config-file options */

char *conn_type_to_string[] = {
  "",            /* 0 */
  "OP listener", /* 1 */
  "OP",          /* 2 */
  "OR listener", /* 3 */
  "OR",          /* 4 */
  "Exit",        /* 5 */
  "App listener",/* 6 */
  "App",         /* 7 */
  "Dir listener",/* 8 */
  "Dir",         /* 9 */
  "DNS master",  /* 10 */
};

char *conn_state_to_string[][15] = {
  { },         /* no type associated with 0 */
  { "ready" }, /* op listener, 0 */
  { "awaiting keys", /* op, 0 */
    "open",              /* 1 */
    "close",             /* 2 */
    "close_wait" },      /* 3 */
  { "ready" }, /* or listener, 0 */
  { "connecting (as OP)",       /* or, 0 */
    "sending keys (as OP)",         /* 1 */
    "connecting (as client)",       /* 2 */
    "sending auth (as client)",     /* 3 */
    "waiting for auth (as client)", /* 4 */
    "sending nonce (as client)",    /* 5 */
    "waiting for auth (as server)", /* 6 */
    "sending auth (as server)",     /* 7 */
    "waiting for nonce (as server)",/* 8 */
    "open" },                       /* 9 */
  { "waiting for dest info",     /* exit, 0 */
    "connecting",                      /* 1 */
    "open" },                          /* 2 */
  { "ready" }, /* app listener, 0 */
  { "", /* 0 */
    "", /* 1 */
    "", /* 2 */
    "awaiting dest info",         /* app, 3 */
    "waiting for OR connection",       /* 4 */
    "open" },                          /* 5 */
  { "ready" }, /* dir listener, 0 */
  { "connecting",                      /* 0 */
    "sending command",                 /* 1 */
    "reading",                         /* 2 */
    "awaiting command",                /* 3 */
    "writing" },                       /* 4 */
  { "open" }, /* dns master, 0 */
};

/********* END VARIABLES ************/


/**************************************************************/

connection_t *connection_new(int type) {
  connection_t *conn;
  struct timeval now;

  my_gettimeofday(&now);

  conn = (connection_t *)malloc(sizeof(connection_t));
  if(!conn)
    return NULL;
  memset(conn,0,sizeof(connection_t)); /* zero it out to start */

  conn->type = type;
  if(buf_new(&conn->inbuf, &conn->inbuflen, &conn->inbuf_datalen) < 0 ||
     buf_new(&conn->outbuf, &conn->outbuflen, &conn->outbuf_datalen) < 0)
    return NULL;

  conn->receiver_bucket = 10240; /* should be enough to do the handshake */
  conn->bandwidth = conn->receiver_bucket / 10; /* give it a default */

  conn->timestamp_created = now.tv_sec;
  conn->timestamp_lastread = now.tv_sec;
  conn->timestamp_lastwritten = now.tv_sec;

  if (connection_speaks_cells(conn)) {
    conn->f_crypto = crypto_new_cipher_env(CRYPTO_CIPHER_3DES);
    if (!conn->f_crypto) {
      free((void *)conn);
      return NULL;
    }
    conn->b_crypto = crypto_new_cipher_env(CRYPTO_CIPHER_3DES);
    if (!conn->b_crypto) {
      crypto_free_cipher_env(conn->f_crypto);
      free((void *)conn);
      return NULL;
    }
  }
#ifdef USE_ZLIB
  if (type == CONN_TYPE_AP || type == CONN_TYPE_EXIT)  {
    if (buf_new(&conn->z_outbuf, &conn->z_outbuflen, &conn->z_outbuf_datalen) < 0)
      return NULL;
    if (! (conn->compression = compression_new()))
      return NULL;
    if (! (conn->decompression = decompression_new()))
      return NULL;
  } else {
    conn->compression = conn->decompression = NULL;
  }
#endif
  conn->done_sending = conn->done_receiving = 0;
  return conn;
}

void connection_free(connection_t *conn) {
  assert(conn);

  buf_free(conn->inbuf);
  buf_free(conn->outbuf);
  if(conn->address)
    free(conn->address);
  if(conn->dest_addr)
    free(conn->dest_addr);

  if(connection_speaks_cells(conn)) {
    if (conn->f_crypto)
      crypto_free_cipher_env(conn->f_crypto);
    if (conn->b_crypto)
      crypto_free_cipher_env(conn->b_crypto);
  }

  if (conn->pkey)
    crypto_free_pk_env(conn->pkey);

  if(conn->s > 0) {
    log(LOG_INFO,"connection_free(): closing fd %d.",conn->s);
    close(conn->s);
  }
  if(conn->type == CONN_TYPE_OR) {
    directory_set_dirty();
  }
#ifdef USE_ZLIB
  if (conn->compression) {
    decompression_free(conn->decompression);
    compression_free(conn->compression);
    buf_free(conn->z_outbuf);
  }
#endif
  free(conn);
}

int connection_create_listener(struct sockaddr_in *bindaddr, int type) {
  connection_t *conn;
  int s;
  int one=1;

  s = socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0)
  { 
    log(LOG_ERR,"connection_create_listener(): Socket creation failed.");
    return -1;
  }

  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  if(bind(s,(struct sockaddr *)bindaddr,sizeof(*bindaddr)) < 0) {
    perror("bind ");
    log(LOG_ERR,"Could not bind to port %u.",ntohs(bindaddr->sin_port));
    return -1;
  }

  if(listen(s,SOMAXCONN) < 0) {
    log(LOG_ERR,"Could not listen on port %u.",ntohs(bindaddr->sin_port));
    return -1;
  }

  fcntl(s, F_SETFL, O_NONBLOCK); /* set s to non-blocking */

  conn = connection_new(type);
  if(!conn) {
    log(LOG_DEBUG,"connection_create_listener(): connection_new failed. Giving up.");
    return -1;
  }
  conn->s = s;

  if(connection_add(conn) < 0) { /* no space, forget it */
    log(LOG_DEBUG,"connection_create_listener(): connection_add failed. Giving up.");
    connection_free(conn);
    return -1;
  }

  log(LOG_DEBUG,"connection_create_listener(): Listening on port %u.",ntohs(bindaddr->sin_port));

  conn->state = LISTENER_STATE_READY;
  connection_start_reading(conn);

  return 0;
}

int connection_handle_listener_read(connection_t *conn, int new_type, int new_state) {

  int news; /* the new socket */
  connection_t *newconn;
  struct sockaddr_in remote; /* information about the remote peer when connecting to other routers */
  int remotelen = sizeof(struct sockaddr_in); /* length of the remote address */

  news = accept(conn->s,(struct sockaddr *)&remote,&remotelen);
  if (news == -1) { /* accept() error */
    if(errno==EAGAIN)
      return 0; /* he hung up before we could accept(). that's fine. */
    /* else there was a real error. */
    log(LOG_ERR,"connection_handle_listener_read(): accept() failed. Closing.");
    return -1;
  }
  log(LOG_INFO,"Connection accepted on socket %d (child of fd %d).",news, conn->s);

  fcntl(news, F_SETFL, O_NONBLOCK); /* set news to non-blocking */

  newconn = connection_new(new_type);
  newconn->s = news;

  if(!connection_speaks_cells(newconn)) {
    newconn->receiver_bucket = -1;
    newconn->bandwidth = -1;
  }

  newconn->address = strdup(inet_ntoa(remote.sin_addr)); /* remember the remote address */
  newconn->addr = ntohl(remote.sin_addr.s_addr);
  newconn->port = ntohs(remote.sin_port);

  if(connection_add(newconn) < 0) { /* no space, forget it */
    connection_free(newconn);
    return 0; /* no need to tear down the parent */
  }

  log(LOG_DEBUG,"connection_handle_listener_read(): socket %d entered state %d.",newconn->s, new_state);
  newconn->state = new_state;
  connection_start_reading(newconn);

  return 0;
}

int retry_all_connections(uint16_t or_listenport,
  uint16_t op_listenport, uint16_t ap_listenport, uint16_t dir_listenport) {

  /* start all connections that should be up but aren't */

  struct sockaddr_in bindaddr; /* where to bind */

  if(or_listenport) {
    router_retry_connections();
  }

  memset(&bindaddr,0,sizeof(struct sockaddr_in));
  bindaddr.sin_family = AF_INET;
  bindaddr.sin_addr.s_addr = htonl(INADDR_ANY); /* anyone can connect */

  if(or_listenport) {
    bindaddr.sin_port = htons(or_listenport);
    if(!connection_get_by_type(CONN_TYPE_OR_LISTENER)) {
      connection_or_create_listener(&bindaddr);
    }
  }

  if(op_listenport) {
    bindaddr.sin_port = htons(op_listenport);
    if(!connection_get_by_type(CONN_TYPE_OP_LISTENER)) {
      connection_op_create_listener(&bindaddr);
    }
  }

  if(dir_listenport) {
    bindaddr.sin_port = htons(dir_listenport);
    if(!connection_get_by_type(CONN_TYPE_DIR_LISTENER)) {
      connection_dir_create_listener(&bindaddr);
    }
  }
 
  if(ap_listenport) {
    bindaddr.sin_port = htons(ap_listenport);
    inet_aton("127.0.0.1", &(bindaddr.sin_addr)); /* the AP listens only on localhost! */
    /* XXX inet_aton is missing on solaris. use something simpler? */
    if(!connection_get_by_type(CONN_TYPE_AP_LISTENER)) {
      connection_ap_create_listener(&bindaddr);
    }
  }

  return 0;
}

int connection_read_to_buf(connection_t *conn) {
  int read_result;
  struct timeval now;

  if(connection_speaks_cells(conn)) {
    assert(conn->receiver_bucket >= 0);
  }
  if(!connection_speaks_cells(conn)) {
    assert(conn->receiver_bucket < 0);
  }

  my_gettimeofday(&now);

  conn->timestamp_lastread = now.tv_sec;

  read_result = read_to_buf(conn->s, conn->receiver_bucket, &conn->inbuf, &conn->inbuflen,
                            &conn->inbuf_datalen, &conn->inbuf_reached_eof);
//  log(LOG_DEBUG,"connection_read_to_buf(): read_to_buf returned %d.",read_result);
  if(read_result >= 0 && connection_speaks_cells(conn)) {
//    log(LOG_DEBUG,"connection_read_to_buf(): Read %d, bucket now %d.",read_result,conn->receiver_bucket);
    conn->receiver_bucket -= read_result;
    if(conn->receiver_bucket <= 0) {

//      log(LOG_DEBUG,"connection_read_to_buf() stopping reading, receiver bucket full.");
      connection_stop_reading(conn);

      /* If we're not in 'open' state here, then we're never going to finish the
       * handshake, because we'll never increment the receiver_bucket. But we
       * can't check for that here, because the buf we just read might have enough
       * on it to finish the handshake. So we check for that in check_conn_read().
       */
    }
  }

  return read_result;
}

int connection_fetch_from_buf(char *string, int len, connection_t *conn) {
  return fetch_from_buf(string, len, &conn->inbuf, &conn->inbuflen, &conn->inbuf_datalen);
}

#ifdef USE_ZLIB
int connection_compress_from_buf(char *string, int len, connection_t *conn,
                                 int flush) {
  return compress_from_buf(string, len,
                           &conn->inbuf, &conn->inbuflen, &conn->inbuf_datalen,
                           conn->compression, flush);
}

int connection_decompress_to_buf(char *string, int len, connection_t *conn,
                                 int flush) {
  int n;
  struct timeval now;

  assert(conn);

  if (len) {
    if (write_to_buf(string, len, 
        &conn->z_outbuf, &conn->z_outbuflen, &conn->z_outbuf_datalen) < 0)
      return -1;
  }

  /* If we have more that 10 payloads worth of data waiting in outbuf, 
   * don't uncompress any more; queue this data in z_outbuf.
   *
   * This check should may be different.
   */
  if (connection_outbuf_too_full(conn))
    return 0;
  
  n = decompress_buf_to_buf(
        &conn->z_outbuf, &conn->z_outbuflen, &conn->z_outbuf_datalen,
        &conn->outbuf, &conn->outbuflen, &conn->outbuf_datalen,
        conn->decompression, flush);

  if (n < 0)
    return -1;

  my_gettimeofday(&now,NULL);
  
  if(!n)
    return 0;

  if(conn->marked_for_close)
    return 0;

  conn->timestamp_lastwritten = now.tv_sec;
  conn->outbuf_flushlen += n;

  return n;
}
#endif

int connection_find_on_inbuf(char *string, int len, connection_t *conn) {
  return find_on_inbuf(string, len, conn->inbuf, conn->inbuf_datalen);
}

int connection_wants_to_flush(connection_t *conn) {
  return conn->outbuf_flushlen;
}

int connection_outbuf_too_full(connection_t *conn) {
  return (conn->outbuf_flushlen > 10*CELL_PAYLOAD_SIZE);
}

int connection_flush_buf(connection_t *conn) {
  return flush_buf(conn->s, &conn->outbuf, &conn->outbuflen, &conn->outbuf_flushlen, &conn->outbuf_datalen);
}

int connection_write_to_buf(char *string, int len, connection_t *conn) {
  struct timeval now;

  my_gettimeofday(&now);

  if(!len)
    return 0;

  if(conn->marked_for_close)
    return 0;

  conn->timestamp_lastwritten = now.tv_sec;

  if( (!connection_speaks_cells(conn)) ||
      (!connection_state_is_open(conn)) ||
      (options.LinkPadding == 0) ) {
    /* connection types other than or and op, or or/op not in 'open' state, should flush immediately */
    /* also flush immediately if we're not doing LinkPadding, since otherwise it will never flush */
    connection_start_writing(conn);
    conn->outbuf_flushlen += len;
  }

  return write_to_buf(string, len, &conn->outbuf, &conn->outbuflen, &conn->outbuf_datalen);
}

int connection_receiver_bucket_should_increase(connection_t *conn) {
  assert(conn);

  if(!connection_speaks_cells(conn))
    return 0; /* edge connections don't use receiver_buckets */

  if(conn->receiver_bucket > 10*conn->bandwidth)
    return 0;

  return 1;
}

void connection_increment_receiver_bucket(connection_t *conn) {
  assert(conn);

  if(connection_receiver_bucket_should_increase(conn)) {
    /* yes, the receiver_bucket can become overfull here. But not by much. */
    conn->receiver_bucket += conn->bandwidth*1.1;
//    log(LOG_DEBUG,"connection_increment_receiver_bucket(): Bucket now %d.",conn->receiver_bucket);
    if(connection_state_is_open(conn)) {
      /* if we're in state 'open', then start reading again */
      connection_start_reading(conn);
    }
  }
}

int connection_speaks_cells(connection_t *conn) {
  assert(conn);

  if(conn->type == CONN_TYPE_OR || conn->type == CONN_TYPE_OP)
    return 1;

  return 0;
}

int connection_is_listener(connection_t *conn) {
  if(conn->type == CONN_TYPE_OP_LISTENER ||
     conn->type == CONN_TYPE_OR_LISTENER ||
     conn->type == CONN_TYPE_AP_LISTENER ||
     conn->type == CONN_TYPE_DIR_LISTENER)
    return 1;
  return 0;
}

int connection_state_is_open(connection_t *conn) {
  assert(conn);

  if((conn->type == CONN_TYPE_OR && conn->state == OR_CONN_STATE_OPEN) ||
     (conn->type == CONN_TYPE_OP && conn->state == OP_CONN_STATE_OPEN) ||
     (conn->type == CONN_TYPE_AP && conn->state == AP_CONN_STATE_OPEN) ||
     (conn->type == CONN_TYPE_EXIT && conn->state == EXIT_CONN_STATE_OPEN))
    return 1;

  return 0;
}

void connection_send_cell(connection_t *conn) {
  cell_t cell;
  int bytes_in_full_flushlen;

  /* this function only gets called if options.LinkPadding is 1 */
  assert(options.LinkPadding == 1);

  assert(conn);

  if(!connection_speaks_cells(conn)) {
    /* this conn doesn't speak cells. do nothing. */
    return;
  }

  if(!connection_state_is_open(conn)) {
    /* it's not in 'open' state, all data should already be waiting to be flushed */
    assert(conn->outbuf_datalen == conn->outbuf_flushlen);
    return;
  }

#if 0 /* use to send evenly spaced cells, but not padding */
  if(conn->outbuf_datalen - conn->outbuf_flushlen >= sizeof(cell_t)) {
    conn->outbuf_flushlen += sizeof(cell_t); /* instruct it to send a cell */
    connection_start_writing(conn);
  }
#endif

  connection_increment_send_timeval(conn); /* update when we'll send the next cell */

  bytes_in_full_flushlen = conn->bandwidth / 100; /* 10ms worth */
  if(bytes_in_full_flushlen < 10*sizeof(cell_t))
    bytes_in_full_flushlen = 10*sizeof(cell_t); /* but at least 10 cells worth */

  if(conn->outbuf_flushlen > bytes_in_full_flushlen - sizeof(cell_t)) {
    /* if we would exceed bytes_in_full_flushlen by adding a new cell */
    return;
  }

  if(conn->outbuf_datalen - conn->outbuf_flushlen < sizeof(cell_t)) {
    /* we need to queue a padding cell first */
    memset(&cell,0,sizeof(cell_t));
    cell.command = CELL_PADDING;
    connection_write_cell_to_buf(&cell, conn);
  }

  /* ???? If we might not have added a cell above, why are we
   * ???? increasing outbuf_flushlen? -NM */
  /* The connection_write_cell_to_buf() call doesn't increase the flushlen
   * (if link padding is on). So if there isn't a whole cell waiting-but-
   * not-yet-flushed, we add a padding cell. Thus in any case the gap between
   * outbuf_datalen and outbuf_flushlen is at least sizeof(cell_t). -RD
   */
  /* XXXX actually, there are some subtle bugs lurking in here. They
   * have to do with the fact that we don't handle connection failure
   * cleanly. Sometimes we mark things to be closed later. Inside
   * connection_write_cell_to_buf, it returns successfully without
   * writing if the connection has been marked for close. We need to
   * look at all our failure cases more carefully and make sure they do
   * the right thing.
   */
  conn->outbuf_flushlen += sizeof(cell_t); /* instruct it to send a cell */
  connection_start_writing(conn);
}

void connection_increment_send_timeval(connection_t *conn) {
  /* add "1000000 * sizeof(cell_t) / conn->bandwidth" microseconds to conn->send_timeval */
  /* FIXME should perhaps use ceil() of this. For now I simply add 1. */

  tv_addms(&conn->send_timeval, 1+1000 * sizeof(cell_t) / conn->bandwidth);
}

void connection_init_timeval(connection_t *conn) {

  assert(conn);

  my_gettimeofday(&conn->send_timeval);

  connection_increment_send_timeval(conn);
}

int connection_send_destroy(aci_t aci, connection_t *conn) {
  cell_t cell;

  assert(conn);

  if(!connection_speaks_cells(conn)) {
     log(LOG_INFO,"connection_send_destroy(): Aci %d: At an edge. Marking connection for close.", aci);
     conn->marked_for_close = 1;
     return 0;
  }

  memset(&cell, 0, sizeof(cell_t));
  cell.aci = aci;
  cell.command = CELL_DESTROY;
  log(LOG_INFO,"connection_send_destroy(): Sending destroy (aci %d).",aci);
  return connection_write_cell_to_buf(&cell, conn);
}

int connection_write_cell_to_buf(const cell_t *cellp, connection_t *conn) {
  char networkcell[CELL_NETWORK_SIZE];
  char *n = networkcell;
 
  cell_pack(n, cellp);

  if(connection_encrypt_cell(n,conn)<0) {
    return -1;
  }

  return connection_write_to_buf(n, CELL_NETWORK_SIZE, conn);
}

int connection_encrypt_cell(char *cellp, connection_t *conn) {
  char cryptcell[CELL_NETWORK_SIZE];
#if 0
  int x;
  char *px;

  printf("Sending: Cell header plaintext: ");
  px = (char *)cellp;
  for(x=0;x<8;x++) {
    printf("%u ",px[x]);
  } 
  printf("\n");
#endif

  if(crypto_cipher_encrypt(conn->f_crypto, cellp, CELL_NETWORK_SIZE, cryptcell)) {
    log(LOG_ERR,"Could not encrypt cell for connection %s:%u.",conn->address,conn->port);
    return -1;
  }
#if 0
  printf("Sending: Cell header crypttext: ");
  px = (char *)&newcell;
  for(x=0;x<8;x++) {
    printf("%u ",px[x]);
  }
  printf("\n");
#endif

  memcpy(cellp,cryptcell,CELL_NETWORK_SIZE);
  return 0;
}

int connection_process_inbuf(connection_t *conn) {

  assert(conn);

  switch(conn->type) {
    case CONN_TYPE_OP:
      return connection_op_process_inbuf(conn);
    case CONN_TYPE_OR:
      return connection_or_process_inbuf(conn);
    case CONN_TYPE_EXIT:
    case CONN_TYPE_AP:
      return connection_edge_process_inbuf(conn);
    case CONN_TYPE_DIR:
      return connection_dir_process_inbuf(conn);
    case CONN_TYPE_DNSMASTER:
      return connection_dns_process_inbuf(conn); 
    default:
      log(LOG_DEBUG,"connection_process_inbuf() got unexpected conn->type.");
      return -1;
  }
}

int connection_package_raw_inbuf(connection_t *conn) {
  int amount_to_process, len;
  cell_t cell;
  circuit_t *circ;

  assert(conn);
  assert(!connection_speaks_cells(conn));
  /* this function should never get called if the receive_streamwindow is 0 */
 
repeat_connection_package_raw_inbuf:

  amount_to_process = conn->inbuf_datalen;
  
  if(!amount_to_process)
    return 0;

  /* Initialize the cell with 0's */
  memset(&cell, 0, sizeof(cell_t));

#ifdef USE_ZLIB
  /* This compression logic is not necessarily optimal:
   *    1) Maybe we should try to read as much as we can onto the inbuf before
   *       compressing.
   *    2) 
   */
  len = connection_compress_from_buf(cell.payload+RELAY_HEADER_SIZE,
                                     CELL_PAYLOAD_SIZE - RELAY_HEADER_SIZE,
                                     conn, Z_SYNC_FLUSH);
  if (len < 0)
    return -1;

  cell.length = len;    
#else 
  if(amount_to_process > CELL_PAYLOAD_SIZE - RELAY_HEADER_SIZE) {
    cell.length = CELL_PAYLOAD_SIZE - RELAY_HEADER_SIZE;
  } else {
    cell.length = amount_to_process;
  }

  if(connection_fetch_from_buf(cell.payload+RELAY_HEADER_SIZE, 
                               cell.length, conn) < 0)
    return -1;
#endif

  circ = circuit_get_by_conn(conn);
  if(!circ) {
    log(LOG_DEBUG,"connection_package_raw_inbuf(): conn has no circuits!");
    return -1;
  }

  log(LOG_DEBUG,"connection_package_raw_inbuf(): (%d) Packaging %d bytes (%d waiting).",conn->s,cell.length, conn->inbuf_datalen);


  cell.command = CELL_RELAY;
  SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_DATA);
  SET_CELL_STREAM_ID(cell, conn->stream_id);
  cell.length += RELAY_HEADER_SIZE;

  if(conn->type == CONN_TYPE_EXIT) {
    cell.aci = circ->p_aci;
    if(circuit_deliver_relay_cell_from_edge(&cell, circ, EDGE_EXIT, NULL) < 0) {
      log(LOG_DEBUG,"connection_package_raw_inbuf(): circuit_deliver_relay_cell_from_edge (backward) failed. Closing.");
      circuit_close(circ);
      return 0;
    }
    assert(conn->n_receive_streamwindow > 0);
    if(--conn->n_receive_streamwindow <= 0) { /* is it 0 after decrement? */
      connection_stop_reading(conn);
      log(LOG_DEBUG,"connection_package_raw_inbuf(): receive_streamwindow at exit reached 0.");
      return 0; /* don't process the inbuf any more */
    }
    log(LOG_DEBUG,"connection_package_raw_inbuf(): receive_streamwindow at exit is %d",conn->n_receive_streamwindow);
  } else { /* send it forward. we're an AP */
    assert(conn->type == CONN_TYPE_AP);
    cell.aci = circ->n_aci;
    if(circuit_deliver_relay_cell_from_edge(&cell, circ, EDGE_AP, conn->cpath_layer) < 0) {
      log(LOG_DEBUG,"connection_package_raw_inbuf(): circuit_deliver_relay_cell_from_edge (forward) failed. Closing.");
      circuit_close(circ);
      return 0;
    }
    assert(conn->p_receive_streamwindow > 0);
    if(--conn->p_receive_streamwindow <= 0) { /* is it 0 after decrement? */
      connection_stop_reading(conn);
      log(LOG_DEBUG,"connection_package_raw_inbuf(): receive_streamwindow at AP reached 0.");
      return 0; /* don't process the inbuf any more */
    }
    log(LOG_DEBUG,"connection_package_raw_inbuf(): receive_streamwindow at AP is %d",conn->p_receive_streamwindow);
  }
  /* handle more if there's more, or return 0 if there isn't */
  goto repeat_connection_package_raw_inbuf;
}

int connection_consider_sending_sendme(connection_t *conn, int edge_type) {
  circuit_t *circ;
  cell_t cell;

  if(connection_outbuf_too_full(conn))
    return 0;

  circ = circuit_get_by_conn(conn);
  if(!circ) {
    /* this can legitimately happen if the destroy has already arrived and torn down the circuit */
    log(LOG_DEBUG,"connection_consider_sending_sendme(): No circuit associated with conn. Skipping.");
    return 0;
  }

  memset(&cell, 0, sizeof(cell_t));
  cell.command = CELL_RELAY;
  SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_SENDME);
  SET_CELL_STREAM_ID(cell, conn->stream_id);
  cell.length += RELAY_HEADER_SIZE;

  if(edge_type == EDGE_EXIT) { /* we're at an exit */
    if(conn->p_receive_streamwindow < STREAMWINDOW_START - STREAMWINDOW_INCREMENT) {
      log(LOG_DEBUG,"connection_consider_sending_sendme(): Outbuf %d, Queueing stream sendme back.", conn->outbuf_flushlen);
      conn->p_receive_streamwindow += STREAMWINDOW_INCREMENT;
      cell.aci = circ->p_aci;
      if(circuit_deliver_relay_cell_from_edge(&cell, circ, edge_type, NULL) < 0) {
        log(LOG_DEBUG,"connection_consider_sending_sendme(): circuit_deliver_relay_cell_from_edge (backward) failed. Closing.");
        circuit_close(circ);
        return 0;
      }
    }
  } else { /* we're at an AP */
    assert(edge_type == EDGE_AP);
    if(conn->n_receive_streamwindow < STREAMWINDOW_START-STREAMWINDOW_INCREMENT) {
      log(LOG_DEBUG,"connection_consider_sending_sendme(): Outbuf %d, Queueing stream sendme forward.", conn->outbuf_flushlen);
      conn->n_receive_streamwindow += STREAMWINDOW_INCREMENT;
      cell.aci = circ->n_aci;
      if(circuit_deliver_relay_cell_from_edge(&cell, circ, edge_type, conn->cpath_layer) < 0) {
        log(LOG_DEBUG,"connection_consider_sending_sendme(): circuit_deliver_relay_cell_from_edge (forward) failed. Closing.");
        circuit_close(circ);
        return 0;
      }
    }
  }
  return 0;
} 

int connection_finished_flushing(connection_t *conn) {

  assert(conn);

//  log(LOG_DEBUG,"connection_finished_flushing() entered. Socket %u.", conn->s);

  switch(conn->type) {
    case CONN_TYPE_OP:
      return connection_op_finished_flushing(conn);
    case CONN_TYPE_OR:
      return connection_or_finished_flushing(conn);
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      return connection_edge_finished_flushing(conn);
    case CONN_TYPE_DIR:
      return connection_dir_finished_flushing(conn);
    case CONN_TYPE_DNSMASTER:
      return connection_dns_finished_flushing(conn);
    default:
      log(LOG_DEBUG,"connection_finished_flushing() got unexpected conn->type.");
      return -1;
  }
}

int connection_process_cell_from_inbuf(connection_t *conn) {
  /* check if there's a whole cell there.
   * if yes, pull it off, decrypt it, and process it.
   */
  char crypted[CELL_NETWORK_SIZE];
  char outbuf[1024];
//  int x;
  cell_t cell;

  if(conn->inbuf_datalen < CELL_NETWORK_SIZE) /* entire response available? */
    return 0; /* not yet */

  if(connection_fetch_from_buf(crypted,CELL_NETWORK_SIZE,conn) < 0) {
    return -1;
  }

#if 0
  printf("Cell header crypttext: ");
  for(x=0;x<8;x++) {
    printf("%u ",crypted[x]);
  }
  printf("\n");
#endif
  /* decrypt */
  if(crypto_cipher_decrypt(conn->b_crypto,crypted,CELL_NETWORK_SIZE,outbuf)) {
    log(LOG_ERR,"connection_process_cell_from_inbuf(): Decryption failed, dropping.");
    return connection_process_inbuf(conn); /* process the remainder of the buffer */
  }
//  log(LOG_DEBUG,"connection_process_cell_from_inbuf(): Cell decrypted (%d bytes).",outlen);
#if 0
  printf("Cell header plaintext: ");
  for(x=0;x<8;x++) {
    printf("%u ",outbuf[x]);
  }
  printf("\n");
#endif

  /* retrieve cell info from outbuf (create the host-order struct from the network-order string) */
  cell_unpack(&cell, outbuf);

//  log(LOG_DEBUG,"connection_process_cell_from_inbuf(): Decrypted cell is of type %u (ACI %u).",cellp->command,cellp->aci);
  command_process_cell(&cell, conn);

  return connection_process_inbuf(conn); /* process the remainder of the buffer */
}

void
cell_pack(char *dest, const cell_t *src)
{
  *(uint16_t*)dest     = htons(src->aci);
  *(uint8_t*)(dest+2)  = src->command;
  *(uint8_t*)(dest+3)  = src->length;
  *(uint32_t*)(dest+4) = 0; /* Reserved */
  memcpy(dest+8, src->payload, CELL_PAYLOAD_SIZE);
}

void
cell_unpack(cell_t *dest, const char *src)
{
  dest->aci     = ntohs(*(uint16_t*)(src));
  dest->command = *(uint8_t*)(src+2);
  dest->length  = *(uint8_t*)(src+3);
  dest->seq     = ntohl(*(uint32_t*)(src+4));
  memcpy(dest->payload, src+8, CELL_PAYLOAD_SIZE);
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
