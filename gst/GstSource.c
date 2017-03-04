/* 
 * Copyright (C) 2015 Vikram Fugro
 *
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/base/gstadapter.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "Upnp.h"
#include "GstSource.h"

struct GstSource {
  GMutex dlock;
  GstBin *bin;
  GstAdapter *adapter;
  int bufferCount;
  gchar *device;
  gchar *url;
  GstClockTime lTime;
};

static void
frame_handoff_cb (GstElement *ele, GstBuffer *buf,
        GstPad *pad, gpointer data)
{
  GstSource* dev = data;

  g_mutex_lock (&dev->dlock);
  gst_adapter_push (dev->adapter, gst_buffer_ref(buf));
  g_mutex_unlock (&dev->dlock);

  if (G_UNLIKELY (dev->bufferCount == 0)) {
	  g_print ("SeEnding PLAY to the DMR\n");
    up_play (dev->device, dev->url);
  }
  dev->bufferCount++;
}

static GstPadProbeReturn
probe_cb (GstPad* pad, GstPadProbeInfo* info, gpointer data)
{
  GstSource* dev = data;
  GstBuffer* buf = GST_BUFFER_CAST (info->data);

  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (dev->lTime))) {
    dev->lTime = GST_BUFFER_DTS (buf);
  } else {
    dev->lTime += (GstClockTime)(GST_SECOND / 30);
    GST_BUFFER_DTS (buf) = dev->lTime;
    GST_BUFFER_PTS (buf) = GST_BUFFER_DTS (buf); 
  }

  GST_BUFFER_DURATION(buf) = (GstClockTime)(GST_SECOND / 30); 
  return GST_PAD_PROBE_OK;
}

static void
pad_added_cb (GstElement* element, GstPad* pad, GstElement* ele)
{
  gchar* name = gst_pad_get_name (pad);

  g_print (">>>>> pad added by %s : %s\n", GST_ELEMENT_NAME (element), name);

  if (!strncmp (name, "recv_rtp_src_", 13) || !strncmp (name, "src_", 4)) {
    GstPad* spad = gst_element_get_static_pad (ele, "sink"); 
    g_print (">>>>>> linking rtpbin pad to dbin /dbin to vconv %s\n", gst_pad_link_get_name(gst_pad_link(pad, spad)));
    gst_object_unref (spad);
  } 

  g_free(name);
}

int
getData (GstSource *p, char* fTo, int fMaxSize)
{
  int avail = 0, ret = 0, try = 0;
  GstSource* dev = p;

  g_mutex_lock (&dev->dlock);

  do {
    avail = (int)gst_adapter_available (dev->adapter);
    if (avail == 0) {
      g_mutex_unlock (&dev->dlock);
      g_usleep (50000);
      g_mutex_lock (&dev->dlock);
    }
  } while (avail == 0 && try++ < 10);

  if (avail) {
    if (avail > fMaxSize) {
      avail = fMaxSize;
    }
    gst_adapter_copy (dev->adapter, fTo, 0, avail);
    gst_adapter_flush(dev->adapter, avail);
    ret = avail; 
  }

  g_mutex_unlock (&dev->dlock);

  return ret;
}

GstSource*
startPipeline  (int port, char *device, char *type, char *url, int *ret)
{
  GstPad* srcpad, *sinkpad;
  GstElement* vsrc, *vque, *vdec=NULL, *idv;
  GstBin *bin;
  GstSource *dev;

  gst_init (NULL, NULL);

  dev = calloc (1, sizeof (GstSource));
  dev->bin = bin = (GstBin*)gst_pipeline_new (NULL);
  g_mutex_init (&dev->dlock);
  dev->adapter = gst_adapter_new();
  dev->bufferCount = 0;
  dev->device = g_strdup (device);
  dev->url = g_strdup (url);
  dev->lTime = GST_CLOCK_TIME_NONE;

  GstElement* vconv = gst_element_factory_make ("videoconvert", NULL);
  GstElement* venc = gst_element_factory_make ("x264enc", NULL);
  GstElement* vid = gst_element_factory_make ("identity", NULL);
  GstElement* vmux = gst_element_factory_make ("qtmux", NULL);
  GstElement* fsink = gst_element_factory_make ("fakesink", NULL);

  if (!strcmp(type, "camera")) {  
    vsrc = gst_element_factory_make ("v4l2src", NULL);
    vque = gst_element_factory_make ("queue", NULL);
    g_object_set (G_OBJECT(vsrc), "io-mode", 2, NULL);
    g_object_set (G_OBJECT(vque), "max-size-time", (guint64)0, "max-size-bytes", 0, "max-size-buffers", 16, NULL);
  } else if (!strcmp(type, "streaming")) {
    vsrc = gst_element_factory_make ("udpsrc", NULL);
    idv = gst_element_factory_make ("capsfilter", NULL); 
    vque = gst_element_factory_make ("rtpbin", NULL); 
    vdec = gst_element_factory_make ("decodebin", NULL);
  }
 
  gst_bin_add_many (GST_BIN(bin), vsrc, vque, vconv, venc, vid, vmux, fsink, NULL);
  if (vdec) {
    gst_bin_add_many (GST_BIN (bin), idv, vdec, NULL);
  } 
  
  g_object_set (G_OBJECT(venc), "threads", 1, "cabac", FALSE, "tune", 4, NULL);
  g_object_set (G_OBJECT(vmux), "streamable", TRUE, "fragment-duration", 100, NULL);
  g_object_set( G_OBJECT( fsink ), "sync", FALSE,
      "enable-last-sample", FALSE, "signal-handoffs", TRUE, NULL );
  g_signal_connect( G_OBJECT( fsink ), "handoff",
      frame_handoff_cb, dev);

  if (!strcmp(type, "camera")) {
    gst_element_link_many (vsrc, vque, vconv, NULL);
  } else if (!strcmp(type, "streaming")) {
    GstCaps* caps = gst_caps_new_simple ("application/x-rtp",
      "payload", G_TYPE_INT, 96,
      "encoding-name", G_TYPE_STRING, "H264",
      "clock-rate", G_TYPE_INT, 90000,
      "media", G_TYPE_STRING, "video",
      NULL);
    g_object_set (G_OBJECT (idv), "caps", caps, NULL);
    gst_element_link (vsrc, idv);
    gst_caps_unref (caps);
    GstPad* srcpad = gst_element_get_static_pad (idv, "src");  
    GstPad* sinkpad = gst_element_get_request_pad (vque, "recv_rtp_sink_%u");
    g_print (">>>>>> linking rtpbin udp pad %s\n", gst_pad_link_get_name(gst_pad_link(srcpad, sinkpad)));
    gst_object_unref (srcpad);
    gst_object_unref (sinkpad);
    g_object_set (G_OBJECT(vque), "latency", 2000, NULL);
    g_object_set (G_OBJECT(vsrc), "port", port, NULL); 
    g_signal_connect (G_OBJECT (vque), "pad-added",
      G_CALLBACK (pad_added_cb), vdec);
    g_signal_connect (G_OBJECT (vdec), "pad-added",
      G_CALLBACK (pad_added_cb), vconv);
  }

  gst_element_link(vconv, venc);
  GstCaps* caps = gst_caps_new_simple ("video/x-h264",
      "width", G_TYPE_INT, 320,
      "height", G_TYPE_INT, 240,
      "profile", G_TYPE_STRING, "constrained-baseline", 
      NULL);
  gst_element_link_filtered(venc, vid, caps);
  gst_caps_unref (caps); 
  srcpad = gst_element_get_static_pad (vid, "src");
  sinkpad = gst_element_get_request_pad (vmux, "video_%u");
  g_print (">>>>>>>>>>>>>>>>>>>----->%s\n", gst_pad_link_get_name (gst_pad_link (srcpad, sinkpad)));
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);
  gst_element_link (vmux, fsink);

  srcpad = gst_element_get_static_pad (venc, "src");
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_BUFFER, probe_cb, dev, NULL);
  gst_object_unref (srcpad); 

  if (gst_element_set_state (GST_ELEMENT(bin), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    printf ("error in pipeline setup");
    *ret = -1;
  } else {
    printf ("done with pipeline setup\n");
  }

  return dev;
}

void 
destroyPipeline (GstSource* p)
{
  GstSource *dev;

  if (!p)
    return;

  dev = p;
  if (dev->bin)
    gst_element_set_state (GST_ELEMENT(dev->bin), GST_STATE_NULL);
  if (dev->adapter)
    gst_adapter_clear (dev->adapter);
  if (dev->bin)
    gst_object_unref (dev->bin);
  g_mutex_clear (&dev->dlock);
  if (dev->url)
    g_free (dev->url);
  if (dev->device) {
    up_stop (dev->device);
    g_free (dev->device);
  }
  free (dev);
}
