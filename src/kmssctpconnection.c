/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#include <kurento/gstmarshal/kmsfragmenter.h>
#include <netinet/sctp.h>
#include <string.h>
#include <gio/gio.h>

#include "kmssctpconnection.h"
#include "gstsctp.h"

#define KMS_SCTP_CONNECTION_ERROR \
  g_quark_from_static_string("kms-sctp-connection-error-quark")

typedef enum
{
  KMS_CONNECTION_READ_ERROR
} KmsSCTPConnectionError;

GST_DEBUG_CATEGORY_STATIC (kms_sctp_connection_debug);
#define GST_CAT_DEFAULT kms_sctp_connection_debug

GType _kms_sctp_connection_type = 0;

struct _KmsSCTPConnection
{
  GstMiniObject obj;

  GSocket *socket;
  GSocketAddress *saddr;
};

GST_DEFINE_MINI_OBJECT_TYPE (KmsSCTPConnection, kms_sctp_connection);

static void
_priv_kms_sctp_connection_initialize (void)
{
  _kms_sctp_connection_type = kms_sctp_connection_get_type ();

  GST_DEBUG_CATEGORY_INIT (kms_sctp_connection_debug, "sctpconnection", 0,
      "sctp connection");
}

static void
_kms_sctp_connection_free (KmsSCTPConnection * conn)
{
  g_return_if_fail (conn != NULL);

  GST_DEBUG ("free");

  if (conn->socket != NULL)
    kms_sctp_connection_close (conn);

  g_clear_object (&conn->socket);
  g_clear_object (&conn->saddr);

  g_slice_free1 (sizeof (KmsSCTPConnection), conn);
}

static gboolean
kms_sctp_connection_create_socket (KmsSCTPConnection * conn, gchar * host,
    gint port, GCancellable * cancellable, GError ** err)
{
  GInetAddress *addr;

  /* look up name if we need to */
  addr = g_inet_address_new_from_string (host);
  if (addr == NULL) {
    GResolver *resolver;
    GList *results;

    resolver = g_resolver_get_default ();
    results = g_resolver_lookup_by_name (resolver, host, cancellable, err);

    if (results == NULL) {
      g_object_unref (resolver);
      return FALSE;
    }

    addr = G_INET_ADDRESS (g_object_ref (results->data));

    g_resolver_free_addresses (results);
    g_object_unref (resolver);
  }

  if (G_UNLIKELY (GST_LEVEL_DEBUG <= _gst_debug_min)) {
    gchar *ip = g_inet_address_to_string (addr);

    GST_DEBUG ("IP address for host %s is %s", host, ip);
    g_free (ip);
  }

  conn->saddr = g_inet_socket_address_new (addr, port);
  g_object_unref (addr);

  /* create sending client socket */
  GST_DEBUG ("opening sending client socket to %s:%d", host, port);

  conn->socket = g_socket_new (g_socket_address_get_family (conn->saddr),
      G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_SCTP, err);

  if (conn->socket == NULL) {
    g_clear_object (&conn->saddr);
    return FALSE;
  }

  return TRUE;
}

KmsSCTPConnection *
kms_sctp_connection_new (gchar * host, gint port, GCancellable * cancellable,
    GError ** err)
{
  KmsSCTPConnection *conn;

  conn = g_slice_new0 (KmsSCTPConnection);

  gst_mini_object_init (GST_MINI_OBJECT_CAST (conn), 0,
      _kms_sctp_connection_type, NULL, NULL,
      (GstMiniObjectFreeFunction) _kms_sctp_connection_free);

  if (!kms_sctp_connection_create_socket (conn, host, port, cancellable, err)) {
    kms_sctp_connection_unref (conn);
    return NULL;
  }

  return GST_SCTP_CONNECTION (conn);
}

void
kms_sctp_connection_close (KmsSCTPConnection * conn)
{
  GError *err = NULL;

  g_return_if_fail (conn != NULL);

  if (conn->socket == NULL)
    return;

  if (g_socket_is_closed (conn->socket)) {
    GST_DEBUG ("Socket is already closed");
    return;
  }

  if (!g_socket_shutdown (conn->socket, TRUE, TRUE, &err)) {
    GST_ERROR ("Could noty shutdown socket %p: %s", conn->socket, err->message);
    g_clear_error (&err);
  }

  GST_DEBUG ("Closing socket");

  if (!g_socket_close (conn->socket, &err)) {
    GST_ERROR ("Failed to close socket %p: %s", conn->socket, err->message);
    g_clear_error (&err);
  }
}

KmsSCTPResult
kms_sctp_connection_connect (KmsSCTPConnection * conn,
    GCancellable * cancellable, GError ** err)
{
  g_return_val_if_fail (conn != NULL, KMS_SCTP_ERROR);
  g_return_val_if_fail (conn->socket != NULL, KMS_SCTP_ERROR);
  g_return_val_if_fail (conn->saddr != NULL, KMS_SCTP_ERROR);

  if (g_socket_is_connected (conn->socket))
    return KMS_SCTP_OK;

  /* connect to server */
  if (!g_socket_connect (conn->socket, conn->saddr, cancellable, err))
    return KMS_SCTP_ERROR;

  if (G_UNLIKELY (GST_LEVEL_DEBUG <= _gst_debug_min)) {
#if defined (SCTP_INITMSG)
    struct sctp_initmsg initmsg;
    socklen_t optlen;

    if (getsockopt (g_socket_get_fd (conn->socket), IPPROTO_SCTP,
            SCTP_INITMSG, &initmsg, &optlen) < 0)
      GST_WARNING ("Could not get SCTP configuration: %s (%d)",
          g_strerror (errno), errno);
    else
      GST_DEBUG ("SCTP client socket: ostreams %u, instreams %u",
          initmsg.sinit_num_ostreams, initmsg.sinit_num_ostreams);
#else
    GST_WARNING ("don't know how to get the configuration of the "
        "SCTP initiation structure on this OS.");
#endif
  }

  GST_DEBUG ("Created sctp socket");

  return KMS_SCTP_OK;
}

KmsSCTPResult
kms_sctp_connection_receive (KmsSCTPConnection * conn, KmsSCTPMessage * message,
    GCancellable * cancellable, GError ** err)
{
  GIOCondition condition;
  guint streamid;

  g_return_val_if_fail (conn != NULL, KMS_SCTP_ERROR);
  g_return_val_if_fail (conn->socket != NULL, KMS_SCTP_ERROR);

  if (!g_socket_condition_wait (conn->socket,
          G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP, cancellable, err)) {
    return KMS_SCTP_ERROR;
  }

  condition = g_socket_condition_check (conn->socket,
      G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP);

  if ((condition & G_IO_ERR)) {
    g_set_error (err, KMS_SCTP_CONNECTION_ERROR, KMS_CONNECTION_READ_ERROR,
        "Socket in error state");
    return KMS_SCTP_ERROR;
  } else if ((condition & G_IO_HUP)) {
    g_set_error (err, KMS_SCTP_CONNECTION_ERROR, KMS_CONNECTION_READ_ERROR,
        "Connection closed");
    return KMS_SCTP_EOF;
  }

  message->used = sctp_socket_receive (conn->socket, message->buf,
      message->size, cancellable, &streamid, err);

  if (message->used == 0)
    return KMS_SCTP_EOF;
  else if (message->used < 0)
    return KMS_SCTP_ERROR;
  else
    return KMS_SCTP_OK;
}

gboolean
kms_sctp_connection_set_init_config (KmsSCTPConnection * conn,
    guint16 num_ostreams, guint16 max_instreams, guint16 max_attempts,
    guint16 max_init_timeo)
{
  g_return_val_if_fail (conn != NULL, FALSE);

#if defined (SCTP_INITMSG)
  {
    struct sctp_initmsg initmsg;

    memset (&initmsg, 0, sizeof (initmsg));
    initmsg.sinit_num_ostreams = num_ostreams;
    initmsg.sinit_max_instreams = max_instreams;
    initmsg.sinit_max_attempts = max_attempts;
    initmsg.sinit_max_init_timeo = max_init_timeo;

    if (setsockopt (g_socket_get_fd (conn->socket), IPPROTO_SCTP,
            SCTP_INITMSG, &initmsg, sizeof (initmsg)) < 0) {
      GST_ERROR ("Could not configure SCTP socket: %s (%d)",
          g_strerror (errno), errno);
      return FALSE;
    }

    return TRUE;
  }
#else
  {
    GST_WARNING ("don't know how to configure the SCTP initiation "
        "parameters on this OS.");

    return FALSE;
  }
#endif
}

static void _priv_kms_sctp_connection_initialize (void)
    __attribute__ ((constructor));