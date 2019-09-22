/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <spa/monitor/monitor.h>
#include <pipewire/pipewire.h>

#include "monitor.h"
#include "error.h"
#include "wpenums.h"

typedef struct {
  struct spa_handle *handle;
  gpointer interface;
} WpSpaObject;

struct _WpMonitor
{
  GObject parent;

  /* Props */
  GWeakRef core;
  gchar *factory_name;
  WpMonitorFlags flags;

  /* Monitor info */
  WpSpaObject *spa_mon;
  GList *devices;  /* element-type: struct device* */
};

struct device
{
  guint32 id;
  WpMonitor *self;

  WpSpaObject *spa_dev;
  WpProxy *proxy;
  WpProperties *properties;
  GList *nodes;  /* element-type: struct node* */

  struct spa_hook listener;
};

struct node
{
  guint32 id;
  WpMonitor *self;

  struct pw_node *node;
  WpProxy *proxy;
};

enum {
  PROP_0,
  PROP_CORE,
  PROP_FACTORY_NAME,
  PROP_FLAGS,
};

enum {
  SIG_SETUP_NODE_PROPS,
  SIG_SETUP_DEVICE_PROPS,
  N_SIGNALS
};

static guint32 signals[N_SIGNALS] = {0};

G_DEFINE_TYPE (WpMonitor, wp_monitor, G_TYPE_OBJECT)

/* spa object */

static void
wp_spa_object_free (WpSpaObject *self)
{
  pw_unload_spa_handle (self->handle);
}

static inline WpSpaObject *
wp_spa_object_ref (WpSpaObject *self)
{
  return g_rc_box_acquire (self);
}

static inline void
wp_spa_object_unref (WpSpaObject *self)
{
  g_rc_box_release_full (self, (GDestroyNotify) wp_spa_object_free);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WpSpaObject, wp_spa_object_unref)

static WpSpaObject *
load_spa_object (WpCore *core, const gchar *factory, guint32 iface_type,
    WpProperties *props, GError **error)
{
  g_autoptr (WpSpaObject) self = g_rc_box_new0 (WpSpaObject);
  gint res;

  /* Load the monitor handle */
  self->handle = pw_core_load_spa_handle (wp_core_get_pw_core (core),
      factory, props ? wp_properties_peek_dict (props) : NULL);
  if (!self->handle) {
    g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_OPERATION_FAILED,
        "SPA handle '%s' could not be loaded; is it installed?",
        factory);
    return NULL;
  }

  /* Get the handle interface */
  res = spa_handle_get_interface (self->handle, iface_type, &self->interface);
  if (res < 0) {
    g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_OPERATION_FAILED,
        "Could not get interface 0x%x from SPA handle", iface_type);
    return NULL;
  }

  return g_steal_pointer (&self);
}

/* common */

static gpointer
find_object (GList *list, guint32 id, GList **link)
{
  /*
   * The first element of struct device & struct node
   * is the guint32 containing the id, so we can directly cast
   * the list data to guint32, no matter what the actual structure is
   */
  for (; list; list = g_list_next (list)) {
    if (id == *((guint32 *) list->data)) {
      *link = list;
      return list->data;
    }
  }
  return NULL;
}

/* node */

static struct node *
node_new (struct device *dev, uint32_t id,
    const struct spa_device_object_info *info)
{
  WpMonitor *self = dev->self;
  g_autoptr (WpCore) core = NULL;
  g_autoptr (WpProperties) props = NULL;
  struct pw_proxy *pw_proxy = NULL;
  struct pw_node *pw_node = NULL;
  struct node *node = NULL;
  const gchar *pw_factory_name = "spa-node-factory";

  if (info->type != SPA_TYPE_INTERFACE_Node)
    return NULL;

  g_debug ("WpMonitor:%p:%s new node %u", self, self->factory_name, id);

  /* use the adapter instead of spa-node-factory if requested */
  if (self->flags & WP_MONITOR_FLAG_USE_ADAPTER)
    pw_factory_name = "adapter";

  core = g_weak_ref_get (&self->core);
  props = wp_properties_new_copy_dict (info->props);

  /* pass down the id to the setup function */
  wp_properties_setf (props, WP_MONITOR_KEY_OBJECT_ID, "%u", id);

  /* the SPA factory name must be set as a property
     for the spa-node-factory / adapter */
  wp_properties_set (props, PW_KEY_FACTORY_NAME, info->factory_name);

  /* the rest is up to the user */
  g_signal_emit (self, signals[SIG_SETUP_NODE_PROPS], 0, dev->properties,
      props);

  /* and delete the id - it should not appear on the proxy */
  wp_properties_set (props, WP_MONITOR_KEY_OBJECT_ID, NULL);

  if (self->flags & WP_MONITOR_FLAG_LOCAL_NODES) {
    struct pw_factory *factory;

    /* create the pipewire node (and the underlying SPA node)
       on the wireplumber process and export it */

    factory = pw_core_find_factory (wp_core_get_pw_core (core), pw_factory_name);
    if (!factory) {
      g_warning ("WpMonitor:%p: no '%s' factory found; node '%s' will "
          "not be created", self, pw_factory_name, info->factory_name);
      return NULL;
    }

    pw_node = pw_factory_create_object (factory,
        NULL,
        PW_TYPE_INTERFACE_Node,
        PW_VERSION_NODE_PROXY,
        wp_properties_to_pw_properties (props),
        0);
    if (!pw_node) {
      g_warning ("WpMonitor:%p: failed to construct pw_node; node '%s' will "
          "not be created", self, info->factory_name);
      return NULL;
    }

    pw_proxy = pw_remote_export (wp_core_get_pw_remote (core),
        PW_TYPE_INTERFACE_Node,
        wp_properties_to_pw_properties (props),
        pw_node,
        0);
    if (!pw_proxy) {
      g_warning ("WpMonitor:%p: failed to export node: %s", self,
          g_strerror (errno));
      pw_node_destroy (pw_node);
      return NULL;
    }
  } else {
    /* create the pipewire node (and the underlying SPA node)
       on the remote pipewire process */
    pw_proxy = pw_core_proxy_create_object (wp_core_get_pw_core_proxy (core),
        pw_factory_name,
        PW_TYPE_INTERFACE_Node,
        PW_VERSION_NODE_PROXY,
        wp_properties_peek_dict (props),
        0);
  }

  node = g_slice_new0 (struct node);
  node->self = self;
  node->id = id;
  node->node = pw_node;
  node->proxy = wp_proxy_new_wrap (core, pw_proxy, PW_TYPE_INTERFACE_Node,
      PW_VERSION_NODE_PROXY);

  return node;
}

static void
node_free (struct node *node)
{
  g_debug ("WpMonitor:%p:%s free node %u", node->self,
      node->self->factory_name, node->id);

  g_clear_object (&node->proxy);
  g_clear_pointer (&node->node, pw_node_destroy);

  g_slice_free (struct node, node);
}

/* device */

static void
device_info (void *data, const struct spa_device_info *info)
{
  struct device *dev = data;

  /*
   * This is emited syncrhonously at the time we add the listener and
   * before object_info is emited. It gives us additional properties
   * about the device, like the "api.alsa.card.*" ones that are not
   * set by the monitor
   */
  if (info->change_mask & SPA_DEVICE_CHANGE_MASK_PROPS)
    wp_properties_update_from_dict (dev->properties, info->props);
}

static void
device_object_info (void *data, uint32_t id,
    const struct spa_device_object_info *info)
{
  struct device *dev = data;
  struct node *node = NULL;
  GList *link = NULL;

  /* Find the node */
  node = find_object (dev->nodes, id, &link);

  if (info && !node) {
    if (!(node = node_new (dev, id, info)))
      return;
    dev->nodes = g_list_append (dev->nodes, node);
  } else if (!info && node) {
    node_free (node);
    dev->nodes = g_list_delete_link (dev->nodes, link);
  }
}

static const struct spa_device_events device_events = {
  SPA_VERSION_DEVICE_EVENTS,
  .info = device_info,
  .object_info = device_object_info
};

static struct device *
device_new (WpMonitor *self, uint32_t id,
    const struct spa_monitor_object_info *info)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WpCore) core = NULL;
  g_autoptr (WpProperties) props = NULL;
  g_autoptr (WpSpaObject) spa_dev = NULL;
  struct pw_proxy *proxy = NULL;
  struct device *dev = NULL;

  if (info->type != SPA_TYPE_INTERFACE_Device)
    return NULL;

  g_debug ("WpMonitor:%p:%s new device %u", self, self->factory_name, id);

  core = g_weak_ref_get (&self->core);
  props = wp_properties_new_copy_dict (info->props);

  /* pass down the id to the setup function */
  wp_properties_setf (props, WP_MONITOR_KEY_OBJECT_ID, "%u", id);

  /* let the handler setup the properties accordingly */
  g_signal_emit (self, signals[SIG_SETUP_DEVICE_PROPS], 0, props);

  /* and delete the id - it should not appear on the proxy */
  wp_properties_set (props, WP_MONITOR_KEY_OBJECT_ID, NULL);

  if (!(spa_dev = load_spa_object (core, info->factory_name, info->type, props,
          &error))) {
    g_warning ("WpMonitor:%p: failed to construct device: %s", self,
        error->message);
    return NULL;
  }

  if (!(proxy = pw_remote_export (wp_core_get_pw_remote (core), info->type,
          wp_properties_to_pw_properties (props), spa_dev->interface, 0))) {
    g_warning ("WpMonitor:%p: failed to export device: %s", self,
        g_strerror (errno));
    return NULL;
  }

  /* Create the device */
  dev = g_slice_new0 (struct device);
  dev->self = self;
  dev->id = id;
  dev->spa_dev = g_steal_pointer (&spa_dev);
  dev->properties = g_steal_pointer (&props);
  dev->proxy = wp_proxy_new_wrap (core, proxy, PW_TYPE_INTERFACE_Device,
      PW_VERSION_DEVICE_PROXY);

  /* Add device listener for events */
  spa_device_add_listener ((struct spa_device *) dev->spa_dev->interface,
      &dev->listener, &device_events, dev);

  return dev;
}

static void
device_free (struct device *dev)
{
  g_debug ("WpMonitor:%p:%s free device %u", dev->self,
      dev->self->factory_name, dev->id);

  spa_hook_remove (&dev->listener);
  g_list_free_full (dev->nodes, (GDestroyNotify) node_free);
  g_clear_object (&dev->proxy);
  g_clear_pointer (&dev->spa_dev, wp_spa_object_unref);
  g_clear_pointer (&dev->properties, wp_properties_unref);

  g_slice_free (struct device, dev);
}

/* monitor */

static int
monitor_object_info (gpointer data, uint32_t id,
    const struct spa_monitor_object_info *info)
{
  WpMonitor * self = WP_MONITOR (data);
  struct device *dev = NULL;
  GList *link = NULL;

  /* Find the device */
  dev = find_object (self->devices, id, &link);

  if (info && !dev) {
    if (!(dev = device_new (self, id, info)))
      return -ENOMEM;
    self->devices = g_list_append (self->devices, dev);
  } else if (!info && dev) {
    device_free (dev);
    self->devices = g_list_delete_link (self->devices, link);
  } else if (!info && !dev) {
    return -ENODEV;
  }

  return 0;
}

static const struct spa_monitor_callbacks monitor_callbacks = {
  SPA_VERSION_MONITOR_CALLBACKS,
  .object_info = monitor_object_info,
};

static void
wp_monitor_init (WpMonitor * self)
{
  g_weak_ref_init (&self->core, NULL);
}

static void
wp_monitor_finalize (GObject * object)
{
  WpMonitor * self = WP_MONITOR (object);

  wp_monitor_stop (self);

  g_weak_ref_clear (&self->core);
  g_free (self->factory_name);

  G_OBJECT_CLASS (wp_monitor_parent_class)->finalize (object);
}

static void
wp_monitor_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  WpMonitor * self = WP_MONITOR (object);

  switch (property_id) {
  case PROP_CORE:
    g_weak_ref_set (&self->core, g_value_get_object (value));
    break;
  case PROP_FACTORY_NAME:
    self->factory_name = g_value_dup_string (value);
    break;
  case PROP_FLAGS:
    self->flags = (WpMonitorFlags) g_value_get_flags (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_monitor_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  WpMonitor * self = WP_MONITOR (object);

  switch (property_id) {
  case PROP_CORE:
    g_value_take_object (value, g_weak_ref_get (&self->core));
    break;
  case PROP_FACTORY_NAME:
    g_value_set_string (value, self->factory_name);
    break;
  case PROP_FLAGS:
    g_value_set_flags (value, (guint) self->flags);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_monitor_class_init (WpMonitorClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;

  object_class->finalize = wp_monitor_finalize;
  object_class->set_property = wp_monitor_set_property;
  object_class->get_property = wp_monitor_get_property;

  /* Install the properties */
  g_object_class_install_property (object_class, PROP_CORE,
      g_param_spec_object ("core", "core", "The wireplumber core",
          WP_TYPE_CORE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FACTORY_NAME,
      g_param_spec_string ("factory-name", "factory-name",
          "The factory name of the monitor", NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FLAGS,
      g_param_spec_flags ("flags", "flags",
          "Additional feature flags", WP_TYPE_MONITOR_FLAGS, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  /**
   * WpMonitor::setup-device-props:
   * @self: the #WpMonitor
   * @device_props: the properties of the device to be created
   *
   * This signal allows the handler to modify the properties of a device
   * object before it is created.
   */
  signals[SIG_SETUP_DEVICE_PROPS] = g_signal_new (
      "setup-device-props", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 1, WP_TYPE_PROPERTIES);

  /**
   * WpMonitor::setup-node-props:
   * @self: the #WpMonitor
   * @device_props: the properties of the parent device
   * @node_props: the properties of the node to be created
   *
   * This signal allows the handler to modify the properties of a node
   * object before it is created.
   */
  signals[SIG_SETUP_NODE_PROPS] = g_signal_new (
      "setup-node-props", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 2, WP_TYPE_PROPERTIES, WP_TYPE_PROPERTIES);
}

/**
 * wp_monitor_new:
 * @core: the wireplumber core
 * @factory_name: the factory name of the monitor
 *
 * Returns: (transfer full): the newly created monitor
 */
WpMonitor *
wp_monitor_new (WpCore * core, const gchar * factory_name, WpMonitorFlags flags)
{
  g_return_val_if_fail (WP_IS_CORE (core), NULL);
  g_return_val_if_fail (factory_name != NULL && *factory_name != '\0', NULL);

  return g_object_new (WP_TYPE_MONITOR,
      "core", core,
      "factory-name", factory_name,
      "flags", flags,
      NULL);
}

const gchar *
wp_monitor_get_factory_name (WpMonitor *self)
{
  g_return_val_if_fail (WP_IS_MONITOR (self), NULL);
  return self->factory_name;
}

gboolean
wp_monitor_start (WpMonitor *self, GError **error)
{
  g_autoptr (WpCore) core = NULL;
  gint ret = 0;

  g_return_val_if_fail (WP_IS_MONITOR (self), FALSE);

  core = g_weak_ref_get (&self->core);

  g_debug ("WpMonitor:%p:%s starting monitor", self, self->factory_name);

  if (!(self->spa_mon = load_spa_object (core, self->factory_name,
          SPA_TYPE_INTERFACE_Monitor, NULL, error))) {
    return FALSE;
  }

  /* Actual monitor implementations start their internal processing
     when the callbacks are set and if they fail, they return an error
     code on this call */
  ret = spa_monitor_set_callbacks (
            (struct spa_monitor *) self->spa_mon->interface,
            &monitor_callbacks, self);
  if (ret < 0) {
    g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_OPERATION_FAILED,
        "Failed to start monitor '%s': %s", self->factory_name,
        g_strerror (-ret));
    g_clear_pointer (&self->spa_mon, wp_spa_object_unref);
    return FALSE;
  }

  return TRUE;
}

void
wp_monitor_stop (WpMonitor *self)
{
  g_return_if_fail (WP_IS_MONITOR (self));

  g_debug ("WpMonitor:%p:%s stopping monitor", self, self->factory_name);

  g_list_free_full (self->devices, (GDestroyNotify) device_free);
  self->devices = NULL;
  g_clear_pointer (&self->spa_mon, wp_spa_object_unref);
}
