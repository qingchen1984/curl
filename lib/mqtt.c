/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 2020, Daniel Stenberg, <daniel@haxx.se>, et al.
 * Copyright (C) 2019, Björn Stenberg, <bjorn@haxx.se>
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

#include "curl_setup.h"

#ifdef CURL_ENABLE_MQTT

#include "urldata.h"
#include <curl/curl.h>
#include "transfer.h"
#include "sendf.h"
#include "progress.h"
#include "mqtt.h"
#include "select.h"
#include "strdup.h"
#include "url.h"
#include "escape.h"
#include "warnless.h"
#include "curl_printf.h"
#include "curl_memory.h"
#include "multiif.h"
#include "rand.h"

/* The last #include file should be: */
#include "memdebug.h"

#define MQTT_MSG_CONNECT   0x10
#define MQTT_MSG_CONNACK   0x20
#define MQTT_MSG_PUBLISH   0x30
#define MQTT_MSG_SUBSCRIBE 0x82
#define MQTT_MSG_SUBACK    0x90
#define MQTT_MSG_DISCONNECT 0xe0

#define MQTT_CONNACK_LEN 4
#define MQTT_SUBACK_LEN 5
#define MQTT_CLIENTID_LEN 12 /* "curl0123abcd" */

/*
 * Forward declarations.
 */

static CURLcode mqtt_do(struct connectdata *conn, bool *done);
static CURLcode mqtt_doing(struct connectdata *conn, bool *done);
static int mqtt_getsock(struct connectdata *conn, curl_socket_t *sock);
static CURLcode mqtt_setup_conn(struct connectdata *conn);

/*
 * MQTT protocol handler.
 */

const struct Curl_handler Curl_handler_mqtt = {
  "MQTT",                             /* scheme */
  mqtt_setup_conn,                    /* setup_connection */
  mqtt_do,                            /* do_it */
  ZERO_NULL,                          /* done */
  ZERO_NULL,                          /* do_more */
  ZERO_NULL,                          /* connect_it */
  ZERO_NULL,                          /* connecting */
  mqtt_doing,                         /* doing */
  ZERO_NULL,                          /* proto_getsock */
  mqtt_getsock,                       /* doing_getsock */
  ZERO_NULL,                          /* domore_getsock */
  ZERO_NULL,                          /* perform_getsock */
  ZERO_NULL,                          /* disconnect */
  ZERO_NULL,                          /* readwrite */
  ZERO_NULL,                          /* connection_check */
  PORT_MQTT,                          /* defport */
  CURLPROTO_MQTT,                     /* protocol */
  PROTOPT_NONE                        /* flags */
};

static CURLcode mqtt_setup_conn(struct connectdata *conn)
{
  /* allocate the HTTP-specific struct for the Curl_easy, only to survive
     during this request */
  struct MQTT *mq;
  struct Curl_easy *data = conn->data;
  DEBUGASSERT(data->req.protop == NULL);

  mq = calloc(1, sizeof(struct MQTT));
  if(!mq)
    return CURLE_OUT_OF_MEMORY;
  data->req.protop = mq;
  return CURLE_OK;
}

static CURLcode mqtt_send(struct connectdata *conn,
                          char *buf, size_t len)
{
  CURLcode result = CURLE_OK;
  curl_socket_t sockfd = conn->sock[FIRSTSOCKET];
  struct Curl_easy *data = conn->data;
  struct MQTT *mq = data->req.protop;
  ssize_t n;
  result = Curl_write(conn, sockfd, buf, len, &n);
  if(!result && data->set.verbose)
    Curl_debug(data, CURLINFO_HEADER_OUT, buf, (size_t)n);
  if(len != (size_t)n) {
    size_t nsend = len - n;
    char *sendleftovers = Curl_memdup(&buf[n], nsend);
    if(!sendleftovers)
      return CURLE_OUT_OF_MEMORY;
    mq->sendleftovers = sendleftovers;
    mq->nsend = nsend;
  }
  return result;
}

/* Generic function called by the multi interface to figure out what socket(s)
   to wait for and for what actions during the DOING and PROTOCONNECT
   states */
static int mqtt_getsock(struct connectdata *conn,
                        curl_socket_t *sock)
{
  sock[0] = conn->sock[FIRSTSOCKET];
  return GETSOCK_READSOCK(FIRSTSOCKET);
}

static CURLcode mqtt_connect(struct connectdata *conn)
{
  CURLcode result = CURLE_OK;
  const size_t client_id_offset = 14;
  const size_t packetlen = client_id_offset + MQTT_CLIENTID_LEN;
  char client_id[MQTT_CLIENTID_LEN + 1] = "curl";
  const size_t curl_len = strlen("curl");
  char packet[32] = {
    MQTT_MSG_CONNECT,  /* packet type */
    0x00,              /* remaining length */
    0x00, 0x04,        /* protocol length */
    'M','Q','T','T',   /* protocol name */
    0x04,              /* protocol level */
    0x02,              /* CONNECT flag: CleanSession */
    0x00, 0x3c,        /* keep-alive 0 = disabled */
    0x00, 0x00         /* payload1 length */
  };
  packet[1] = (packetlen - 2) & 0x7f;
  packet[client_id_offset - 1] = MQTT_CLIENTID_LEN;

  result = Curl_rand_hex(conn->data, (unsigned char *)&client_id[curl_len],
                         MQTT_CLIENTID_LEN - curl_len + 1);
  memcpy(&packet[client_id_offset], client_id, MQTT_CLIENTID_LEN);
  infof(conn->data, "Using client id '%s'\n", client_id);
  if(!result)
    result = mqtt_send(conn, packet, packetlen);
  return result;
}

static CURLcode mqtt_disconnect(struct connectdata *conn)
{
  CURLcode result = CURLE_OK;
  result = mqtt_send(conn, (char *)"\xe0\x00", 2);
  return result;
}

static CURLcode mqtt_verify_connack(struct connectdata *conn)
{
  CURLcode result;
  curl_socket_t sockfd = conn->sock[FIRSTSOCKET];
  unsigned char readbuf[MQTT_CONNACK_LEN];
  ssize_t nread;
  struct Curl_easy *data = conn->data;

  result = Curl_read(conn, sockfd, (char *)readbuf, MQTT_CONNACK_LEN, &nread);
  if(result)
    goto fail;

  if(data->set.verbose)
    Curl_debug(data, CURLINFO_HEADER_IN, (char *)readbuf, (size_t)nread);

  /* fixme */
  if(nread < MQTT_CONNACK_LEN) {
    result = CURLE_WEIRD_SERVER_REPLY;
    goto fail;
  }

  /* verify CONNACK */
  if(readbuf[0] != MQTT_MSG_CONNACK ||
     readbuf[1] != 0x02 ||
     readbuf[2] != 0x00 ||
     readbuf[3] != 0x00) {
    failf(data, "Expected %02x%02x%02x%02x but got %02x%02x%02x%02x",
          MQTT_MSG_CONNACK, 0x02, 0x00, 0x00,
          readbuf[0], readbuf[1], readbuf[2], readbuf[3]);
    result = CURLE_WEIRD_SERVER_REPLY;
  }

fail:
  return result;
}

static CURLcode mqtt_get_topic(struct connectdata *conn,
                               char **topic, size_t *topiclen)
{
  CURLcode result = CURLE_OK;
  char *path = conn->data->state.up.path;

  if(strlen(path) > 1) {
    result = Curl_urldecode(conn->data, path + 1, 0, topic, topiclen, FALSE);
  }
  else {
    failf(conn->data, "Error: No topic specified.");
    result = CURLE_URL_MALFORMAT;
  }
  return result;
}


static int mqtt_encode_len(char *buf, size_t len)
{
  unsigned char encoded;
  int i;

  for(i = 0; (len > 0) && (i<4); i++) {
    encoded = len % 0x80;
    len /= 0x80;
    if(len)
      encoded |= 0x80;
    buf[i] = encoded;
  }

  return i;
}

static CURLcode mqtt_subscribe(struct connectdata *conn)
{
  CURLcode result = CURLE_OK;
  char *topic = NULL;
  size_t topiclen;
  unsigned char *packet = NULL;
  size_t packetlen;
  char encodedsize[4];
  size_t n;

  result = mqtt_get_topic(conn, &topic, &topiclen);
  if(result)
    goto fail;

  conn->proto.mqtt.packetid++;

  packetlen = topiclen + 5; /* packetid + topic (has a two byte length field)
                               + 2 bytes topic length + QoS byte */
  n = mqtt_encode_len((char *)encodedsize, packetlen);
  packetlen += n + 1; /* add one for the control packet type byte */

  packet = malloc(packetlen);
  if(!packet) {
    result = CURLE_OUT_OF_MEMORY;
    goto fail;
  }

  packet[0] = MQTT_MSG_SUBSCRIBE;
  memcpy(&packet[1], encodedsize, n);
  packet[1 + n] = (conn->proto.mqtt.packetid >> 8) & 0xff;
  packet[2 + n] = conn->proto.mqtt.packetid & 0xff;
  packet[3 + n] = (topiclen >> 8) & 0xff;
  packet[4 + n ] = topiclen & 0xff;
  memcpy(&packet[5 + n], topic, topiclen);
  packet[5 + n + topiclen] = 0; /* QoS zero */

  result = mqtt_send(conn, (char *)packet, packetlen);

fail:
  free(topic);
  free(packet);
  return result;
}

static CURLcode mqtt_verify_suback(struct connectdata *conn)
{
  CURLcode result;
  curl_socket_t sockfd = conn->sock[FIRSTSOCKET];
  unsigned char readbuf[MQTT_SUBACK_LEN];
  ssize_t nread;
  struct mqtt_conn *mqtt = &conn->proto.mqtt;

  result = Curl_read(conn, sockfd, (char *)readbuf, MQTT_SUBACK_LEN, &nread);
  if(result)
    goto fail;

  if(conn->data->set.verbose)
    Curl_debug(conn->data, CURLINFO_HEADER_IN, (char *)readbuf, (size_t)nread);

  /* fixme */
  if(nread < MQTT_SUBACK_LEN) {
    result = CURLE_WEIRD_SERVER_REPLY;
    goto fail;
  }

  /* verify SUBACK */
  if(readbuf[0] != MQTT_MSG_SUBACK ||
     readbuf[1] != 0x03 ||
     readbuf[2] != ((mqtt->packetid >> 8) & 0xff) ||
     readbuf[3] != (mqtt->packetid & 0xff) ||
     readbuf[4] != 0x00)
    result = CURLE_WEIRD_SERVER_REPLY;

fail:
  return result;
}

static CURLcode mqtt_publish(struct connectdata *conn)
{
  CURLcode result;
  char *payload = conn->data->set.postfields;
  size_t payloadlen = (size_t)conn->data->set.postfieldsize;
  char *topic = NULL;
  size_t topiclen;
  unsigned char *pkt = NULL;
  size_t i = 0;
  size_t remaininglength;
  size_t encodelen;
  char encodedbytes[4];

  result = mqtt_get_topic(conn, &topic, &topiclen);
  if(result)
    goto fail;

  remaininglength = payloadlen + 2 + topiclen;
  encodelen = mqtt_encode_len(encodedbytes, remaininglength);

  /* add the control byte and the encoded remaining length */
  pkt = malloc(remaininglength + 1 + encodelen);
  if(!pkt) {
    result = CURLE_OUT_OF_MEMORY;
    goto fail;
  }

  /* assemble packet */
  pkt[i++] = MQTT_MSG_PUBLISH;
  memcpy(&pkt[i], encodedbytes, encodelen);
  i += encodelen;
  pkt[i++] = (topiclen >> 8) & 0xff;
  pkt[i++] = (topiclen & 0xff);
  memcpy(&pkt[i], topic, topiclen);
  i += topiclen;
  memcpy(&pkt[i], payload, payloadlen);
  i += payloadlen;
  result = mqtt_send(conn, (char *)pkt, i);

fail:
  free(pkt);
  free(topic);
  return result;
}

static size_t mqtt_decode_len(unsigned char *buf,
                              size_t buflen, size_t *lenbytes)
{
  size_t len = 0;
  size_t mult = 1;
  size_t i;
  unsigned char encoded = 128;

  for(i = 0; (i < buflen) && (encoded & 128); i++) {
    encoded = buf[i];
    len += (encoded & 127) * mult;
    mult *= 128;
  }

  *lenbytes = i;

  return len;
}

/* for the publish packet */
#define MQTT_HEADER_LEN 5    /* max 5 bytes */

static CURLcode mqtt_read_publish(struct connectdata *conn,
                                  bool *done)
{
  CURLcode result;
  curl_socket_t sockfd = conn->sock[FIRSTSOCKET];
  ssize_t nread;
  struct Curl_easy *data = conn->data;
  unsigned char *pkt = (unsigned char *)data->state.buffer;
  size_t remlen, lenbytes;
  struct mqtt_conn *mqtt = &conn->proto.mqtt;
  struct MQTT *mq = data->req.protop;

  switch(mqtt->state) {
  case MQTT_SUBWAIT:
    /* Read the initial byte and the entire Remaining Length field
       in this state */
    result = Curl_read(conn, sockfd, (char *)&pkt[mq->npacket], 1, &nread);
    if(result)
      goto end;
    if(data->set.verbose)
      Curl_debug(data, CURLINFO_HEADER_IN, (char *)&pkt[mq->npacket], 1);
    /* we are expecting a PUBLISH message */
    if(!mq->npacket && ((pkt[0] & 0xf0) != MQTT_MSG_PUBLISH)) {
      if(pkt[0] == MQTT_MSG_DISCONNECT) {
        infof(data, "Got DISCONNECT\n");
        *done = TRUE;
        goto end;
      }
      result = CURLE_WEIRD_SERVER_REPLY;
      goto end;
    }
    else if((mq->npacket >= 1) && !(pkt[mq->npacket] & 0x80))
      /* as long as the high bit is set in the length byte, we read one more
         byte, then get the remainder of the PUBLISH */
      mqtt->state = MQTT_SUB_REMAIN;
    mq->npacket++;
    if(mqtt->state == MQTT_SUBWAIT)
      return result;

    /* -- switched state -- */

    /* remember the first byte */
    mq->firstbyte = pkt[0];

    remlen = mqtt_decode_len(&pkt[1], 4, &lenbytes);

    infof(data, "Remaining length: %zd bytes\n", remlen);
    Curl_pgrsSetDownloadSize(data, remlen);
    data->req.bytecount = 0;
    data->req.size = remlen;
    mq->npacket = remlen; /* get this many bytes */
    /* FALLTHROUGH */
  case MQTT_SUB_REMAIN: {
    /* read rest of packet, but no more. Cap to buffer size */
    struct SingleRequest *k = &data->req;
    size_t rest = mq->npacket;
    if(rest > (size_t)data->set.buffer_size)
      rest = (size_t)data->set.buffer_size;
    result = Curl_read(conn, sockfd, (char *)pkt, rest, &nread);
    if(result) {
      if(CURLE_AGAIN == result) {
        infof(data, "EEEE AAAAGAIN\n");
      }
      goto end;
    }
    if(data->set.verbose)
      Curl_debug(data, CURLINFO_DATA_IN, (char *)pkt, (size_t)nread);

    mq->npacket -= nread;
    k->bytecount += nread;
    Curl_pgrsSetDownloadCounter(data, k->bytecount);

    /* if QoS is set, message contains packet id */

    result = Curl_client_write(conn, CLIENTWRITE_BODY, (char *)pkt, nread);
    if(result)
      goto end;

    if(!mq->npacket)
      /* no more PUBLISH payload, back to subscribe wait state */
      mqtt->state = MQTT_SUBWAIT;
    break;
  }
  default:
    DEBUGASSERT(NULL); /* illegal state */
    result = CURLE_WEIRD_SERVER_REPLY;
    goto end;
  }
  end:
  return result;
}

static CURLcode mqtt_do(struct connectdata *conn, bool *done)
{
  CURLcode result = CURLE_OK;
  struct Curl_easy *data = conn->data;
  struct mqtt_conn *mqtt = &conn->proto.mqtt;

  *done = FALSE; /* unconditionally */

  result = mqtt_connect(conn);
  if(result) {
    failf(data, "Error %d sending MQTT CONN request", result);
    return result;
  }
  mqtt->state = MQTT_CONNACK;
  return CURLE_OK;
}

static CURLcode mqtt_doing(struct connectdata *conn, bool *done)
{
  CURLcode result = CURLE_OK;
  struct mqtt_conn *mqtt = &conn->proto.mqtt;
  struct Curl_easy *data = conn->data;
  struct MQTT *mq = data->req.protop;

  *done = FALSE;

  if(mq->nsend) {
    /* send the remainder of an outgoing packet */
    char *ptr = mq->sendleftovers;
    result = mqtt_send(conn, mq->sendleftovers, mq->nsend);
    free(ptr);
    if(result)
      return result;
  }

  switch(mqtt->state) {
  case MQTT_CONNACK:
    result = mqtt_verify_connack(conn);
    if(result)
      break;

    if(conn->data->set.httpreq == HTTPREQ_POST) {
      result = mqtt_publish(conn);
      if(!result) {
        result = mqtt_disconnect(conn);
        *done = TRUE;
      }
    }
    else {
      result = mqtt_subscribe(conn);
      if(!result)
        mqtt->state = MQTT_SUBACK;
    }
    break;

  case MQTT_SUBACK:
    result = mqtt_verify_suback(conn);
    if(result)
      break;

    mqtt->state = MQTT_SUBWAIT;
    break;

  case MQTT_SUBWAIT:
  case MQTT_SUB_REMAIN:
    result = mqtt_read_publish(conn, done);
    if(result)
      break;
    break;

  default:
    failf(conn->data, "State not handled yet");
    *done = TRUE;
    break;
  }

  if(result == CURLE_AGAIN)
    result = CURLE_OK;
  return result;
}

#endif /* CURL_ENABLE_MQTT */
