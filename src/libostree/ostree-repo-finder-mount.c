/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libglnx/glnx-dirfd.h>
#include <libglnx/glnx-errors.h>
#include <libglnx/glnx-fdio.h>
#include <libglnx/glnx-local-alloc.h>
#include <stdlib.h>

#include "ostree-autocleanups.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-mount.h"

/* TODO: Check if this can sensibly represent apps and the OS from a whole-OS
 * OSTree on a USB stick. */

/**
 * SECTION:ostree-repo-finder-mount
 * @title: OstreeRepoFinderMount
 * @short_description: Finds remote repositories from ref names by looking at
 *    mounted removable volumes
 * @stability: Unstable
 * @include: libostree/ostree-repo-finder-mount.h
 *
 * #OstreeRepoFinderMount is an implementation of #OstreeRepoFinder which looks
 * refs up in well-known locations on any mounted removable volumes.
 *
 * For a ref, `R`, it checks whether `.ostree/repos/R` exists and is an OSTree
 * repository on each mounted removable volume. Ref names are not escaped when
 * building the path, so if a ref contains `/` in its name, the repository will
 * be checked for in a subdirectory of `.ostree/repos`. Non-removable volumes
 * are ignored.
 *
 * Symlinks are followed when resolving the refs, so a volume might contain a
 * single OSTree at some arbitrary path, with a number of refs linking to it
 * from `.ostree/repos`. Any symlink which points outside the volume’s file
 * system will be ignored. Repositories are deduplicated in the results.
 *
 * The volume monitor used to find mounted volumes can be overridden by setting
 * #OstreeRepoFinderMount:monitor. By default, g_volume_monitor_get() is used.
 *
 * Since: 2017.6
 */

typedef GList/*<owned GObject>*/ ObjectList;

static void
object_list_free (ObjectList *list)
{
  g_list_free_full (list, g_object_unref);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ObjectList, object_list_free)

static void ostree_repo_finder_mount_iface_init (OstreeRepoFinderInterface *iface);

struct _OstreeRepoFinderMount
{
  GObject parent_instance;

  GVolumeMonitor *monitor;  /* owned */
};

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderMount, ostree_repo_finder_mount, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_mount_iface_init))

static void
ostree_repo_finder_mount_resolve_async (OstreeRepoFinder    *finder,
                                        const gchar * const *refs,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (finder);
  g_autoptr(GTask) task = NULL;
  g_autoptr(ObjectList) volumes = NULL;
  g_autoptr(GPtrArray) results = NULL;
  GList *l;
  const gint priority = 50;  /* arbitrarily chosen */

  task = g_task_new (finder, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_mount_resolve_async);

  volumes = g_volume_monitor_get_volumes (self->monitor);
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);

  for (l = volumes; l != NULL; l = l->next)
    {
      GVolume *volume = G_VOLUME (l->data);
      g_autoptr(GDrive) drive = NULL;
      g_autoptr(GMount) mount = NULL;
      g_autofree gchar *volume_name = NULL;
      g_autoptr(GFile) mount_root = NULL;
      g_autofree gchar *mount_root_path = NULL;
      glnx_fd_close int mount_root_dfd = -1;
      struct stat mount_root_stbuf;
      glnx_fd_close int repos_dfd = -1;
      gsize i;
      g_autoptr(GHashTable) repo_uri_to_refs = NULL;  /* (element-type utf8 GPtrArray) */
      GPtrArray *supported_refs;  /* (element-type utf8) */
      GHashTableIter iter;
      const gchar *repo_uri;
      g_autoptr(GError) local_error = NULL;

      drive = g_volume_get_drive (volume);
      mount = g_volume_get_mount (volume);
      volume_name = g_volume_get_name (volume);

      /* Check the drive’s general properties. */
      if (drive == NULL || mount == NULL)
        {
          g_debug ("Ignoring volume ‘%s’ due to NULL drive or mount.",
                   volume_name);
          continue;
        }

#if GLIB_CHECK_VERSION(2, 50, 0)
      if (!g_drive_is_removable (drive))
        {
          g_debug ("Ignoring volume ‘%s’ as drive is not removable.",
                   volume_name);
          continue;
        }
#endif

      /* Check if it contains a .ostree/repos directory. */
      mount_root = g_mount_get_root (mount);
      mount_root_path = g_file_get_path (mount_root);

      if (!glnx_opendirat (AT_FDCWD, mount_root_path, TRUE, &mount_root_dfd, &local_error))
        {
          g_debug ("Ignoring volume ‘%s’ as ‘%s’ directory can’t be opened: %s",
                   volume_name, mount_root_path, local_error->message);
          continue;
        }

      if (!glnx_opendirat (mount_root_dfd, ".ostree/repos", TRUE, &repos_dfd, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            g_debug ("Ignoring volume ‘%s’ as ‘%s/.ostree/repos’ directory doesn’t exist.",
                     volume_name, mount_root_path);
          else
            g_debug ("Ignoring volume ‘%s’ as ‘%s/.ostree/repos’ directory can’t be opened: %s",
                     volume_name, mount_root_path, local_error->message);

          continue;
        }

      /* stat() the mount root so we can later check whether the resolved
       * repositories for individual refs are on the same device (to avoid the
       * symlinks for them pointing outside the mount root). */
      if (!glnx_fstat (mount_root_dfd, &mount_root_stbuf, &local_error))
        {
          g_debug ("Ignoring volume ‘%s’ as querying info of ‘%s’ failed: %s",
                   volume_name, mount_root_path, local_error->message);
          continue;
        }

      /* Check whether a subdirectory exists for any of the @refs we’re looking
       * for. If so, and it’s a symbolic link, dereference it so multiple links
       * to the same repository (containing multiple refs) are coalesced.
       * Otherwise, include it as a result by itself. */
      repo_uri_to_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);

      for (i = 0; refs[i] != NULL; i++)
        {
          struct stat stbuf;
          g_autofree gchar *repo_dir_path = NULL;
          g_autofree char *canonical_repo_dir_path = NULL;
          g_autofree gchar *resolved_repo_uri = NULL;

          repo_dir_path = g_build_filename (mount_root_path, ".ostree", "repos", refs[i], NULL);

          if (!glnx_fstatat (repos_dfd, refs[i], &stbuf, AT_NO_AUTOMOUNT, &local_error))
            {
              g_debug ("Ignoring ref ‘%s’ on volume ‘%s’ as querying info of ‘%s’ failed: %s",
                       refs[i], volume_name, repo_dir_path, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          if ((stbuf.st_mode & S_IFMT) != S_IFDIR)
            {
              g_debug ("Ignoring ref ‘%s’ on volume ‘%s’ as ‘%s’ is of type %u, not a directory.",
                       refs[i], volume_name, repo_dir_path, (stbuf.st_mode & S_IFMT));
              continue;
            }

          /* Check the resolved repository path is below the mount point. Do not
           * allow ref symlinks to point somewhere outside of the mounted
           * volume. */
          if (stbuf.st_dev != mount_root_stbuf.st_dev)
            {
              g_debug ("Ignoring ref ‘%s’ on volume ‘%s’ as it’s on a different file system from the mount.",
                       refs[i], volume_name);
              continue;
            }

          /* There is a valid repo at (or pointed to by)
           * $mount_root/.ostree/repos/$refs[i]. Add it to the results, keyed by
           * the canonicalised repository URI to deduplicate the results. */
          canonical_repo_dir_path = realpath (repo_dir_path, NULL);
          resolved_repo_uri = g_strconcat ("file://", canonical_repo_dir_path, NULL);
          g_debug ("Resolved ref ‘%s’ on volume ‘%s’ to repo URI ‘%s’.",
                   refs[i], volume_name, resolved_repo_uri);

          supported_refs = g_hash_table_lookup (repo_uri_to_refs, resolved_repo_uri);

          if (supported_refs == NULL)
            {
              supported_refs = g_ptr_array_new_with_free_func (NULL);
              g_hash_table_insert (repo_uri_to_refs, g_steal_pointer (&resolved_repo_uri), supported_refs  /* transfer */);
            }

          g_ptr_array_add (supported_refs, (gpointer) refs[i]);
        }

      /* Aggregate the results. */
      g_hash_table_iter_init (&iter, repo_uri_to_refs);

      while (g_hash_table_iter_next (&iter, (gpointer *) &repo_uri, (gpointer *) &supported_refs))
        {
          g_autoptr(OstreeRemote) remote = NULL;

          g_ptr_array_add (supported_refs, NULL);  /* NULL terminator */

          remote = ostree_remote_new ();
          remote->name = g_strdup (repo_uri);
          remote->group = g_strdup_printf ("remote \"%s\"", remote->name);
          remote->keyring = NULL;
          remote->file = NULL;
          remote->options = g_key_file_new ();

          g_key_file_set_string (remote->options, remote->group, "url", repo_uri);
          g_key_file_set_boolean (remote->options, remote->group, "gpg-verify", TRUE);
          g_key_file_set_boolean (remote->options, remote->group, "gpg-verify-summary", TRUE);

          /* Set the timestamp in the #OstreeRepoFinderResult to 0 because
           * the code in ostree_repo_pull_from_remotes_async() will be able to
           * check it just as quickly as we can here; so don’t duplicate the
           * code. */
          g_ptr_array_add (results, ostree_repo_finder_result_new (remote, priority, (const gchar * const *) supported_refs->pdata, 0));
        }
    }

  g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

static GPtrArray *
ostree_repo_finder_mount_resolve_finish (OstreeRepoFinder  *self,
                                         GAsyncResult      *result,
                                         GError           **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_repo_finder_mount_init (OstreeRepoFinderMount *self)
{
  /* Nothing to see here. */
}

static void
ostree_repo_finder_mount_constructed (GObject *object)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  G_OBJECT_CLASS (ostree_repo_finder_mount_parent_class)->constructed (object);

  if (self->monitor == NULL)
    self->monitor = g_volume_monitor_get ();
}

typedef enum
{
  PROP_MONITOR = 1,
} OstreeRepoFinderMountProperty;

static void
ostree_repo_finder_mount_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  switch ((OstreeRepoFinderMountProperty) property_id)
    {
    case PROP_MONITOR:
      g_value_set_object (value, self->monitor);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
ostree_repo_finder_mount_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  switch ((OstreeRepoFinderMountProperty) property_id)
    {
    case PROP_MONITOR:
      /* Construct-only. */
      g_assert (self->monitor == NULL);
      self->monitor = g_value_dup_object (value);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
ostree_repo_finder_mount_dispose (GObject *object)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (ostree_repo_finder_mount_parent_class)->dispose (object);
}

static void
ostree_repo_finder_mount_class_init (OstreeRepoFinderMountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = ostree_repo_finder_mount_get_property;
  object_class->set_property = ostree_repo_finder_mount_set_property;
  object_class->constructed = ostree_repo_finder_mount_constructed;
  object_class->dispose = ostree_repo_finder_mount_dispose;

  /**
   * OstreeRepoFinderMount:monitor:
   *
   * Volume monitor to use to look up mounted volumes when queried.
   *
   * Since: 2017.6
   */
  g_object_class_install_property (object_class, PROP_MONITOR,
                                   g_param_spec_object ("monitor",
                                                        "Volume Monitor",
                                                        "Volume monitor to use "
                                                        "to look up mounted "
                                                        "volumes when queried.",
                                                        G_TYPE_VOLUME_MONITOR,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
ostree_repo_finder_mount_iface_init (OstreeRepoFinderInterface *iface)
{
  iface->resolve_async = ostree_repo_finder_mount_resolve_async;
  iface->resolve_finish = ostree_repo_finder_mount_resolve_finish;
}

/**
 * ostree_repo_finder_mount_new:
 * @monitor: (nullable) (transfer none): volume monitor to use, or %NULL to use
 *    the system default
 *
 * Create a new #OstreeRepoFinderMount, using the given @monitor to look up
 * volumes. If @monitor is %NULL, the monitor from g_volume_monitor_get() will
 * be used.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderMount
 * Since: 2017.6
 */
OstreeRepoFinderMount *
ostree_repo_finder_mount_new (GVolumeMonitor *monitor)
{
  g_return_val_if_fail (monitor == NULL || G_IS_VOLUME_MONITOR (monitor), NULL);

  return g_object_new (OSTREE_TYPE_REPO_FINDER_MOUNT,
                       "monitor", monitor,
                       NULL);
}
