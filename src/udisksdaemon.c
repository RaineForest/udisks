/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 * Copyright (C)      2016 Peter Hatina <phatina@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>
#include <blockdev/blockdev.h>

#include "udiskslogging.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udisksprovider.h"
#include "udiskslinuxprovider.h"
#include "udisksmountmonitor.h"
#include "udisksmount.h"
#include "udisksbasejob.h"
#include "udisksspawnedjob.h"
#include "udisksthreadedjob.h"
#include "udiskssimplejob.h"
#include "udisksstate.h"
#include "udisksfstabmonitor.h"
#include "udisksfstabentry.h"
#include "udiskscrypttabmonitor.h"
#include "udiskscrypttabentry.h"
#include "udiskslinuxblockobject.h"
#include "udiskslinuxdevice.h"
#include "udisksmodulemanager.h"
#include "udisksconfigmanager.h"

/**
 * SECTION:udisksdaemon
 * @title: UDisksDaemon
 * @short_description: Main daemon object
 *
 * Object holding all global state.
 */

typedef struct _UDisksDaemonClass   UDisksDaemonClass;

/**
 * UDisksDaemon:
 *
 * The #UDisksDaemon structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksDaemon
{
  GObject parent_instance;
  GDBusConnection *connection;
  GDBusObjectManagerServer *object_manager;

  UDisksMountMonitor *mount_monitor;

  UDisksLinuxProvider *linux_provider;

  /* may be NULL if polkit is masked */
  PolkitAuthority *authority;

  UDisksState *state;

  UDisksFstabMonitor *fstab_monitor;

  UDisksCrypttabMonitor *crypttab_monitor;

  UDisksModuleManager *module_manager;

  UDisksConfigManager *config_manager;

  gboolean disable_modules;
  gboolean force_load_modules;
  gboolean uninstalled;
};

struct _UDisksDaemonClass
{
  GObjectClass parent_class;
};

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_OBJECT_MANAGER,
  PROP_MOUNT_MONITOR,
  PROP_FSTAB_MONITOR,
  PROP_CRYPTTAB_MONITOR,
  PROP_MODULE_MANAGER,
  PROP_CONFIG_MANAGER,
  PROP_DISABLE_MODULES,
  PROP_FORCE_LOAD_MODULES,
  PROP_UNINSTALLED,
};

G_DEFINE_TYPE (UDisksDaemon, udisks_daemon, G_TYPE_OBJECT);

static void
udisks_daemon_finalize (GObject *object)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);

  udisks_state_stop_cleanup (daemon->state);
  g_object_unref (daemon->state);

  g_clear_object (&daemon->authority);
  g_object_unref (daemon->object_manager);
  g_object_unref (daemon->linux_provider);
  g_object_unref (daemon->connection);

  /* Modules use the monitors and try to reference them when cleaning up */
  udisks_module_manager_unload_modules (daemon->module_manager);
  g_object_unref (daemon->mount_monitor);
  g_object_unref (daemon->fstab_monitor);
  g_object_unref (daemon->crypttab_monitor);

  g_clear_object (&daemon->module_manager);

  g_clear_object (&daemon->config_manager);

  if (G_OBJECT_CLASS (udisks_daemon_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_daemon_parent_class)->finalize (object);
}

static void
udisks_daemon_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_value_set_object (value, udisks_daemon_get_connection (daemon));
      break;

    case PROP_OBJECT_MANAGER:
      g_value_set_object (value, udisks_daemon_get_object_manager (daemon));
      break;

    case PROP_MOUNT_MONITOR:
      g_value_set_object (value, udisks_daemon_get_mount_monitor (daemon));
      break;

    case PROP_FSTAB_MONITOR:
      g_value_set_object (value, udisks_daemon_get_fstab_monitor (daemon));
      break;

    case PROP_CRYPTTAB_MONITOR:
      g_value_set_object (value, udisks_daemon_get_crypttab_monitor (daemon));
      break;

    case PROP_MODULE_MANAGER:
      g_value_set_object (value, udisks_daemon_get_module_manager (daemon));
      break;

    case PROP_CONFIG_MANAGER:
      g_value_set_object (value, udisks_daemon_get_config_manager (daemon));
      break;

    case PROP_DISABLE_MODULES:
      g_value_set_boolean (value, udisks_daemon_get_disable_modules (daemon));
      break;

    case PROP_FORCE_LOAD_MODULES:
      g_value_set_boolean (value, udisks_daemon_get_force_load_modules (daemon));
      break;

    case PROP_UNINSTALLED:
      g_value_set_boolean (value, udisks_daemon_get_uninstalled (daemon));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_daemon_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_assert (daemon->connection == NULL);
      daemon->connection = g_value_dup_object (value);
      break;

    case PROP_DISABLE_MODULES:
      daemon->disable_modules = g_value_get_boolean (value);
      break;

    case PROP_FORCE_LOAD_MODULES:
      daemon->force_load_modules = g_value_get_boolean (value);
      break;

    case PROP_UNINSTALLED:
      daemon->uninstalled = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_daemon_init (UDisksDaemon *daemon)
{
}

static void
mount_monitor_on_mount_removed (UDisksMountMonitor *monitor,
                                UDisksMount        *mount,
                                gpointer            user_data)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (user_data);
  udisks_state_check (daemon->state);
}

static void
udisks_daemon_constructed (GObject *object)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);
  GError *error;
  gboolean ret = FALSE;

  /* NULL means no specific so_name (implementation) */
  BDPluginSpec part_plugin = {BD_PLUGIN_PART, NULL};
  BDPluginSpec swap_plugin = {BD_PLUGIN_SWAP, NULL};
  BDPluginSpec mdraid_plugin = {BD_PLUGIN_MDRAID, NULL};

  /* the core daemon only needs the part, swap plugins and mdraid, additional
     plugins are required by various modules, but they make sure plugins are
     loaded themselves */
  BDPluginSpec *plugins[] = {&part_plugin, &swap_plugin, &mdraid_plugin, NULL};

  error = NULL;

  ret = bd_ensure_init (plugins, NULL, &error);
  if (!ret)
    {
      udisks_error ("Error initializing libblockdev library: %s (%s, %d)",
                    error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  daemon->authority = polkit_authority_get_sync (NULL, &error);
  if (daemon->authority == NULL)
    {
      udisks_critical ("Error initializing polkit authority: %s (%s, %d)",
                    error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
    }

  daemon->object_manager = g_dbus_object_manager_server_new ("/org/freedesktop/UDisks2");

  if (!g_file_test ("/run/udisks2", G_FILE_TEST_IS_DIR))
    {
      if (g_mkdir_with_parents ("/run/udisks2", 0700) != 0)
        {
          udisks_critical ("Error creating directory %s: %m", "/run/udisks2");
        }
    }

  if (!g_file_test (PACKAGE_LOCALSTATE_DIR "/lib/udisks2", G_FILE_TEST_IS_DIR))
    {
      if (g_mkdir_with_parents (PACKAGE_LOCALSTATE_DIR "/lib/udisks2", 0700) != 0)
        {
          udisks_critical ("Error creating directory %s: %m", PACKAGE_LOCALSTATE_DIR "/lib/udisks2");
        }
    }


  if (! daemon->uninstalled)
    {
      daemon->config_manager = udisks_config_manager_new ();
      daemon->module_manager = udisks_module_manager_new (daemon);
    }
  else
    {
      daemon->config_manager = udisks_config_manager_new_uninstalled ();
      daemon->module_manager = udisks_module_manager_new_uninstalled (daemon);
    }

  daemon->mount_monitor = udisks_mount_monitor_new ();

  daemon->state = udisks_state_new (daemon);

  g_signal_connect (daemon->mount_monitor,
                    "mount-removed",
                    G_CALLBACK (mount_monitor_on_mount_removed),
                    daemon);

  daemon->fstab_monitor = udisks_fstab_monitor_new ();
  daemon->crypttab_monitor = udisks_crypttab_monitor_new ();

  /* now add providers */
  daemon->linux_provider = udisks_linux_provider_new (daemon);

  if (daemon->force_load_modules
      || (udisks_config_manager_get_load_preference (daemon->config_manager)
          == UDISKS_MODULE_LOAD_ONSTARTUP))
    {
      udisks_module_manager_load_modules (daemon->module_manager);
    }

  udisks_provider_start (UDISKS_PROVIDER (daemon->linux_provider));

  /* Export the ObjectManager */
  g_dbus_object_manager_server_set_connection (daemon->object_manager, daemon->connection);

  /* Start cleaning up */
  udisks_state_start_cleanup (daemon->state);
  udisks_state_check (daemon->state);

  if (G_OBJECT_CLASS (udisks_daemon_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (udisks_daemon_parent_class)->constructed (object);
}


static void
udisks_daemon_class_init (UDisksDaemonClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_daemon_finalize;
  gobject_class->constructed  = udisks_daemon_constructed;
  gobject_class->set_property = udisks_daemon_set_property;
  gobject_class->get_property = udisks_daemon_get_property;

  /**
   * UDisksDaemon:connection:
   *
   * The #GDBusConnection the daemon is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The D-Bus connection the daemon is for",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksDaemon:object-manager:
   *
   * The #GDBusObjectManager used by the daemon
   */
  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_MANAGER,
                                   g_param_spec_object ("object-manager",
                                                        "Object Manager",
                                                        "The D-Bus Object Manager server used by the daemon",
                                                        G_TYPE_DBUS_OBJECT_MANAGER_SERVER,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksDaemon:mount-monitor:
   *
   * The #UDisksMountMonitor used by the daemon
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MOUNT_MONITOR,
                                   g_param_spec_object ("mount-monitor",
                                                        "Mount Monitor",
                                                        "The mount monitor",
                                                        UDISKS_TYPE_MOUNT_MONITOR,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksDaemon:disable-modules:
   *
   * Whether modules should be disabled
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DISABLE_MODULES,
                                   g_param_spec_boolean ("disable-modules",
                                                         "Disable modules",
                                                         "Whether modules should be disabled",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksDaemon:force-load-modules:
   *
   * Whether modules should be activated upon startup
   */
  g_object_class_install_property (gobject_class,
                                   PROP_FORCE_LOAD_MODULES,
                                   g_param_spec_boolean ("force-load-modules",
                                                         "Force load modules",
                                                         "Whether modules should be activated upon startup",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksDaemon:uninstalled:
   *
   * Loads modules from the build directory.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_UNINSTALLED,
                                   g_param_spec_boolean ("uninstalled",
                                                         "Load modules from the build directory",
                                                         "Whether the modules should be loaded from the build directory",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));
}

/**
 * udisks_daemon_new:
 * @connection: A #GDBusConnection.
 * @disable_modules: Indicates whether modules should never be activated.
 * @force_load_modules: Activate modules on startup (for debugging purposes).
 * @uninstalled: Loads modules from the build directory (for debugging purposes).
 *
 * Create a new daemon object for exporting objects on @connection.
 *
 * Returns: A #UDisksDaemon object. Free with g_object_unref().
 */
UDisksDaemon *
udisks_daemon_new (GDBusConnection *connection,
                   gboolean         disable_modules,
                   gboolean         force_load_modules,
                   gboolean         uninstalled)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  return UDISKS_DAEMON (g_object_new (UDISKS_TYPE_DAEMON,
                                      "connection", connection,
                                      "disable-modules", disable_modules,
                                      "force-load-modules", force_load_modules,
                                      "uninstalled", uninstalled,
                                      NULL));
}

/**
 * udisks_daemon_get_connection:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the D-Bus connection used by @daemon.
 *
 * Returns: A #GDBusConnection. Do not free, the object is owned by @daemon.
 */
GDBusConnection *
udisks_daemon_get_connection (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->connection;
}

/**
 * udisks_daemon_get_object_manager:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the D-Bus object manager used by @daemon.
 *
 * Returns: A #GDBusObjectManagerServer. Do not free, the object is owned by @daemon.
 */
GDBusObjectManagerServer *
udisks_daemon_get_object_manager (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->object_manager;
}

/**
 * udisks_daemon_get_mount_monitor:
 * @daemon: A #UDisksDaemon
 *
 * Gets the mount monitor used by @daemon.
 *
 * Returns: A #UDisksMountMonitor. Do not free, the object is owned by @daemon.
 */
UDisksMountMonitor *
udisks_daemon_get_mount_monitor (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->mount_monitor;
}

/**
 * udisks_daemon_get_fstab_monitor:
 * @daemon: A #UDisksDaemon
 *
 * Gets the fstab monitor used by @daemon.
 *
 * Returns: A #UDisksFstabMonitor. Do not free, the object is owned by @daemon.
 */
UDisksFstabMonitor *
udisks_daemon_get_fstab_monitor (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->fstab_monitor;
}

/**
 * udisks_daemon_get_crypttab_monitor:
 * @daemon: A #UDisksDaemon
 *
 * Gets the crypttab monitor used by @daemon.
 *
 * Returns: A #UDisksCrypttabMonitor. Do not free, the object is owned by @daemon.
 */
UDisksCrypttabMonitor *
udisks_daemon_get_crypttab_monitor (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->crypttab_monitor;
}

/**
 * udisks_daemon_get_linux_provider:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the Linux Provider, if any.
 *
 * Returns: A #UDisksLinuxProvider or %NULL. Do not free, the object is owned by @daemon.
 */
UDisksLinuxProvider *
udisks_daemon_get_linux_provider (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->linux_provider;
}

/**
 * udisks_daemon_get_authority:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the PolicyKit authority used by @daemon.
 *
 * Returns: A #PolkitAuthority instance or %NULL if the polkit
 * authority is not available. Do not free, the object is owned by
 * @daemon.
 */
PolkitAuthority *
udisks_daemon_get_authority (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->authority;
}

/**
 * udisks_daemon_get_state:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the state object used by @daemon.
 *
 * Returns: A #UDisksState instance. Do not free, the object is owned by @daemon.
 */
UDisksState *
udisks_daemon_get_state (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->state;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_job_completed (UDisksJob    *job,
                  gboolean      success,
                  const gchar  *message,
                  gpointer      user_data)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (user_data);
  UDisksObjectSkeleton *object;

  object = UDISKS_OBJECT_SKELETON (g_dbus_interface_get_object (G_DBUS_INTERFACE (job)));
  g_assert (object != NULL);

  /* Unexport job */
  g_dbus_object_manager_server_unexport (daemon->object_manager,
                                         g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  g_object_unref (object);

  /* free the allocated job object */
  g_object_unref (job);

  /* returns the reference we took when connecting to the
   * UDisksJob::completed signal in udisks_daemon_launch_{spawned,threaded}_job()
   * below
   */
  g_object_unref (daemon);
}

/* ---------------------------------------------------------------------------------------------------- */

static guint job_id = 0;

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_launch_simple_job:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 *
 * Launches a new simple job.
 *
 * The job is started immediately - When the job is done, call
 * udisks_simple_job_complete() on the returned object. Long-running
 * jobs should periodically check @cancellable to see if they have
 * been cancelled.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * Returns: A #UDisksSimpleJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_simple_job (UDisksDaemon    *daemon,
                                 UDisksObject    *object,
                                 const gchar     *job_operation,
                                 uid_t            job_started_by_uid,
                                 GCancellable    *cancellable)
{
  UDisksSimpleJob *job;
  UDisksObjectSkeleton *job_object;
  gchar *job_object_path;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);

  job = udisks_simple_job_new (daemon, cancellable);
  if (object != NULL)
    udisks_base_job_add_object (UDISKS_BASE_JOB (job), object);

  job_object_path = g_strdup_printf ("/org/freedesktop/UDisks2/jobs/%u", g_atomic_int_add (&job_id, 1));
  job_object = udisks_object_skeleton_new (job_object_path);
  udisks_object_skeleton_set_job (job_object, UDISKS_JOB (job));
  g_free (job_object_path);

  udisks_job_set_cancelable (UDISKS_JOB (job), TRUE);
  udisks_job_set_operation (UDISKS_JOB (job), job_operation);
  udisks_job_set_started_by_uid (UDISKS_JOB (job), job_started_by_uid);

  g_dbus_object_manager_server_export (daemon->object_manager, G_DBUS_OBJECT_SKELETON (job_object));
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (on_job_completed),
                          g_object_ref (daemon));

  return UDISKS_BASE_JOB (job);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_launch_threaded_job:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @job_func: The function to run in another thread.
 * @user_data: User data to pass to @job_func.
 * @user_data_free_func: Function to free @user_data with or %NULL.
 * @cancellable: A #GCancellable or %NULL.
 *
 * Launches a new job by running @job_func in a new dedicated thread.
 *
 * The job is started immediately - connect to the
 * #UDisksThreadedJob::threaded-job-completed or #UDisksJob::completed
 * signals to get notified when the job is done.
 *
 * Long-running jobs should periodically check @cancellable to see if
 * they have been cancelled.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * Returns: A #UDisksThreadedJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_threaded_job  (UDisksDaemon          *daemon,
                                    UDisksObject          *object,
                                    const gchar           *job_operation,
                                    uid_t                  job_started_by_uid,
                                    UDisksThreadedJobFunc  job_func,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free_func,
                                    GCancellable          *cancellable)
{
  UDisksThreadedJob *job;
  UDisksObjectSkeleton *job_object;
  gchar *job_object_path;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (job_func != NULL, NULL);

  job = udisks_threaded_job_new (job_func,
                                 user_data,
                                 user_data_free_func,
                                 daemon,
                                 cancellable);
  if (object != NULL)
    udisks_base_job_add_object (UDISKS_BASE_JOB (job), object);

  job_object_path = g_strdup_printf ("/org/freedesktop/UDisks2/jobs/%u", g_atomic_int_add (&job_id, 1));
  job_object = udisks_object_skeleton_new (job_object_path);
  udisks_object_skeleton_set_job (job_object, UDISKS_JOB (job));
  g_free (job_object_path);

  udisks_job_set_cancelable (UDISKS_JOB (job), TRUE);
  udisks_job_set_operation (UDISKS_JOB (job), job_operation);
  udisks_job_set_started_by_uid (UDISKS_JOB (job), job_started_by_uid);

  g_dbus_object_manager_server_export (daemon->object_manager, G_DBUS_OBJECT_SKELETON (job_object));
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (on_job_completed),
                          g_object_ref (daemon));

  return UDISKS_BASE_JOB (job);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_launch_spawned_job:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Launches a new job for @command_line_format.
 *
 * The job is started immediately - connect to the
 * #UDisksSpawnedJob::spawned-job-completed or #UDisksJob::completed
 * signals to get notified when the job is done.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * Returns: A #UDisksSpawnedJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_spawned_job (UDisksDaemon    *daemon,
                                  UDisksObject    *object,
                                  const gchar     *job_operation,
                                  uid_t            job_started_by_uid,
                                  GCancellable    *cancellable,
                                  uid_t            run_as_uid,
                                  uid_t            run_as_euid,
                                  const gchar     *input_string,
                                  const gchar     *command_line_format,
                                  ...)
{
  va_list var_args;
  gchar *command_line;
  GString *input_string_as_gstring = NULL;
  UDisksBaseJob *job;

  if (input_string != NULL)
    input_string_as_gstring = g_string_new (input_string);

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);

  job = udisks_daemon_launch_spawned_job_gstring (daemon,
                                          object,
                                          job_operation,
                                          job_started_by_uid,
                                          cancellable,
                                          run_as_uid,
                                          run_as_euid,
                                          input_string_as_gstring,
                                          "%s",
                                          command_line);

  udisks_string_wipe_and_free (input_string_as_gstring);
  return job;
}

/**
 * udisks_daemon_launch_spawned_job_gstring:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Launches a new job for @command_line_format.
 *
 * The job is started immediately - connect to the
 * #UDisksSpawnedJob::spawned-job-completed or #UDisksJob::completed
 * signals to get notified when the job is done.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * This function is the same as udisks_daemon_launch_spawned_job, with
 * the only difference that it takes a GString and is therefore able to
 * handle binary inputs that contain NUL bytes.
 *
 * Returns: A #UDisksSpawnedJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_spawned_job_gstring (
                                  UDisksDaemon    *daemon,
                                  UDisksObject    *object,
                                  const gchar     *job_operation,
                                  uid_t            job_started_by_uid,
                                  GCancellable    *cancellable,
                                  uid_t            run_as_uid,
                                  uid_t            run_as_euid,
                                  GString         *input_string,
                                  const gchar     *command_line_format,
                                  ...)
{
  va_list var_args;
  gchar *command_line;
  UDisksSpawnedJob *job;
  UDisksObjectSkeleton *job_object;
  gchar *job_object_path;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (command_line_format != NULL, NULL);

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);
  job = udisks_spawned_job_new (command_line, input_string, run_as_uid, run_as_euid, daemon, cancellable);
  g_free (command_line);

  if (object != NULL)
    udisks_base_job_add_object (UDISKS_BASE_JOB (job), object);

  job_object_path = g_strdup_printf ("/org/freedesktop/UDisks2/jobs/%u", g_atomic_int_add (&job_id, 1));
  job_object = udisks_object_skeleton_new (job_object_path);
  udisks_object_skeleton_set_job (job_object, UDISKS_JOB (job));
  g_free (job_object_path);

  udisks_job_set_cancelable (UDISKS_JOB (job), TRUE);
  udisks_job_set_operation (UDISKS_JOB (job), job_operation);
  udisks_job_set_started_by_uid (UDISKS_JOB (job), job_started_by_uid);

  g_dbus_object_manager_server_export (daemon->object_manager, G_DBUS_OBJECT_SKELETON (job_object));
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (on_job_completed),
                          g_object_ref (daemon));

  return UDISKS_BASE_JOB (job);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GMainContext *context;
  GMainLoop *loop;
  gboolean success;
  gint status;
  gchar *message;
} SpawnedJobSyncData;

static gboolean
spawned_job_sync_on_spawned_job_completed (UDisksSpawnedJob *job,
                                           GError           *error,
                                           gint              status,
                                           GString          *standard_output,
                                           GString          *standard_error,
                                           gpointer          user_data)
{
  SpawnedJobSyncData *data = user_data;
  data->status = status;
  return FALSE; /* let other handlers run */
}

static void
spawned_job_sync_on_completed (UDisksJob    *job,
                               gboolean      success,
                               const gchar  *message,
                               gpointer      user_data)
{
  SpawnedJobSyncData *data = user_data;
  data->success = success;
  data->message = g_strdup (message);
  g_main_loop_quit (data->loop);
}

/**
 * udisks_daemon_launch_spawned_job_sync:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @out_status: Return location for the @status parameter of the #UDisksSpawnedJob::spawned-job-completed signal.
 * @out_message: Return location for the @message parameter of the #UDisksJob::completed signal.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Like udisks_daemon_launch_spawned_job() but blocks the calling
 * thread until the job completes.
 *
 * Returns: The @success parameter of the #UDisksJob::completed signal.
 */
gboolean
udisks_daemon_launch_spawned_job_sync (UDisksDaemon    *daemon,
                                       UDisksObject    *object,
                                       const gchar     *job_operation,
                                       uid_t            job_started_by_uid,
                                       GCancellable    *cancellable,
                                       uid_t            run_as_uid,
                                       uid_t            run_as_euid,
                                       gint            *out_status,
                                       gchar          **out_message,
                                       const gchar     *input_string,
                                       const gchar     *command_line_format,
                                       ...)
{
  va_list var_args;
  gchar *command_line;
  GString *input_string_as_gstring = NULL;
  gboolean ret;

  if (input_string != NULL)
    input_string_as_gstring = g_string_new (input_string);

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);

  ret = udisks_daemon_launch_spawned_job_gstring_sync (daemon,
                                          object,
                                          job_operation,
                                          job_started_by_uid,
                                          cancellable,
                                          run_as_uid,
                                          run_as_euid,
                                          out_status,
                                          out_message,
                                          input_string_as_gstring,
                                          "%s",
                                          command_line);

  udisks_string_wipe_and_free (input_string_as_gstring);
  return ret;
}

/**
 * udisks_daemon_launch_spawned_job_gstring_sync:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @out_status: Return location for the @status parameter of the #UDisksSpawnedJob::spawned-job-completed signal.
 * @out_message: Return location for the @message parameter of the #UDisksJob::completed signal.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Like udisks_daemon_launch_spawned_job() but blocks the calling
 * thread until the job completes.
 *
 * This function is the same as udisks_daemon_launch_spawned_job_sync, with
 * the only difference that it takes a GString and is therefore able to
 * handle binary inputs that contain NUL bytes.
 *
 * Returns: The @success parameter of the #UDisksJob::completed signal.
 */
gboolean
udisks_daemon_launch_spawned_job_gstring_sync (UDisksDaemon    *daemon,
                                       UDisksObject    *object,
                                       const gchar     *job_operation,
                                       uid_t            job_started_by_uid,
                                       GCancellable    *cancellable,
                                       uid_t            run_as_uid,
                                       uid_t            run_as_euid,
                                       gint            *out_status,
                                       gchar          **out_message,
                                       GString         *input_string,
                                       const gchar     *command_line_format,
                                       ...)
{
  va_list var_args;
  gchar *command_line;
  UDisksBaseJob *job;
  SpawnedJobSyncData data;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
  g_return_val_if_fail (command_line_format != NULL, FALSE);

  data.context = g_main_context_new ();
  g_main_context_push_thread_default (data.context);
  data.loop = g_main_loop_new (data.context, FALSE);
  data.success = FALSE;
  data.status = 0;
  data.message = NULL;

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);
  job = udisks_daemon_launch_spawned_job_gstring (daemon,
                                          object,
                                          job_operation,
                                          job_started_by_uid,
                                          cancellable,
                                          run_as_uid,
                                          run_as_euid,
                                          input_string,
                                          "%s",
                                          command_line);
  g_signal_connect (job,
                    "spawned-job-completed",
                    G_CALLBACK (spawned_job_sync_on_spawned_job_completed),
                    &data);
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (spawned_job_sync_on_completed),
                          &data);

  udisks_spawned_job_start (UDISKS_SPAWNED_JOB (job));
  g_main_loop_run (data.loop);

  if (out_status != NULL)
    *out_status = data.status;

  if (out_message != NULL)
    *out_message = data.message;
  else
    g_free (data.message);

  g_free (command_line);
  g_main_loop_unref (data.loop);
  g_main_context_pop_thread_default (data.context);
  g_main_context_unref (data.context);

  /* note: the job object is freed in the ::completed handler */

  return data.success;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GMainContext *context;
  GMainLoop    *loop;
  gboolean      result;
  GError       *error;
} ThreadedJobSyncData;

static gboolean
threaded_job_sync_on_threaded_job_completed (UDisksThreadedJob *job,
                                             gboolean           result,
                                             GError            *error,
                                             gpointer           user_data)
{
  ThreadedJobSyncData *data = user_data;
  data->result = result;
  if (error)
    data->error = g_error_copy (error);
  return FALSE; /* let other handlers run */
}

static void
threaded_job_sync_on_completed (UDisksJob    *job,
                                gboolean      success,
                                const gchar  *message,
                                gpointer      user_data)
{
  ThreadedJobSyncData *data = user_data;
  g_main_loop_quit (data->loop);
}

/**
 * udisks_daemon_launch_threaded_job_sync:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @job_func: The function to run in another thread.
 * @user_data: User data to pass to @job_func.
 * @user_data_free_func: Function to free @user_data with or %NULL.
 * @cancellable: A #GCancellable or %NULL.
 * @error: The #GError set by the #UDisksThreadedJobFunc.
 *
 * Like udisks_daemon_launch_threaded_job() but blocks the calling
 * thread until the job completes.
 *
 * Returns: The @success parameter of the #UDisksJob::completed signal.
 */
gboolean
udisks_daemon_launch_threaded_job_sync (UDisksDaemon          *daemon,
                                        UDisksObject          *object,
                                        const gchar           *job_operation,
                                        uid_t                  job_started_by_uid,
                                        UDisksThreadedJobFunc  job_func,
                                        gpointer               user_data,
                                        GDestroyNotify         user_data_free_func,
                                        GCancellable          *cancellable,
                                        GError               **error)
{
  UDisksBaseJob *job;
  ThreadedJobSyncData data;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);

  data.context = g_main_context_new ();
  g_main_context_push_thread_default (data.context);
  data.loop = g_main_loop_new (data.context, FALSE);
  data.error = NULL;
  data.result = FALSE;

  job = udisks_daemon_launch_threaded_job (daemon,
                                           object,
                                           job_operation,
                                           job_started_by_uid,
                                           job_func,
                                           user_data,
                                           user_data_free_func,
                                           cancellable);
  g_signal_connect (job,
                    "threaded-job-completed",
                    G_CALLBACK (threaded_job_sync_on_threaded_job_completed),
                    &data);
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (threaded_job_sync_on_completed),
                          &data);

  udisks_threaded_job_start (UDISKS_THREADED_JOB (job));
  g_main_loop_run (data.loop);

  g_main_loop_unref (data.loop);
  g_main_context_pop_thread_default (data.context);
  g_main_context_unref (data.context);

  if (data.error)
    g_propagate_error (error, data.error);

  /* note: the job object is freed in the ::completed handler */
  return data.result;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_wait_for_object_sync:
 * @daemon: A #UDisksDaemon.
 * @wait_func: Function to check for desired object.
 * @user_data: User data to pass to @wait_func.
 * @user_data_free_func: (allow-none): Function to free @user_data or %NULL.
 * @timeout_seconds: Maximum time to wait for the object (in seconds) or 0 to never wait.
 * @error: (allow-none): Return location for error or %NULL.
 *
 * Blocks the calling thread until an object picked by @wait_func is
 * available or until @timeout_seconds has passed (in which case the
 * function fails with %UDISKS_ERROR_TIMED_OUT).
 *
 * Note that @wait_func will be called from time to time - for example
 * if there is a device event.
 *
 * Returns: (transfer full): The object picked by @wait_func or %NULL if @error is set.
 */

typedef struct {
  GMainContext *context;
  GMainLoop *loop;
  gboolean timed_out;
} WaitData;

static gboolean
wait_on_timed_out (gpointer user_data)
{
  WaitData *data = user_data;
  data->timed_out = TRUE;
  g_main_loop_quit (data->loop);
  return FALSE; /* remove the source */
}

static gboolean
wait_on_recheck (gpointer user_data)
{
  WaitData *data = user_data;
  g_main_loop_quit (data->loop);
  return FALSE; /* remove the source */
}

UDisksObject *
udisks_daemon_wait_for_object_sync (UDisksDaemon          *daemon,
                                    UDisksDaemonWaitFunc   wait_func,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free_func,
                                    guint                  timeout_seconds,
                                    GError               **error)
{
  UDisksObject *ret;
  WaitData data;

  /* TODO: support GCancellable */

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (wait_func != NULL, NULL);

  ret = NULL;

  memset (&data, '\0', sizeof (data));
  data.context = NULL;
  data.loop = NULL;

  g_object_ref (daemon);

 again:
  ret = wait_func (daemon, user_data);

  if (ret == NULL && timeout_seconds > 0)
    {
      GSource *source;

      /* sit and wait for up to @timeout_seconds if the object isn't there already */
      if (data.context == NULL)
        {
          /* TODO: this will deadlock if we are calling from the main thread... */
          data.context = g_main_context_new ();
          data.loop = g_main_loop_new (data.context, FALSE);

          source = g_timeout_source_new_seconds (timeout_seconds);
          g_source_set_priority (source, G_PRIORITY_DEFAULT);
          g_source_set_callback (source, wait_on_timed_out, &data, NULL);
          g_source_attach (source, data.context);
          g_source_unref (source);
        }

      /* TODO: do something a bit more elegant than checking every 250ms ... it's
       *       probably going to involve having each UDisksProvider emit a "changed"
       *       signal when it's time to recheck... for now this works.
       */
      source = g_timeout_source_new (250);
      g_source_set_priority (source, G_PRIORITY_DEFAULT);
      g_source_set_callback (source, wait_on_recheck, &data, NULL);
      g_source_attach (source, data.context);
      g_source_unref (source);

      g_main_loop_run (data.loop);

      if (data.timed_out)
        {
          g_set_error (error,
                       UDISKS_ERROR, UDISKS_ERROR_FAILED,
                       "Timed out waiting for object");
        }
      else
        {
          goto again;
        }
    }

  if (user_data_free_func != NULL)
    user_data_free_func (user_data);

  g_object_unref (daemon);

  if (data.loop != NULL)
    g_main_loop_unref (data.loop);
  if (data.context != NULL)
    g_main_context_unref (data.context);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_block:
 * @daemon: A #UDisksDaemon.
 * @block_device_number: A #dev_t with the device number to find.
 *
 * Finds a block device with the number given by @block_device_number.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_block (UDisksDaemon *daemon,
                          dev_t         block_device_number)
{
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_peek_block (object);
      if (block == NULL)
        continue;

      if (udisks_block_get_device_number (block) == block_device_number)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }
 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_block_by_device_file:
 * @daemon: A #UDisksDaemon.
 * @device_file: A device file.
 *
 * Finds a block device with device file given by @device_file.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_block_by_device_file (UDisksDaemon *daemon,
                                         const gchar  *device_file)
{
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_peek_block (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_get_device (block), device_file) == 0)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }
 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_block_by_sysfs_path:
 * @daemon: A #UDisksDaemon.
 * @sysfs_path: A sysfs path.
 *
 * Finds a block device with a sysfs path given by @sysfs_path.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_block_by_sysfs_path (UDisksDaemon *daemon,
                                        const gchar  *sysfs_path)
{
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksLinuxDevice *device;

      if (!UDISKS_IS_LINUX_BLOCK_OBJECT (object))
        continue;

      device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (object));
      if (device == NULL)
        continue;

      if (g_strcmp0 (g_udev_device_get_sysfs_path (device->udev_device), sysfs_path) == 0)
        {
          g_object_unref (device);
          ret = g_object_ref (object);
          goto out;
        }
      g_object_unref (device);
    }
 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_object:
 * @daemon: A #UDisksDaemon.
 * @object_path: A #dev_t with the device number to find.
 *
 * Finds an exported object with the object path given by @object_path.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_object (UDisksDaemon *daemon,
                           const gchar  *object_path)
{
  return (UDisksObject *) g_dbus_object_manager_get_object (G_DBUS_OBJECT_MANAGER (daemon->object_manager),
                                                            object_path);
}

/**
 * udisks_daemon_get_objects:
 * @daemon: A #UDisksDaemon.
 *
 * Gets all D-Bus objects exported by @daemon.
 *
 * Returns: (transfer full) (element-type UDisksObject): A list of #UDisksObject instaces. The returned list should be freed with g_list_free() after each element has been freed with g_object_unref().
 */
GList *
udisks_daemon_get_objects (UDisksDaemon *daemon)
{
  return g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
}

/**
 * udisks_daemon_get_module_manager:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the module manager used by @daemon.
 *
 * Returns: A #UDisksModuleManager. Do not free, the object is owned by @daemon.
 */
UDisksModuleManager *
udisks_daemon_get_module_manager (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->module_manager;
}

/**
 * udisks_daemon_get_config_manager:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the config manager used by @daemon.
 *
 * Returns: A #ConfigModuleManager. Do not free, the object is owned by @daemon.
 */
UDisksConfigManager *
udisks_daemon_get_config_manager (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->config_manager;
}

/**
 * udisks_daemon_get_disable_modules:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether modules should never be loaded.
 *
 * Returns: %TRUE if --disable-modules commandline switch has been specified.
 */
gboolean
udisks_daemon_get_disable_modules (UDisksDaemon*daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->disable_modules;
}

/**
 * udisks_daemon_get_force_load_modules:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether modules should be activated upon start.
 *
 * Returns: %TRUE if --force-load-modules commandline switch has been specified.
 */
gboolean
udisks_daemon_get_force_load_modules (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->force_load_modules;
}

/**
 * udisks_daemon_get_uninstalled:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether the modules should be loaded from the build
 * directory.
 *
 * Returns: %TRUE if --uninstalled commandline switch has been specified.
 */
gboolean
udisks_daemon_get_uninstalled (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->uninstalled;
}

/* ---------------------------------------------------------------------------------------------------- */

gchar *
udisks_daemon_get_parent_for_tracking (UDisksDaemon  *daemon,
                                       const gchar   *path,
                                       gchar        **uuid_ret)
{
  const gchar *parent_path = NULL;
  const gchar *parent_uuid = NULL;

  UDisksObject *object = NULL;
  UDisksObject *crypto_object = NULL;
  UDisksObject *mdraid_object = NULL;
  UDisksObject *table_object = NULL;
  GList *track_parent_funcs;

  UDisksBlock *block;
  UDisksBlock *crypto_block;
  UDisksMDRaid *mdraid;
  UDisksPartition *partition;
  UDisksBlock *table_block;

  object = udisks_daemon_find_object (daemon, path);
  if (object == NULL)
    goto out;

  block = udisks_object_peek_block (object);
  if (block)
    {
      crypto_object = udisks_daemon_find_object (daemon, udisks_block_get_crypto_backing_device (block));
      if (crypto_object)
        {
          crypto_block = udisks_object_peek_block (crypto_object);
          if (crypto_block)
            {
              parent_uuid = udisks_block_get_id_uuid (crypto_block);
              parent_path = udisks_block_get_crypto_backing_device (block);
              goto out;
            }
        }

      mdraid_object = udisks_daemon_find_object (daemon, udisks_block_get_mdraid (block));
      if (mdraid_object)
        {
          mdraid = udisks_object_peek_mdraid (mdraid_object);
          if (mdraid)
            {
              parent_uuid = udisks_mdraid_get_uuid (mdraid);
              parent_path = udisks_block_get_mdraid (block);
              goto out;
            }
        }

      partition = udisks_object_peek_partition (object);
      if (partition)
        {
          table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
          if (table_object)
            {
              table_block = udisks_object_peek_block (table_object);
              if (table_block)
                {
                  /* We don't want to track partition tables because
                     they can't be 'closed' in a way that their
                     children temporarily invisible.
                  */
                  parent_uuid = NULL;
                  parent_path = udisks_partition_get_table (partition);
                  goto out;
                }
            }
        }
    }

 out:
  g_clear_object (&object);
  g_clear_object (&crypto_object);
  g_clear_object (&mdraid_object);
  g_clear_object (&table_object);

  if (parent_path)
    {
      if (uuid_ret)
        *uuid_ret = g_strdup (parent_uuid);
      return g_strdup (parent_path);
    }

  track_parent_funcs = udisks_module_manager_get_track_parent_funcs (daemon->module_manager);
  while (track_parent_funcs)
    {
      UDisksTrackParentFunc func = track_parent_funcs->data;
      gchar *path_ret = func (daemon, path, uuid_ret);
      if (path_ret)
        return path_ret;

      track_parent_funcs = track_parent_funcs->next;
    }

  return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */
