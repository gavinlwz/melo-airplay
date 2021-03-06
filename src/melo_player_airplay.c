/*
 * melo_player_airplay.c: Airplay Player using GStreamer
 *
 * Copyright (C) 2016 Alexandre Dilly <dillya@sparod.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <string.h>

#include <gst/gst.h>
#include "gsttcpraop.h"
#include "gstrtpraop.h"
#include "gstrtpraopdepay.h"

#include "ext/gstrtpjitterbuffer.h"

#include "melo_sink.h"
#include "melo_player_airplay.h"

#define MIN_LATENCY 100
#define DEFAULT_LATENCY 1000
#define DEFAULT_RTX_DELAY 500
#define DEFAULT_RTX_RETRY_PERIOD 100
#define DEFAULT_VOLUME 1.0

static gboolean melo_player_airplay_play (MeloPlayer *player, const gchar *path,
                                          const gchar *name, MeloTags *tags,
                                          gboolean insert);
static gboolean melo_player_airplay_set_mute (MeloPlayer *player,
                                              gboolean mute);

static gint melo_player_airplay_get_pos (MeloPlayer *player);

struct _MeloPlayerAirplayPrivate {
  GMutex mutex;
  guint32 start_rtptime;
  gdouble volume;

  /* Gstreamer pipeline */
  GstElement *pipeline;
  GstElement *raop_depay;
  GstElement *vol;
  MeloSink *sink;
  guint bus_watch_id;

  /* Gstreamer pipeline tunning */
  guint latency;
  gint rtx_delay;
  gint rtx_retry_period;
  gboolean disable_sync;

  /* Format */
  guint samplerate;
  guint channel_count;
};

G_DEFINE_TYPE_WITH_PRIVATE (MeloPlayerAirplay, melo_player_airplay, MELO_TYPE_PLAYER)

static void
melo_player_airplay_finalize (GObject *gobject)
{
  MeloPlayerAirplay *pair = MELO_PLAYER_AIRPLAY (gobject);
  MeloPlayerAirplayPrivate *priv =
                                melo_player_airplay_get_instance_private (pair);

  /* Stop pipeline */
  melo_player_airplay_teardown (pair);

  /* Clear mutex */
  g_mutex_clear (&priv->mutex);

  /* Chain up to the parent class */
  G_OBJECT_CLASS (melo_player_airplay_parent_class)->finalize (gobject);
}

static void
melo_player_airplay_class_init (MeloPlayerAirplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MeloPlayerClass *pclass = MELO_PLAYER_CLASS (klass);

  /* Register TCP RAOP depayloader */
  gst_tcp_raop_plugin_init (NULL);

  /* Register RTP RAOP depayloader */
  gst_rtp_raop_plugin_init (NULL);
  gst_rtp_raop_depay_plugin_init (NULL);

  /* Register internal RTP jitter buffer backported from Gstreamer 1.8.3  */
  gst_element_register (NULL, "rtpjitterbuffer_bp", GST_RANK_NONE,
      GST_TYPE_RTP_JITTER_BUFFER);

  /* Control */
  pclass->play = melo_player_airplay_play;
  pclass->set_mute = melo_player_airplay_set_mute;

  /* Status */
  pclass->get_pos = melo_player_airplay_get_pos;

  /* Add custom finalize() function */
  object_class->finalize = melo_player_airplay_finalize;
}

static void
melo_player_airplay_init (MeloPlayerAirplay *self)
{
  MeloPlayerAirplayPrivate *priv =
                                melo_player_airplay_get_instance_private (self);

  self->priv = priv;
  priv->latency = DEFAULT_LATENCY;
  priv->rtx_delay = DEFAULT_RTX_DELAY;
  priv->rtx_retry_period = DEFAULT_RTX_RETRY_PERIOD;
  priv->volume = DEFAULT_VOLUME;

  /* Init player mutex */
  g_mutex_init (&priv->mutex);
}

static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer data)
{
  MeloPlayerAirplay *pair = MELO_PLAYER_AIRPLAY (data);
  MeloPlayerAirplayPrivate *priv = pair->priv;
  MeloPlayer *player = MELO_PLAYER (pair);
  GError *error;

  /* Process bus message */
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      /* Stop playing */
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      melo_player_set_status_state (player, MELO_PLAYER_STATE_STOPPED);
      break;
    case GST_MESSAGE_ERROR:
      /* Update error message */
      gst_message_parse_error (msg, &error, NULL);
      melo_player_set_status_error (player, error->message);
      g_error_free (error);
      break;
    default:
      ;
  }

  return TRUE;
}

static gboolean
melo_player_airplay_play (MeloPlayer *player, const gchar *path,
                          const gchar *name, MeloTags *tags, gboolean insert)
{
  /* Update tags */
  melo_player_take_status_tags (player, tags);

  return TRUE;
}

static gboolean
melo_player_airplay_set_mute (MeloPlayer *player, gboolean mute)
{
  MeloPlayerAirplayPrivate *priv = (MELO_PLAYER_AIRPLAY (player))->priv;

  /* Lock player mutex */
  g_mutex_lock (&priv->mutex);

  /* Mute pipeline */
  if (priv->vol)
    g_object_set (priv->vol, "mute", mute, NULL);

  /* Unlock player mutex */
  g_mutex_unlock (&priv->mutex);

  return mute;
}

static gint
melo_player_airplay_get_pos (MeloPlayer *player)
{
  MeloPlayerAirplayPrivate *priv = (MELO_PLAYER_AIRPLAY (player))->priv;
  guint32 pos = 0;

  /* Lock player mutex */
  g_mutex_lock (&priv->mutex);

  /* Get RTP time */
  if (gst_rtp_raop_depay_query_rtptime (GST_RTP_RAOP_DEPAY (priv->raop_depay),
                                        &pos)) {
    pos = ((pos - priv->start_rtptime) * G_GUINT64_CONSTANT (1000)) /
          priv->samplerate;
  }

  /* Unlock player mutex */
  g_mutex_unlock (&priv->mutex);

  return pos;
}

static gboolean
melo_player_airplay_parse_format (MeloPlayerAirplayPrivate *priv,
                                  MeloAirplayCodec codec, const gchar *format,
                                  const gchar **encoding)
{
  gboolean ret = TRUE;
  guint tmp;

  switch (codec) {
    case MELO_AIRPLAY_CODEC_ALAC:
      /* Set encoding */
      *encoding = "ALAC";

      /* Get ALAC parameters:
       *  - Payload type
       *  - Max samples per frame (4 bytes)
       *  - Compatible version (1 byte)
       *  - Sample size (1 bytes)
       *  - History mult (1 byte)
       *  - Initial history (1 byte)
       *  - Rice param limit (1 byte)
       *  - Channel count (1 byte)
       *  - Max run (2 bytes)
       *  - Max coded frame size (4 bytes)
       *  - Average bitrate (4 bytes)
       *  - Sample rate (4 bytes)
       */
      if (sscanf (format, "%*d %*d %*d %*d %*d %*d %*d %d %*d %*d %*d %d",
                  &priv->channel_count, &priv->samplerate) != 2)
        ret = FALSE;

      break;
    case MELO_AIRPLAY_CODEC_PCM:
      /* Set encoding */
      *encoding = "L16";

      /* Get samplerate and channel count */
      if (sscanf (format, "%*d L%*d/%d/%d", &priv->samplerate,
                  &priv->channel_count) != 2)
        ret = FALSE;

      break;
    case MELO_AIRPLAY_CODEC_AAC:
      /* Set encoding */
      *encoding = "AAC";
    default:
      priv->samplerate = 44100;
      priv->channel_count = 2;
  }

  /* Set default values if not found */
  if (!priv->samplerate)
    priv->samplerate = 44100;
  if (!priv->channel_count)
      priv->channel_count = 2;

  return ret;
}

gboolean
melo_player_airplay_setup (MeloPlayerAirplay *pair,
                           MeloAirplayTransport transport,
                           const gchar *client_ip, guint *port,
                           guint *control_port, guint *timing_port,
                           MeloAirplayCodec codec, const gchar *format,
                           const guchar *key, gsize key_len,
                           const guchar *iv, gsize iv_len)
{
  MeloPlayerAirplayPrivate *priv = pair->priv;
  MeloPlayer *player = MELO_PLAYER (pair);
  guint max_port = *port + 100;
  GstElement *src, *vol, *sink;
  GstState next_state = GST_STATE_READY;
  const gchar *id, *name;
  const gchar *encoding;
  gchar *ename;
  GstBus *bus;

  /* Lock player mutex */
  g_mutex_lock (&priv->mutex);

  if (priv->pipeline)
    goto failed;

  /* Parse format */
  if (!melo_player_airplay_parse_format (priv, codec, format, &encoding))
    goto failed;

  /* Get ID from player */
  id = melo_player_get_id (player);
  name = melo_player_get_name (player);

  /* Create pipeline */
  ename = g_strjoin ("_", id, "pipeline", NULL);
  priv->pipeline = gst_pipeline_new (ename);
  g_free (ename);

  /* Create melo audio sink */
  ename = g_strjoin ("_", id, "sink", NULL);
  priv->sink = melo_sink_new (player, ename, name);
  g_free (ename);

  /* Create source */
  if (transport == MELO_AIRPLAY_TRANSPORT_UDP) {
    GstElement *src_caps, *raop, *rtp, *rtp_caps, *depay, *dec;
    GstCaps *caps;

    /* Add an UDP source and a RTP jitter buffer to pipeline */
    src = gst_element_factory_make ("udpsrc", NULL);
    src_caps = gst_element_factory_make ("capsfilter", NULL);
    raop = gst_element_factory_make ("rtpraop", NULL);
    rtp = gst_element_factory_make ("rtpjitterbuffer_bp", NULL);
    rtp_caps = gst_element_factory_make ("capsfilter", NULL);
    depay = gst_element_factory_make ("rtpraopdepay", NULL);
    if (codec == MELO_AIRPLAY_CODEC_AAC)
      dec = gst_element_factory_make ("avdec_aac", NULL);
    else
      dec = gst_element_factory_make ("avdec_alac", NULL);
    vol = gst_element_factory_make ("volume", NULL);
    sink = melo_sink_get_gst_sink (priv->sink);
    gst_bin_add_many (GST_BIN (priv->pipeline), src, src_caps, raop, rtp,
                      rtp_caps, depay, dec, vol, sink, NULL);

    /* Save RAOP depay element */
    priv->raop_depay = depay;

    /* Set caps for UDP source -> RTP jitter buffer link */
    caps = gst_caps_new_simple ("application/x-rtp",
                                "payload", G_TYPE_INT, 96,
                                "clock-rate", G_TYPE_INT, priv->samplerate,
                                NULL);
    g_object_set (G_OBJECT (src_caps), "caps", caps, NULL);
    gst_caps_unref (caps);

    /* Set caps for RTP jitter -> RTP RAOP depayloader link */
    caps = gst_caps_new_simple ("application/x-rtp",
                                "payload", G_TYPE_INT, 96,
                                "clock-rate", G_TYPE_INT, priv->samplerate,
                                "encoding-name", G_TYPE_STRING, encoding,
                                "config", G_TYPE_STRING, format,
                                NULL);
    g_object_set (G_OBJECT (rtp_caps), "caps", caps, NULL);
    gst_caps_unref (caps);

    /* Set keys into RTP RAOP depayloader */
    if (key)
      gst_rtp_raop_depay_set_key (GST_RTP_RAOP_DEPAY (depay), key, key_len,
                                  iv, iv_len);

    /* Force UDP source to use a new port */
    g_object_set (src, "reuse", FALSE, NULL);

    /* Disable synchronization on sink */
    if (priv->disable_sync)
      melo_sink_set_sync (priv->sink, FALSE);

    /* Set latency in jitter buffer */
    if (priv->latency)
      g_object_set (G_OBJECT (rtp), "latency", priv->latency, NULL);

    /* Link all elements */
    gst_element_link_many (src, src_caps, raop, rtp, rtp_caps, depay, dec, vol,
                           sink, NULL);

    /* Add sync / retransmit support to pipeline */
    if (*control_port) {
      GstElement *ctrl_src, *ctrl_sink;
      GstPad *raop_pad, *udp_pad;
      guint ctrl_port = *control_port;
      guint max_control_port = *control_port + 100;
      GSocket *sock;

      /* Enable retransmit events */
      g_object_set (G_OBJECT (rtp), "do-retransmission", TRUE, NULL);

      /* Set RTX delay */
      if (priv->rtx_delay > 0)
        g_object_set (G_OBJECT (rtp), "rtx-delay", priv->rtx_delay, NULL);

      /* Set RTX retry period */
      if (priv->rtx_retry_period > 0)
        g_object_set (G_OBJECT (rtp), "rtx-retry-period",
                      priv->rtx_retry_period, NULL);

      /* Send only one retransmit event */
      g_object_set (G_OBJECT (rtp), "rtx-max-retries", 0, NULL);

      /* Create and add control UDP source and sink */
      ctrl_src = gst_element_factory_make ("udpsrc", NULL);
      ctrl_sink = gst_element_factory_make ("udpsink", NULL);
      gst_bin_add_many (GST_BIN (priv->pipeline), ctrl_src, ctrl_sink, NULL);

      /* Set control port */
      g_object_set (ctrl_src, "port", *control_port, "reuse", FALSE, NULL);
      while (gst_element_set_state (ctrl_src, GST_STATE_READY) ==
                                                     GST_STATE_CHANGE_FAILURE) {
        /* Retry until a free port is available */
        *control_port += 2;
        if (*control_port > max_control_port)
          goto failed;

        /* Update UDP source port */
        g_object_set (ctrl_src, "port", *control_port, NULL);
      }

      /* Connect UDP source to ROAP control sink */
      udp_pad = gst_element_get_static_pad (ctrl_src, "src");
      raop_pad = gst_element_get_request_pad (raop, "sink_ctrl");
      gst_pad_link (udp_pad, raop_pad);
      gst_object_unref (raop_pad);
      gst_object_unref (udp_pad);

      /* Use socket from UDP source on UDP sink in order to get retransmit
       * replies on UDP source.
       */
      g_object_get (ctrl_src, "used-socket", &sock, NULL);
      g_object_set (ctrl_sink, "socket", sock, NULL);
      g_object_set (ctrl_sink, "port", ctrl_port, "host", client_ip, NULL);

      /* Disable async state and synchronization since we only send retransmit
       * requests on this UDP sink, so no need for synchronization..
       */
      g_object_set (ctrl_sink, "async", FALSE, "sync", FALSE, NULL);

      /* Connect RAOP control source to UDP sink */
      raop_pad = gst_element_get_request_pad (raop, "src_ctrl");
      udp_pad = gst_element_get_static_pad (ctrl_sink, "sink");
      gst_pad_link (raop_pad, udp_pad);
      gst_object_unref (raop_pad);
      gst_object_unref (udp_pad);
    }
  } else {
    GstElement *rtp_caps, *raop, *depay, *dec;
    GstCaps *caps;

    /* Create pipeline for TCP streaming */
    src = gst_element_factory_make ("tcpserversrc", NULL);
    rtp_caps = gst_element_factory_make ("capsfilter", NULL);
    raop = gst_element_factory_make ("tcpraop", NULL);
    depay = gst_element_factory_make ("rtpraopdepay", NULL);
    dec = gst_element_factory_make ("avdec_alac", NULL);
    vol = gst_element_factory_make ("volume", NULL);
    sink = melo_sink_get_gst_sink (priv->sink);
    gst_bin_add_many (GST_BIN (priv->pipeline), src, rtp_caps, raop, depay, dec,
                      vol, sink, NULL);

    /* Save RAOP depay element */
    priv->raop_depay = depay;

    /* Set caps for TCP source -> TCP RAOP depayloader link */
    caps = gst_caps_new_simple ("application/x-rtp-stream",
                                "clock-rate", G_TYPE_INT, priv->samplerate,
                                "encoding-name", G_TYPE_STRING, "ALAC",
                                "config", G_TYPE_STRING, format,
                                NULL);
    g_object_set (G_OBJECT (rtp_caps), "caps", caps, NULL);
    gst_caps_unref (caps);

    /* Set keys into TCP RAOP decryptor */
    if (key)
      gst_rtp_raop_depay_set_key (GST_RTP_RAOP_DEPAY (depay), key, key_len,
                                  iv, iv_len);

    /* Listen on all interfaces */
    g_object_set (src, "host", "0.0.0.0", NULL);

    /* To start listening, state muste be set to playing */
    next_state = GST_STATE_PLAYING;

    /* Link all elements */
    gst_element_link_many (src, rtp_caps, raop, depay, dec, vol, sink, NULL);
  }

  /* Save elements */
  priv->vol = vol;

  /* Set server port */
  g_object_set (src, "port", *port, NULL);

  /* Add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  priv->bus_watch_id = gst_bus_add_watch (bus, bus_call, pair);
  gst_object_unref (bus);

  /* Start the pipeline */
  while (gst_element_set_state (src, next_state) == GST_STATE_CHANGE_FAILURE) {
    /* Incremnent port until we found a free port */
    *port += 2;
    if (*port > max_port)
      goto failed;

    /* Update port */
    g_object_set (src, "port", *port, NULL);
  }

  /* Unlock player mutex */
  g_mutex_unlock (&priv->mutex);

  return TRUE;

failed:
  g_mutex_unlock (&priv->mutex);
  return FALSE;
}

gboolean
melo_player_airplay_record (MeloPlayerAirplay *pair, guint seq)
{
  MeloPlayerAirplayPrivate *priv = pair->priv;
  MeloPlayer *player = MELO_PLAYER (pair);

  /* Lock player mutex */
  g_mutex_lock (&priv->mutex);

  /* Set playing */
  gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  melo_player_set_status_state (player, MELO_PLAYER_STATE_PLAYING);

  /* Unlock player mutex */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}

gboolean
melo_player_airplay_flush (MeloPlayerAirplay *pair, guint seq)
{
  MeloPlayer *player = MELO_PLAYER (pair);

  /* Set paused */
  melo_player_set_status_state (player, MELO_PLAYER_STATE_PAUSED);

  return TRUE;
}

gboolean
melo_player_airplay_teardown (MeloPlayerAirplay *pair)
{
  MeloPlayerAirplayPrivate *priv = pair->priv;
  MeloPlayer *player = MELO_PLAYER (pair);

  /* Lock player mutex */
  g_mutex_lock (&priv->mutex);

  /* Already stoped */
  if (!priv->pipeline) {
    g_mutex_unlock (&priv->mutex);
    return FALSE;
  }

  /* Stop pipeline */
  gst_element_set_state (priv->pipeline, GST_STATE_NULL);
  melo_player_set_status_state (player, MELO_PLAYER_STATE_NONE);

  /* Remove message handler */
  g_source_remove (priv->bus_watch_id);

  /* Free gstreamer pipeline */
  g_object_unref (priv->pipeline);

  /* Free melo audio sink */
  g_object_unref (priv->sink);

  /* Unlock player mutex */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}

gboolean
melo_player_airplay_set_volume (MeloPlayerAirplay *pair, gdouble volume)
{
  MeloPlayerAirplayPrivate *priv = pair->priv;
  MeloPlayer *player = MELO_PLAYER (pair);

  /* Set volume */
  if (volume > -144.0)
    priv->volume = (volume + 30.0) / 30.0;
  else
    priv->volume = 0.0;

  /* Lock player mutex */
  g_mutex_lock (&priv->mutex);

  /* Update volume in pipeline */
  if (priv->vol)
    g_object_set (G_OBJECT (priv->vol), "volume", priv->volume, NULL);

  /* Unlock player mutex */
  g_mutex_unlock (&priv->mutex);

  /* Update status volume */
  melo_player_set_status_volume (player, priv->volume);

  return TRUE;
}

gdouble
melo_player_airplay_get_volume (MeloPlayerAirplay *pair)
{
  if (pair->priv->volume == 0.0)
    return -144.0;
  return (pair->priv->volume - 1.0) * 30.0;
}

gboolean
melo_player_airplay_set_progress (MeloPlayerAirplay *pair, guint start,
                                  guint cur, guint end)
{
  MeloPlayerAirplayPrivate *priv = pair->priv;
  MeloPlayer *player = MELO_PLAYER (pair);

  /* Set progression */
  priv->start_rtptime = start;
  melo_player_set_status_state (player, MELO_PLAYER_STATE_PLAYING);
  melo_player_set_status_pos (player,
                              (cur - start) * G_GUINT64_CONSTANT (1000) /
                              priv->samplerate);
  melo_player_set_status_duration (player,
                                   (end - start) * G_GUINT64_CONSTANT (1000) /
                                   priv->samplerate);

  return TRUE;
}

gboolean
melo_player_airplay_set_cover (MeloPlayerAirplay *pair, GBytes *cover,
                               const gchar *cover_type)
{
  MeloPlayer *player = MELO_PLAYER (pair);
  MeloTags *tags;

  /* Get tags */
  tags = melo_player_get_tags (player);

  /* Set cover */
  if (tags) {
    melo_tags_take_cover (tags, cover, cover_type);
    melo_tags_set_cover_url (tags, G_OBJECT (pair), NULL, NULL);
    melo_player_take_status_tags (player, tags);
  }

  return TRUE;
}

void
melo_player_airplay_set_latency (MeloPlayerAirplay *pair, guint latency)
{
  pair->priv->latency = latency;
}

void
melo_player_airplay_set_rtx (MeloPlayerAirplay *pair, gint rtx_delay,
                             gint rtx_retry_period)
{
  pair->priv->rtx_delay = rtx_delay;
  pair->priv->rtx_retry_period = rtx_retry_period;
}

void
melo_player_airplay_disable_sync (MeloPlayerAirplay *pair, gboolean sync)
{
  pair->priv->disable_sync = sync;
}
