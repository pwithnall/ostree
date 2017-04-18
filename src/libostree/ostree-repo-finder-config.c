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

#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libglnx/glnx-fdio.h>
#include <libglnx/glnx-local-alloc.h>

#include "ostree-remote-private.h"
#include "ostree-repo.h"
#include "ostree-repo-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-config.h"

/**
 * SECTION:ostree-repo-finder-config
 * @title: OstreeRepoFinderConfig
 * @short_description: Finds remote repositories from ref names using local
 *    configuration files
 * @stability: Unstable
 * @include: libostree/ostree-repo-finder-config.h
 *
 * #OstreeRepoFinderConfig is an implementation of #OstreeRepoFinder which looks
 * refs up in configuration files in `/etc` and returns remote URIs from there.
 * Duplicate remote URIs are combined into a single #OstreeRepoFinderResult
 * which lists multiple refs.
 *
 * For a ref, `R`, it tries to open `/etc/ostree/refs.d/R.conf`. If that file
 * exists, it returns a remote with the URI given in the `Remote.url` key in it.
 * The configuration files are #GKeyFiles with a `[Remote]` section. All other
 * sections and keys are currently ignored. Note that ref names are not escaped
 * when building the path, so if a ref contains `/` in its name, the `.conf`
 * file will be in a subdirectory.
 *
 * The `/etc/ostree/refs.d` path can be overridden by setting
 * #OstreeRepoFinderConfig:refs-directory.
 *
 * Internally, #OstreeRepoFinderConfig keeps an FD open pointing to the
 * top-level configuration file directory throughout its lifetime. Due to its
 * use of file descriptors, #OstreeRepoFinderConfig will only work with
 * configuration files on a local file system. TODO
 *
 * Since: 2017.6
 */

/* TODO: Add another implementation (OstreeRepoFinderOldConfig?) which returns
 * all the remotes from /ostree/repo/config. */

static void ostree_repo_finder_config_iface_init (OstreeRepoFinderInterface *iface);

struct _OstreeRepoFinderConfig
{
  GObject parent_instance;

  OstreeRepo *repo;  /* owned */
};

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderConfig, ostree_repo_finder_config, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_config_iface_init))

static void
ostree_repo_finder_config_resolve_async (OstreeRepoFinder    *finder,
                                         const gchar * const *refs,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  OstreeRepoFinderConfig *self = OSTREE_REPO_FINDER_CONFIG (finder);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  const gint priority = 100;  /* arbitrarily chosen; lower than the others */
  gsize i, j;
  g_autoptr(GHashTable) repo_name_to_refs = NULL;  /* (element-type utf8 GPtrArray) */
  GPtrArray *supported_refs;  /* (element-type utf8) */
  GHashTableIter iter;
  const gchar *remote_name;
  g_auto(GStrv) remotes = NULL;
  gsize n_remotes = 0;

  task = g_task_new (finder, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_config_resolve_async);
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);
  repo_name_to_refs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            NULL, (GDestroyNotify) g_ptr_array_unref);

  /* List all remotes in this #OstreeRepo and see which of their ref lists
   * intersect with @refs. */
  remotes = ostree_repo_remote_list (self->repo, (guint *) &n_remotes);

  for (i = 0; i < n_remotes; i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GHashTable) remote_refs = NULL;  /* (element-type utf8 utf8) */

      remote_name = remotes[i];

      if (!ostree_repo_remote_list_refs (self->repo, remote_name, &remote_refs,
                                         cancellable, &local_error))
        {
          g_debug ("Ignoring remote ‘%s’ due to error loading its refs: %s",
                   remote_name, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      for (j = 0; refs[j] != NULL; j++)
        {
          if (g_hash_table_contains (remote_refs, refs[j]))
            {
              /* The requested ref is listed in the refs for this remote. Add
               * the remote to the results, and the ref to its
               * @supported_refs.. */
              g_debug ("Resolved ref ‘%s’ to remote ‘%s’.",
                       refs[j], remote_name);

              supported_refs = g_hash_table_lookup (repo_name_to_refs, remote_name);

              if (supported_refs == NULL)
                {
                  supported_refs = g_ptr_array_new_with_free_func (NULL);
                  g_hash_table_insert (repo_name_to_refs, (gpointer) remote_name, supported_refs  /* transfer */);
                }

              g_ptr_array_add (supported_refs, (gpointer) refs[j]);
            }
        }
    }

  /* Aggregate the results. */
  g_hash_table_iter_init (&iter, repo_name_to_refs);

  while (g_hash_table_iter_next (&iter, (gpointer *) &remote_name, (gpointer *) &supported_refs))
    {
      g_autoptr(GError) local_error = NULL;
      OstreeRemote *remote;

      g_ptr_array_add (supported_refs, NULL);  /* NULL terminator */

      /* We don’t know what last-modified timestamp the remote has without
       * making expensive HTTP queries, so leave that information blank. We
       * assume that the configuration which says these @supported_refs are in
       * the repository is correct; the code in ostree_repo_find_remotes_async()
       * will check that. */
      remote = _ostree_repo_get_remote_inherited (self->repo, remote_name, &local_error);
      if (remote == NULL)
        {
          g_debug ("Configuration for remote ‘%s’ could not be found. Ignoring.",
                   remote_name);
          continue;
        }

      g_ptr_array_add (results, ostree_repo_finder_result_new (remote, priority, (const gchar * const *) supported_refs->pdata, 0));
    }

  g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

static GPtrArray *
ostree_repo_finder_config_resolve_finish (OstreeRepoFinder  *finder,
                                          GAsyncResult      *result,
                                          GError           **error)
{
  g_return_val_if_fail (g_task_is_valid (result, finder), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_repo_finder_config_init (OstreeRepoFinderConfig *self)
{
  /* Nothing to see here. */
}

static void
ostree_repo_finder_config_constructed (GObject *object)
{
  OstreeRepoFinderConfig *self = OSTREE_REPO_FINDER_CONFIG (object);

  G_OBJECT_CLASS (ostree_repo_finder_config_parent_class)->constructed (object);

  g_assert (self->repo != NULL);
}

typedef enum
{
  PROP_REPO = 1,
} OstreeRepoFinderConfigProperty;

static void
ostree_repo_finder_config_get_property (GObject    *object,
                                        guint       property_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  OstreeRepoFinderConfig *self = OSTREE_REPO_FINDER_CONFIG (object);

  switch ((OstreeRepoFinderConfigProperty) property_id)
    {
    case PROP_REPO:
      g_value_set_object (value, self->repo);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
ostree_repo_finder_config_set_property (GObject      *object,
                                        guint         property_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  OstreeRepoFinderConfig *self = OSTREE_REPO_FINDER_CONFIG (object);

  switch ((OstreeRepoFinderConfigProperty) property_id)
    {
    case PROP_REPO:
      /* Construct-only. */
      g_assert (self->repo == NULL);
      self->repo = g_value_dup_object (value);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
ostree_repo_finder_config_dispose (GObject *object)
{
  OstreeRepoFinderConfig *self = OSTREE_REPO_FINDER_CONFIG (object);

  g_clear_object (&self->repo);

  G_OBJECT_CLASS (ostree_repo_finder_config_parent_class)->dispose (object);
}

static void
ostree_repo_finder_config_class_init (OstreeRepoFinderConfigClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = ostree_repo_finder_config_get_property;
  object_class->set_property = ostree_repo_finder_config_set_property;
  object_class->constructed = ostree_repo_finder_config_constructed;
  object_class->dispose = ostree_repo_finder_config_dispose;

  /**
   * OstreeRepoFinderConfig:repo:
   *
   * Directory containing configuration files for refs. Each ref’s configuration
   * file is named after the ref, plus a `.conf` suffix. This should be provided
   * in the form of an open directory FD, which will be duplicated. The
   * corresponding path should be provided as #OstreeRepoFinderConfig:refs-path,
   * which will be used in error messages, but no I/O operations.
   *
   * The default is `/etc/ostree/refs.d`. TODO
   *
   * Since: 2017.6
   */
  g_object_class_install_property (object_class, PROP_REPO,
                                   g_param_spec_object ("repo",
                                                        "Repository",
                                                        "TODO",
                                                        OSTREE_TYPE_REPO,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
ostree_repo_finder_config_iface_init (OstreeRepoFinderInterface *iface)
{
  iface->resolve_async = ostree_repo_finder_config_resolve_async;
  iface->resolve_finish = ostree_repo_finder_config_resolve_finish;
}

/**
 * ostree_repo_finder_config_new:
 * @repo: (transfer none): OSTree repository to use the remote list from
 *
 * Create a new #OstreeRepoFinderConfig, loading configuration files from the
 * given @refs_dfd .If @refs_dfd is < 0, the system default will be used. The
 * path in @refs_path should correspond to @refs_dfd, and is used in error
 * messages. TODO
 *
 * Returns: (transfer full): a new #OstreeRepoFinderConfig
 * Since: 2017.6
 */
OstreeRepoFinderConfig *
ostree_repo_finder_config_new (OstreeRepo *repo)
{
  g_return_val_if_fail (OSTREE_IS_REPO (repo), NULL);

  return g_object_new (OSTREE_TYPE_REPO_FINDER_CONFIG,
                       "repo", repo,
                       NULL);
}
