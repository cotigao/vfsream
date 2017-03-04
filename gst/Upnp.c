/* 
 * Copyright (C) 2017 Vikram Fugro
 *
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.
 */

#include <libgupnp/gupnp.h>
#include <libgupnp-av/gupnp-av.h>
#include <libgupnp/gupnp-control-point.h>
#include <gst/gst.h>
#include <libxml/tree.h>

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <errno.h>

#include "Upnp.h"

#define MEDIA_RENDERER "urn:schemas-upnp-org:device:MediaRenderer:1"
#define CONNECTION_MANAGER "urn:schemas-upnp-org:service:ConnectionManager"
#define AV_TRANSPORT "urn:schemas-upnp-org:service:AVTransport"
#define RENDERING_CONTROL "urn:schemas-upnp-org:service:RenderingControl"


typedef enum
{
  EV_STOP = 0,
  EV_SCAN,
  EV_PLAY
} PlaybackCmd;

typedef enum
{
  PLAYBACK_STATE_UNKNOWN,
  PLAYBACK_STATE_TRANSITIONING,
  PLAYBACK_STATE_STOPPED,
  PLAYBACK_STATE_PAUSED,
  PLAYBACK_STATE_PLAYING
} PlaybackState;


typedef struct
{
  gpointer data1;
  gpointer data2;
  PlaybackCmd type;
} cmd;

typedef struct
{
  void (*callback) (char*);
  GUPnPDIDLLiteResource *resource;
  gchar* target;
} SetAVTransportURIData;

typedef struct
{
  GUPnPDeviceProxy  *proxy;
  GUPnPServiceProxy *av_transport;
  GUPnPServiceProxy *rendering_control;
  gchar* sink_protocol_info;
  PlaybackState state;
  gchar* name;
} dmr;

static GstAtomicQueue* aqueue;
static GRecMutex evlock;
static GList* dmrList = NULL;
static GUPnPControlPoint *dmr_cp = NULL;
static GUPnPContextManager *context_manager;

static PlaybackState
state_name_to_state (const char *state_name)
{
  PlaybackState state;

  if (strcmp ("STOPPED", state_name) == 0) {
    state = PLAYBACK_STATE_STOPPED;
  } else if (strcmp ("PLAYING", state_name) == 0) {
    state = PLAYBACK_STATE_PLAYING;
  } else if (strcmp ("PAUSED_PLAYBACK", state_name) == 0) {
    state = PLAYBACK_STATE_PAUSED;
  } else if (strcmp ("TRANSITIONING", state_name) == 0) {
    state = PLAYBACK_STATE_TRANSITIONING;
  } else {
    state = PLAYBACK_STATE_UNKNOWN;
  }

  return state;
}

static void
g_value_free (gpointer data)
{
  g_value_unset ((GValue *) data);
  g_slice_free (GValue, data);
}

static gint
find_udn (gconstpointer d1, gconstpointer d2) 
{
  dmr* c = (dmr*) d1;
  GUPnPDeviceInfo* info = GUPNP_DEVICE_INFO(c->proxy);
  gchar* udn = (gchar*) d2;
  return strcmp(gupnp_device_info_get_udn(info), udn);
}

static gboolean
find_renderer (const char *udn, dmr** c)
{
  gboolean ret = FALSE;
  //g_assert (udn != NULL);
  if (!udn)
    return FALSE;
  g_rec_mutex_lock (&evlock);
  GList* list = g_list_find_custom (dmrList, udn, find_udn);
  if (list) {
    if (c)
      *c = (dmr*)list->data;
    ret = TRUE;
  }
  g_rec_mutex_unlock (&evlock);

  return ret;
}

static GUPnPServiceProxy *
get_selected_av_transport (gchar **sink_protocol_info, char *target)
{
  GUPnPServiceProxy *av_transport;
  dmr* c;

  if (!find_renderer(target, &c))
    return NULL;

  if (sink_protocol_info != NULL) {
    *sink_protocol_info = g_strdup(c->sink_protocol_info);
  }

  return ((GUPnPServiceProxy*)g_object_ref(G_OBJECT(c->av_transport)));
}

static GUPnPServiceProxy *
get_av_transport (GUPnPDeviceProxy *renderer)
{
  GUPnPDeviceInfo  *info;
  GUPnPServiceInfo *av_transport;

  info = GUPNP_DEVICE_INFO (renderer);

  av_transport = gupnp_device_info_get_service (info, AV_TRANSPORT);

  return GUPNP_SERVICE_PROXY (av_transport);
}

static GUPnPServiceProxy *
get_connection_manager (GUPnPDeviceProxy *proxy)
{
  GUPnPDeviceInfo  *info;
  GUPnPServiceInfo *cm;

  info = GUPNP_DEVICE_INFO (proxy);

  cm = gupnp_device_info_get_service (info, CONNECTION_MANAGER);

  return GUPNP_SERVICE_PROXY (cm);
}

static GUPnPServiceProxy *
get_rendering_control (GUPnPDeviceProxy *proxy)
{
  GUPnPDeviceInfo  *info;
  GUPnPServiceInfo *rendering_control;

  info = GUPNP_DEVICE_INFO (proxy);

  rendering_control = gupnp_device_info_get_service (info,
      RENDERING_CONTROL);

  return GUPNP_SERVICE_PROXY (rendering_control);
}

static void
on_didl_object_available (GUPnPDIDLLiteParser *parser,
                          GUPnPDIDLLiteObject *object,
                          gpointer             user_data)
{
  //GUPnPDIDLLiteResource **resource;
  SetAVTransportURIData  *rdata;
  GUPnPServiceProxy      *av_transport;
  char                   *sink_protocol_info;
  gboolean                lenient_mode = FALSE;

  //resource = (GUPnPDIDLLiteResource **) user_data;
  rdata = (SetAVTransportURIData*)user_data;

  av_transport = get_selected_av_transport (&sink_protocol_info, rdata->target);
  if (av_transport == NULL) {
    g_warning ("No renderer selected");
    return;
  }

  rdata->resource = gupnp_didl_lite_object_get_compat_resource
    (object,
     sink_protocol_info,
     lenient_mode);

  g_free (sink_protocol_info);
  g_object_unref (av_transport);
}

static GUPnPDIDLLiteResource *
find_compat_res_from_metadata (const char *metadata, char *target)
{
  GUPnPDIDLLiteParser   *parser;
  //GUPnPDIDLLiteResource *resource;
  SetAVTransportURIData rdata; 
  GError *error;
  

  parser = gupnp_didl_lite_parser_new ();
  rdata.resource = NULL;
  rdata.target = target;
  error = NULL;

  g_signal_connect (parser,
      "object-available",
      G_CALLBACK (on_didl_object_available),
      &rdata);

  /* Assumption: metadata only contains a single didl object */
  gupnp_didl_lite_parser_parse_didl (parser, metadata, &error);
  if (error) {
    g_warning ("%s\n", error->message);
    g_error_free (error);
  }

  g_object_unref (parser);

  return rdata.resource;
}

static void
append_media_renderer_to_list (GUPnPDeviceProxy  *proxy,
                               GUPnPServiceProxy *av_transport,
                               GUPnPServiceProxy *rendering_control,
                               const char        *udn)
{
  GUPnPDeviceInfo  *info;
  char             *name;

  info = GUPNP_DEVICE_INFO (proxy);

  name = gupnp_device_info_get_friendly_name (info);
  if (name == NULL)
    name = g_strdup (udn);
  g_print ("%s\n", name);

  g_rec_mutex_lock (&evlock);
  dmr* c = (dmr*)g_malloc(sizeof(dmr));
  c->name = g_strdup(name);
  c->proxy = g_object_ref(G_OBJECT(proxy));
  c->av_transport = g_object_ref(G_OBJECT(av_transport));
  c->rendering_control = g_object_ref(G_OBJECT(rendering_control));
  c->state = PLAYBACK_STATE_UNKNOWN;

  dmrList = g_list_append(dmrList, c); 
  g_rec_mutex_unlock (&evlock);
 
  gupnp_service_proxy_set_subscribed (av_transport, TRUE);
  gupnp_service_proxy_set_subscribed (rendering_control, TRUE);

  g_free (name);
}

static void
get_protocol_info_cb (GUPnPServiceProxy       *cm,
                      GUPnPServiceProxyAction *action,
                      gpointer                 user_data)
{
  gchar      *sink_protocol_info;
  const gchar *udn;
  GError      *error;

  udn = gupnp_service_info_get_udn (GUPNP_SERVICE_INFO (cm));

  error = NULL;
  if (!gupnp_service_proxy_end_action (cm,
        action,
        &error,
        "Sink",
        G_TYPE_STRING,
        &sink_protocol_info,
        NULL)) {
    g_warning ("Failed to get sink protocol info from "
        "media renderer '%s':%s\n",
        udn,
        error->message);
    g_error_free (error);

    goto return_point;
  }

  if (sink_protocol_info) {
    dmr* c;
    if (find_renderer (udn, &c)) {
      c->sink_protocol_info = g_strdup(sink_protocol_info);
    }
    g_free(sink_protocol_info);
  }

return_point:
  g_object_unref (cm);
}

static void
get_transport_info_cb (GUPnPServiceProxy       *av_transport,
                      GUPnPServiceProxyAction *action,
                      gpointer                 user_data)
{
  gchar       *state_name;
  const gchar *udn;
  GError      *error;

  udn = gupnp_service_info_get_udn (GUPNP_SERVICE_INFO (av_transport));

  error = NULL;
  if (!gupnp_service_proxy_end_action (av_transport,
        action,
        &error,
        "CurrentTransportState",
        G_TYPE_STRING,
        &state_name,
        NULL)) {
    g_warning ("Failed to get transport info from media renderer"
        " '%s':%s\n",
        udn,
        error->message);
    g_error_free (error);

    goto return_point;
  }

  if (state_name) {
    dmr* c;
    if (find_renderer (udn, &c)) {
      c->state = state_name_to_state (state_name); 
    } 
    g_free (state_name);
  }

return_point:
  g_object_unref (av_transport);
}

static void
add_media_renderer (GUPnPDeviceProxy *proxy)
{
  const char        *udn;
  GUPnPServiceProxy *cm;
  GUPnPServiceProxy *av_transport;
  GUPnPServiceProxy *rendering_control;

  udn = gupnp_device_info_get_udn (GUPNP_DEVICE_INFO (proxy));
  if (udn == NULL) {
    return;
  }

  g_print ("adding DMR %s\n", udn);

  cm = get_connection_manager (proxy);
  if (G_UNLIKELY (cm == NULL)) {
    return;
  }

  av_transport = get_av_transport (proxy);
  if (av_transport == NULL)
    goto no_av_transport;

  rendering_control = get_rendering_control (proxy);
  if (rendering_control == NULL)
    goto no_rendering_control;

  if (!find_renderer (udn, NULL))
    append_media_renderer_to_list (proxy,
        av_transport,
        rendering_control,
        udn);

  gupnp_service_proxy_begin_action (g_object_ref (G_OBJECT(cm)),
      "GetProtocolInfo",
      get_protocol_info_cb,
      NULL,
      NULL);

  gupnp_service_proxy_begin_action (g_object_ref (G_OBJECT(av_transport)),
      "GetTransportInfo",
      get_transport_info_cb,
      NULL,
      "InstanceID", G_TYPE_UINT, 0,
      NULL);

  g_object_unref (rendering_control);

no_rendering_control:
  g_object_unref (av_transport);
no_av_transport:
  g_object_unref (cm);
}

static void
dmr_proxy_available_cb (GUPnPControlPoint *cp,
                        GUPnPDeviceProxy  *proxy)
{
  add_media_renderer (proxy);
}

static void
remove_media_renderer (GUPnPDeviceProxy *proxy)
{
  GList* list;

  g_rec_mutex_lock (&evlock);
  list = g_list_find_custom (dmrList, 
    gupnp_device_info_get_udn(GUPNP_DEVICE_INFO(proxy)), find_udn);
  
  g_print ("removing DMR %s\n", gupnp_device_info_get_udn(
        GUPNP_DEVICE_INFO(proxy)));

  if (list) {
    dmr* c = (dmr*)list->data;
    g_free(c->name);
    g_free(c->sink_protocol_info);
    g_object_unref (c->proxy);
    g_object_unref (c->av_transport);
    g_object_unref (c->rendering_control);
    g_free(c);
    dmrList = g_list_remove_link (dmrList, list);
    g_list_free (list);
  }
  
  g_rec_mutex_unlock (&evlock);
}

static void
dmr_proxy_unavailable_cb (GUPnPControlPoint *cp,
                          GUPnPDeviceProxy  *proxy)
{
  remove_media_renderer (proxy);
}

static void
on_context_available (GUPnPContextManager *context_manager,
                      GUPnPContext        *context,
                      gpointer             user_data)
{
  dmr_cp = gupnp_control_point_new (context, MEDIA_RENDERER);

  //g_print ("context available\n");

  g_signal_connect (dmr_cp,
      "device-proxy-available",
      G_CALLBACK (dmr_proxy_available_cb),
      NULL);
  g_signal_connect (dmr_cp,
      "device-proxy-unavailable",
      G_CALLBACK (dmr_proxy_unavailable_cb),
      NULL);

  gssdp_resource_browser_set_active (GSSDP_RESOURCE_BROWSER (dmr_cp),
      TRUE);

  /* Let context manager take care of the control point life cycle */
  gupnp_context_manager_manage_control_point (context_manager, dmr_cp);

  /* We don't need to keep our own references to the control points */
  g_object_unref (dmr_cp);
}

static gboolean
init_upnp (int port)
{
  context_manager = gupnp_context_manager_create (port);
  g_assert (context_manager != NULL);

  g_signal_connect (context_manager,
      "context-available",
      G_CALLBACK (on_context_available),
      NULL);

  return TRUE;
}

static void
av_transport_action_cb (GUPnPServiceProxy       *av_transport,
                        GUPnPServiceProxyAction *action,
                        gpointer                 user_data)
{
  const char *action_name;
  GError *error;

  action_name = (const char *) user_data;

  error = NULL;
  if (!gupnp_service_proxy_end_action (av_transport,
        action,
        &error,
        NULL)) {
    const char *udn;

    udn = gupnp_service_info_get_udn
      (GUPNP_SERVICE_INFO (av_transport));

    g_warning ("Failed to send action '%s' to '%s': %s",
        action_name,
        udn,
        error->message);

    g_error_free (error);
  }

  g_object_unref (av_transport);
}

static GList *
create_av_transport_args (char **additional_args, GList **out_values)
{
  GValue     *instance_id;
  gint        i;
  GList      *names = NULL, *values = NULL;

  instance_id = g_slice_alloc0 (sizeof (GValue));
  g_value_init (instance_id, G_TYPE_UINT);
  g_value_set_uint (instance_id, 0);

  names = g_list_append (names, g_strdup ("InstanceID"));
  values = g_list_append (values, instance_id);


  if (additional_args != NULL) {
    for (i = 0; additional_args[i]; i += 2) {
      GValue *value;

      value = g_slice_alloc0 (sizeof (GValue));
      g_value_init (value, G_TYPE_STRING);
      g_value_set_string (value, additional_args[i + 1]);
      names = g_list_append (names,
          g_strdup (additional_args[i]));
      values = g_list_append (values, value);
    }
  }

  *out_values = values;

  return names;
}

static void
av_transport_send_action (char *action,
                          char *additional_args[],
                          char *target)
{
  GUPnPServiceProxy *av_transport;
  GList             *names, *values;

  av_transport = get_selected_av_transport (NULL, target);
  if (av_transport == NULL) {
    g_warning ("No renderer selected");
    return;
  }

  names = create_av_transport_args (additional_args, &values);

  gupnp_service_proxy_begin_action_list (av_transport,
      action,
      names,
      values,
      av_transport_action_cb,
      (char *) action);
  g_list_free_full (names, g_free);
  g_list_free_full (values, g_value_free);
}

static SetAVTransportURIData *
set_av_transport_uri_data_new (void (*callback) (char*),
                               GUPnPDIDLLiteResource *resource,
                               gchar *target)
{
  SetAVTransportURIData *data;

  data = g_slice_new (SetAVTransportURIData);
  data->callback = callback;
  data->target = g_strdup (target);
  data->resource = resource; /* Steal the ref */

  return data;
}

static void
set_av_transport_uri_data_free (SetAVTransportURIData *data)
{
  g_object_unref (data->resource);
  g_free (data->target);
  g_slice_free (SetAVTransportURIData, data);
}

static void
set_av_transport_uri_cb (GUPnPServiceProxy       *av_transport,
                         GUPnPServiceProxyAction *action,
                         gpointer                 user_data)
{
  SetAVTransportURIData *data;
  GError                *error;

  data = (SetAVTransportURIData *) user_data;

  error = NULL;
  if (gupnp_service_proxy_end_action (av_transport,
        action,
        &error,
        NULL)) {
    if (data->callback) {
      data->callback (data->target);
    }
  } else {
    const char *udn;
    udn = gupnp_service_info_get_udn
      (GUPNP_SERVICE_INFO (av_transport));
    g_warning ("Failed to set URI '%s' on %s: %s",
        gupnp_didl_lite_resource_get_uri (data->resource),
        udn,
        error->message);
    g_error_free (error);
  }

  set_av_transport_uri_data_free (data);
  g_object_unref (av_transport);
}

static void
play (char *target)
{
  char *args[] = { "Speed", "1", NULL };
  av_transport_send_action ("Play", args, target);
}

static void
set_av_transport_uri (const char *metadata,
                       void (*callback) (char*), char* rtarget)
{
  GUPnPServiceProxy     *av_transport;
  SetAVTransportURIData *data;
  GUPnPDIDLLiteResource *resource;
  const char            *uri;

  av_transport = get_selected_av_transport (NULL, rtarget);
  if (av_transport == NULL) {
    g_warning ("No renderer selected");
    return;
  }

  resource = find_compat_res_from_metadata (metadata, rtarget);
  if (resource == NULL) {
    g_warning ("no compatible URI found.");

    g_object_unref (av_transport);

    return;
  }

  data = set_av_transport_uri_data_new (callback, resource, rtarget);
  uri = gupnp_didl_lite_resource_get_uri (resource);

  gupnp_service_proxy_begin_action (av_transport,
      "SetAVTransportURI",
      set_av_transport_uri_cb,
      data,
      "InstanceID",
      G_TYPE_UINT,
      0,
      "CurrentURI",
      G_TYPE_STRING,
      uri,
      "CurrentURIMetaData",
      G_TYPE_STRING,
      metadata,
      NULL);
}

static void
up_ev_play(char* rtarget, char* url) 
{
  /*char r[1500];
    FILE* fp = fopen ("./tmp.ddl", "r");
    memset(r,0,1500);
    fread(r,1,1500,fp);
    fclose(fp);*/

  gchar *r, *title;
  GUPnPDIDLLiteResource *res;
  GUPnPProtocolInfo *info;
  GUPnPDIDLLiteWriter *writer;
  GUPnPDIDLLiteObject *item;
  //struct ifaddrs *myaddrs, *ifa;
  //struct sockaddr_in *s4;
  char buf[512];

  writer = gupnp_didl_lite_writer_new (NULL);
  item = GUPNP_DIDL_LITE_OBJECT (gupnp_didl_lite_writer_add_item (writer));
  sprintf(buf, "%d", rand());
  gupnp_didl_lite_object_set_id (item, buf);
  gupnp_didl_lite_object_set_restricted (item, TRUE);
  title = g_strdup ("cotigao_rocks");
  gupnp_didl_lite_object_set_title (item, title);
  g_free (title);
  gupnp_didl_lite_object_set_upnp_class (item, "object.item.videoItem");

  res = gupnp_didl_lite_object_add_resource (item);

  /*strcpy (buf, "http://");
  getifaddrs(&myaddrs);
  for (ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
  {
    if (ifa->ifa_addr == NULL) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0) continue;

    if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, "lo")) {
      s4 = (struct sockaddr_in *)(ifa->ifa_addr);
      if (inet_ntop(ifa->ifa_addr->sa_family, (void *)&(s4->sin_addr), buf+7, sizeof(buf)-6) == NULL)
        printf("%s: inet_ntop failed!\n", ifa->ifa_name);
      else {
        strcat (buf, ":7070/camera.mp4");
        printf("%s: %s\n", ifa->ifa_name, buf);
        break;
      }
    }
  }*/

  gupnp_didl_lite_resource_set_uri (res, url);

  info = gupnp_protocol_info_new ();
  gupnp_protocol_info_set_protocol (info, "http-get");
  gupnp_protocol_info_set_network (info, "*");
  gupnp_protocol_info_set_mime_type (info, "video/mp4");
  gupnp_protocol_info_set_dlna_profile(info, "AVC_MP4_BL_CIF15_AAC_520");
  gupnp_protocol_info_set_dlna_operation (info, GUPNP_DLNA_OPERATION_NONE);
  gupnp_protocol_info_set_dlna_conversion (info, GUPNP_DLNA_CONVERSION_TRANSCODED);
  gupnp_protocol_info_set_dlna_flags (info,
      /*GUPNP_DLNA_FLAGS_SENDER_PACED|GUPNP_DLNA_FLAGS_PLAY_CONTAINER|*/
      GUPNP_DLNA_FLAGS_S0_INCREASE|GUPNP_DLNA_FLAGS_SN_INCREASE|
      GUPNP_DLNA_FLAGS_STREAMING_TRANSFER_MODE|GUPNP_DLNA_FLAGS_BACKGROUND_TRANSFER_MODE|
      GUPNP_DLNA_FLAGS_CONNECTION_STALL|GUPNP_DLNA_FLAGS_DLNA_V15);

  gupnp_didl_lite_resource_set_protocol_info (res, info);
  gupnp_didl_lite_resource_set_width (res, 320);
  gupnp_didl_lite_resource_set_height (res, 240);

  g_object_unref (info);
  g_object_unref (res);

  {
    xmlNodePtr root = gupnp_didl_lite_writer_get_xml_node (writer);
    xmlNsPtr ns1 = xmlNewNs(root, BAD_CAST "urn:schemas-dlna-org:metadata-1-0/", BAD_CAST "dlna");
    xmlNodePtr child1 = xmlNewChild(root, NULL, NULL, NULL);
    xmlSetNs (child1, ns1);
  }

  r = gupnp_didl_lite_writer_get_string (writer);
  g_object_unref (item);
  g_object_unref (writer);

  printf ("%s\n", r);
  set_av_transport_uri(r, play, rtarget); 

  g_free(r);
}

void
up_play (char* target, char *url)
{
  cmd* c = g_new0(cmd, 1);
  c->type = EV_PLAY;
  c->data1 = (void*)g_strdup(target);
  c->data2 = (void*)g_strdup(url);
  gst_atomic_queue_push(aqueue, c);
}

static void
up_ev_scan ()
{
  GSSDPResourceBrowser *browser = GSSDP_RESOURCE_BROWSER (dmr_cp);
  gssdp_resource_browser_rescan (browser);
}

//char**
struct Renderer*
up_scan (int *len)
{
  struct Renderer* r = g_new0(struct Renderer, 10);
  //char** arr = g_malloc0 (20 * sizeof(char*));
  GList* list;
  gint i = 0;

  cmd* c = g_new0(cmd, 1);
  c->type = EV_SCAN;
  gst_atomic_queue_push(aqueue, c);

  sleep(5);
  
  g_rec_mutex_lock (&evlock);
 
  for (list = dmrList; list != NULL && i < 10; list = list->next) {
    dmr *c = (dmr*)list->data;
    //arr[i] =g_strdup( gupnp_device_info_get_udn (GUPNP_DEVICE_INFO (c->proxy)));
    //arr[i+1] = g_strdup(c->name);
    //i += 2;
    strcpy (r[i].Udn, gupnp_device_info_get_udn (GUPNP_DEVICE_INFO (c->proxy)));
    strcpy (r[i++].Name, c->name);
  }
 
  *len = i;
  g_rec_mutex_unlock (&evlock);
  
  return r;
}

static void
up_ev_stop(char* target) 
{
  av_transport_send_action ("Stop", NULL, target);
}

void
up_stop (char *target)
{
  cmd* c = g_new0(cmd, 1);
  c->type = EV_STOP;
  c->data1 = g_strdup (target);
  gst_atomic_queue_push(aqueue, c);
}

static gboolean
listener(gpointer udata) 
{
  gpointer data;

  while ((data = gst_atomic_queue_pop(aqueue))) {
    cmd* c = (cmd*) data;
  
    switch (c->type) {
      case EV_STOP:
        g_print ("sending stop\n");
        up_ev_stop(c->data1);
        break;
    
      case EV_PLAY:
        g_print ("sending play %s %s\n", (char*)c->data1, (char*)c->data2);
        up_ev_play(c->data1, c->data2);
        break;
    
      case EV_SCAN:
        g_print ("sending scan\n");
        up_ev_scan();
        break;
    }

    if (c->data1) 
      g_free(c->data1);
    if (c->data2) 
      g_free(c->data2);
    g_free(c);
  }

  return TRUE;
}

static gpointer
upnp_thread (gpointer data)
{
  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  
  if(!init_upnp(0)) {
    g_print ("upnp init failed");
    return NULL;
  }

  g_timeout_add(400, listener, NULL);  
  g_main_loop_run (loop);
  return NULL;
} 

void*
start_upnp (void)
{
  GThread* td;
#if !GLIB_CHECK_VERSION(2, 35, 0)
  g_type_init ();
#endif
  aqueue = gst_atomic_queue_new(0);
  g_rec_mutex_init(&evlock);
  td = g_thread_new("upnp", upnp_thread, NULL);
  return (void*)td;
}

void
stop_upnp(void* td)
{
}

/*
int main()
{
  start_upnp();
  sleep(10);
  up_play("uuid:6f4e620e-f056-581b-752b-1c0ce33bc370"); 
  sleep(60);
  up_stop(); 
  sleep(10000);  
}*/
