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
#include <gio/gio.h>

#include "kmsrpcclient.h"
#include "kmssctpconnection.h"

GST_DEBUG_CATEGORY_STATIC (kms_rpc_client_debug);
#define GST_CAT_DEFAULT kms_rpc_client_debug

#define SCTP_NUM_OSTREAMS 1
#define SCTP_MAX_INSTREAMS 1

#define KMS_RTP_CLIENT_CANCELLABLE \
  g_quark_from_static_string("kms-rtp-client-cancellable-quark")

#define KMS_RPC_CLIENT_LOCK(obj) (                  \
  g_rec_mutex_lock (&GST_RPC_CLIENT (obj)->mutex)   \
)

#define KMS_RPC_CLIENT_UNLOCK(obj) (                  \
  g_rec_mutex_unlock (&GST_RPC_CLIENT (obj)->mutex)   \
)

GType _kms_rpc_client_type = 0;

struct _KmsRPCClient
{
  GstMiniObject obj;

  KmsSCTPConnection *conn;

  guint reqid;
  gsize size;
  GHashTable *table;
  KurentoMarshalRules rules;
  GRecMutex mutex;

  GstTask *task;
  GRecMutex tmutex;

  KmsEOFFunction cb;
  gpointer cb_data;
  GDestroyNotify destroy;
};

GST_DEFINE_MINI_OBJECT_TYPE (KmsRPCClient, kms_rpc_client);

static void
_priv_kms_rpc_client_initialize (void)
{
  _kms_rpc_client_type = kms_rpc_client_get_type ();

  GST_DEBUG_CATEGORY_INIT (kms_rpc_client_debug, "rpc_client", 0, "rpc client");
}

static void
_kms_rpc_client_free (KmsRPCClient * rpc)
{
  g_return_if_fail (rpc != NULL);

  GST_DEBUG ("free");

  if (rpc->cb_data != NULL && rpc->destroy != NULL)
    rpc->destroy (rpc->cb_data);

  if (rpc->conn != NULL)
    kms_sctp_connection_unref (rpc->conn);

  g_rec_mutex_clear (&rpc->mutex);
  g_rec_mutex_clear (&rpc->tmutex);

  g_hash_table_unref (rpc->table);

  g_slice_free1 (sizeof (KmsRPCClient), rpc);
}

void
destroy_hash_value (gpointer data)
{
  /* TODO: */
}

KmsRPCClient *
kms_rpc_client_new (KurentoMarshalRules rules, gsize max)
{
  KmsRPCClient *m;

  m = g_slice_new0 (KmsRPCClient);

  gst_mini_object_init (GST_MINI_OBJECT_CAST (m), 0,
      _kms_rpc_client_type, NULL, NULL,
      (GstMiniObjectFreeFunction) _kms_rpc_client_free);

  m->rules = rules;
  m->size = max;

  g_rec_mutex_init (&m->mutex);
  g_rec_mutex_init (&m->tmutex);

  m->table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      destroy_hash_value);

  return GST_RPC_CLIENT (m);
}

static void
kms_rpc_client_thread (KmsRPCClient * rpc)
{
  GCancellable *cancellable;
  GError *err = NULL;
  KmsSCTPConnection *conn;
  KmsSCTPResult result;
  KmsSCTPMessage msg;

  KMS_RPC_CLIENT_LOCK (rpc);

  if (rpc->conn == NULL) {
    KMS_RPC_CLIENT_UNLOCK (rpc);
    return;
  }

  cancellable = gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (rpc),
      KMS_RTP_CLIENT_CANCELLABLE);
  conn = kms_sctp_connection_ref (rpc->conn);

  KMS_RPC_CLIENT_UNLOCK (rpc);

  if (g_cancellable_is_cancelled (cancellable))
    goto pause;

  GST_DEBUG ("Buffer size %" G_GSIZE_FORMAT, rpc->size);

  INIT_SCTP_MESSAGE (msg, rpc->size);
  result = kms_sctp_connection_receive (rpc->conn, &msg, cancellable, &err);

  if (result != KMS_SCTP_OK)
    goto error;

  GST_DEBUG ("Got buffer!");

  CLEAR_SCTP_MESSAGE (msg);

  kms_sctp_connection_unref (conn);

  return;

error:
  {
    if (err != NULL) {
      GST_ERROR ("Error code (%u): %s", result, err->message);
      g_error_free (err);
    } else {
      GST_ERROR ("Failed reading from socket code (%u)", result);
    }

    kms_sctp_connection_unref (conn);

    if (result == KMS_SCTP_EOF) {
      KmsEOFFunction cb;
      gpointer cb_data;

      KMS_RPC_CLIENT_LOCK (rpc);
      cb = rpc->cb;
      cb_data = rpc->cb_data;
      KMS_RPC_CLIENT_UNLOCK (rpc);

      if (cb != NULL)
        cb (cb_data);
    }

  pause:
    /* pause task */
    KMS_RPC_CLIENT_LOCK (rpc);
    if (rpc->task != NULL)
      gst_task_pause (rpc->task);
    KMS_RPC_CLIENT_UNLOCK (rpc);
  }
}

gboolean
kms_rpc_client_start (KmsRPCClient * rpc, gchar * host, gint port,
    GCancellable * cancellable, GError ** err)
{
  KmsSCTPConnection *conn;
  KmsSCTPResult result;
  GstTask *task;

  if (rpc->conn != NULL)
    return TRUE;

  conn = kms_sctp_connection_new (host, port, cancellable, err);

  if (conn == NULL) {
    GST_ERROR ("Error creating SCTP socket");
    return FALSE;
  }

  if (!kms_sctp_connection_set_init_config (conn, SCTP_NUM_OSTREAMS,
          SCTP_MAX_INSTREAMS, 0, 0)) {
    kms_sctp_connection_unref (conn);
    return FALSE;
  }

  result = kms_sctp_connection_connect (conn, cancellable, err);
  if (result != KMS_SCTP_OK) {
    GST_ERROR ("Error connecting SCTP socket");
    kms_sctp_connection_unref (conn);
    return FALSE;
  }

  if (rpc->task == NULL) {
    task = gst_task_new ((GstTaskFunction) kms_rpc_client_thread, rpc, NULL);
    if (task == NULL)
      goto task_error;

    gst_task_set_lock (task, &rpc->tmutex);
  }

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (rpc),
      KMS_RTP_CLIENT_CANCELLABLE, cancellable, NULL);

  if (gst_task_start (task)) {
    rpc->conn = conn;
    rpc->task = task;
    return TRUE;
  }

  /* ERRORS */
task_error:
  {
    GST_ERROR ("failed to create task");

    kms_sctp_connection_close (conn);
    kms_sctp_connection_unref (conn);

    gst_object_unref (task);
    gst_mini_object_steal_qdata ((GstMiniObject *) rpc,
        KMS_RTP_CLIENT_CANCELLABLE);

    return FALSE;
  }
}

void
kms_rpc_client_stop (KmsRPCClient * rpc)
{
  KmsSCTPConnection *conn;
  GstTask *task;

  g_return_val_if_fail (rpc != NULL, FALSE);

  GST_DEBUG ("stopping");

  KMS_RPC_CLIENT_LOCK (rpc);

  conn = rpc->conn;
  rpc->conn = NULL;

  if ((task = rpc->task)) {
    rpc->task = NULL;

    KMS_RPC_CLIENT_UNLOCK (rpc);

    gst_task_stop (task);

    /* make sure it is not running */
    g_rec_mutex_lock (&rpc->tmutex);
    g_rec_mutex_unlock (&rpc->tmutex);

    /* now wait for the task to finish */
    gst_task_join (task);

    /* and free the task */
    gst_object_unref (GST_OBJECT (task));

  } else {
    KMS_RPC_CLIENT_UNLOCK (rpc);
  }

  if (conn != NULL) {
    kms_sctp_connection_close (conn);
    kms_sctp_connection_unref (conn);
  }
}

void
kms_rpc_client_set_eof_function_full (KmsRPCClient * rpc, KmsEOFFunction func,
    gpointer user_data, GDestroyNotify notify)
{
  GDestroyNotify destroy;
  gpointer data;

  g_return_if_fail (rpc != NULL);

  KMS_RPC_CLIENT_LOCK (rpc);

  data = rpc->cb_data;
  destroy = rpc->destroy;

  rpc->cb_data = user_data;
  rpc->destroy = notify;
  rpc->cb = func;

  KMS_RPC_CLIENT_UNLOCK (rpc);

  if (data != NULL && destroy != NULL)
    destroy (data);
}

GstQuery *
kms_rpc_client_query (KmsRPCClient * m, GstQuery * query, GError ** err)
{
  KmsFragmenter *f;

  f = kms_fragmenter_new (m->rules, m->size);

  KMS_RPC_CLIENT_LOCK (m);

  if (!kms_fragmenter_query (f, m->reqid++, query, err)) {
    kms_fragmenter_unref (f);
    m->reqid--;
    KMS_RPC_CLIENT_UNLOCK (m);
    return NULL;
  }

  KMS_RPC_CLIENT_UNLOCK (m);
  kms_fragmenter_unref (f);

  /* TODO: Send query */

  return NULL;
}

void
kms_rpc_client_event (KmsRPCClient * m, GstEvent * event, GError ** err)
{
  KmsFragmenter *f;

  f = kms_fragmenter_new (m->rules, m->size);

  KMS_RPC_CLIENT_LOCK (m);

  if (!kms_fragmenter_event (f, m->reqid++, event, err)) {
    kms_fragmenter_unref (f);
    m->reqid--;
    KMS_RPC_CLIENT_UNLOCK (m);
    return;
  }

  KMS_RPC_CLIENT_UNLOCK (m);
  kms_fragmenter_unref (f);

  /* TODO: Send event */
}

static void _priv_kms_rpc_client_initialize (void)
    __attribute__ ((constructor));