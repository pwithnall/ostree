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

#include "ostree-remote-private.h"
#include "ostree-repo-finder.h"

static void ostree_repo_finder_default_init (OstreeRepoFinderInterface *iface);

G_DEFINE_INTERFACE (OstreeRepoFinder, ostree_repo_finder, G_TYPE_OBJECT)

static void
ostree_repo_finder_default_init (OstreeRepoFinderInterface *iface)
{
  /* Nothing to see here. */
}

/* Validate the given string is potentially a ref name. */
static gboolean
is_valid_ref_name (const gchar *ref_name)
{
  return (ref_name != NULL && *ref_name != '\0' && g_str_is_ascii (ref_name));
}

/* Validate @refs is non-%NULL, non-empty, and contains only valid ref names. */
static gboolean
is_valid_ref_array (const gchar * const *refs)
{
  gsize i;

  if (refs == NULL || *refs == NULL)
    return FALSE;

  for (i = 0; refs[i] != NULL; i++)
    {
      if (!is_valid_ref_name (refs[i]))
        return FALSE;
    }

  return TRUE;
}

/* TODO: Docs */
static gboolean
is_valid_checksum (const gchar *checksum)
{
  /* TODO */
  return TRUE;
}

/* Validate @ref_to_checksum is non-%NULL, non-empty, and contains only valid
 * ref names as keys and only valid commit checksums as values. */
static gboolean
is_valid_ref_map (GHashTable *ref_to_checksum)
{
  GHashTableIter iter;
  const gchar *ref;
  const gchar *checksum;

  if (ref_to_checksum == NULL || g_hash_table_size (ref_to_checksum) == 0)
    return FALSE;

  g_hash_table_iter_init (&iter, ref_to_checksum);

  while (g_hash_table_iter_next (&iter, (gpointer *) &ref, (gpointer *) &checksum))
    {
      if (!is_valid_ref_name (ref))
        return FALSE;
      if (!is_valid_checksum (checksum))
        return FALSE;
    }

  return TRUE;
}

static void resolve_cb (GObject      *obj,
                        GAsyncResult *result,
                        gpointer      user_data);

/**
 * ostree_repo_finder_resolve_async:
 * @self: an #OstreeRepoFinder
 * @refs: (array zero-terminated=1): non-empty array of refs to find remotes for
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: asynchronous completion callback
 * @user_data: data to pass to @callback
 *
 * Find reachable remote URIs which claim to provide any of the given @refs. The
 * specific method for finding the remotes depends on the #OstreeRepoFinder
 * implementation.
 *
 * Any remote which is found and which claims to support any of the given @refs
 * will be returned in the results. It is possible that a remote claims to
 * support a given ref, but turns out not to — it is not possible to verify this
 * until ostree_repo_pull_from_remotes_async() is called.
 *
 * The returned results will be sorted with the most useful first — this is
 * typically the remote which claims to provide the most of @refs, at the lowest
 * latency. TODO: Verify that implementations actually do this.
 *
 * Each result contains a list of the subset of @refs it claims to provide. It
 * is possible for a non-empty list of results to be returned, but for some of
 * @refs to not be listed in any of the results. Callers must check for this.
 *
 * Pass the results to ostree_repo_pull_from_remotes_async() to pull the given
 * @refs from those remotes.
 *
 * Since: 2017.6
 */
void
ostree_repo_finder_resolve_async (OstreeRepoFinder    *self,
                                  const gchar * const *refs,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  OstreeRepoFinderInterface *iface;
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (OSTREE_IS_REPO_FINDER (self));
  g_return_if_fail (is_valid_ref_array (refs));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_resolve_async);

  iface = OSTREE_REPO_FINDER_GET_IFACE (self);
  g_assert (iface->resolve_async != NULL);
  g_assert (iface->resolve_finish != NULL);

  iface->resolve_async (self, refs, cancellable, resolve_cb, g_steal_pointer (&task));
}

static void
resolve_cb (GObject      *obj,
            GAsyncResult *result,
            gpointer      user_data)
{
  OstreeRepoFinder *self;
  OstreeRepoFinderInterface *iface;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  g_autoptr(GError) local_error = NULL;

  self = OSTREE_REPO_FINDER (obj);
  iface = OSTREE_REPO_FINDER_GET_IFACE (self);
  task = G_TASK (user_data);
  results = iface->resolve_finish (self, result, &local_error);

  g_assert ((local_error == NULL) != (results == NULL));

  if (local_error != NULL)
    g_task_return_error (task, g_steal_pointer (&local_error));
  else
    g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

/**
 * ostree_repo_finder_resolve_finish:
 * @self: an #OstreeRepoFinder
 * @result: #GAsyncResult from the callback
 * @error: return location for a #GError
 *
 * Get the results from a ostree_repo_finder_resolve_async() operation.
 *
 * Returns: (transfer full) (element-type OstreeRepoFinderResult): array of zero
 *    or more results
 * Since: 2017.6
 */
GPtrArray *
ostree_repo_finder_resolve_finish (OstreeRepoFinder  *self,
                                   GAsyncResult      *result,
                                   GError           **error)
{
  g_return_val_if_fail (OSTREE_IS_REPO_FINDER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static gint
sort_results_cb (gconstpointer a,
                 gconstpointer b)
{
  const OstreeRepoFinderResult *result_a = *((const OstreeRepoFinderResult **) a);
  const OstreeRepoFinderResult *result_b = *((const OstreeRepoFinderResult **) b);

  return ostree_repo_finder_result_compare (result_a, result_b);
}

typedef struct
{
  gsize n_finders_pending;
  GPtrArray *results;
} ResolveAllData;

static void
resolve_all_data_free (ResolveAllData *data)
{
  g_assert (data->n_finders_pending == 0);
  g_clear_pointer (&data->results, g_ptr_array_unref);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ResolveAllData, resolve_all_data_free)

static void resolve_all_cb (GObject      *obj,
                            GAsyncResult *result,
                            gpointer      user_data);
static void resolve_all_finished_one (GTask *task);

/**
 * ostree_repo_finder_resolve_all_async:
 * @finders: (array zero-terminated=1): non-empty array of #OstreeRepoFinders
 * @refs: (array zero-terminated=1): non-empty array of refs to find remotes for
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: asynchronous completion callback
 * @user_data: data to pass to @callback
 *
 * A version of ostree_repo_finder_resolve_async() which queries one or more
 * @finders in parallel and combines the results.
 *
 * Since: 2017.6
 */
void
ostree_repo_finder_resolve_all_async (OstreeRepoFinder * const *finders,
                                      const gchar * const      *refs,
                                      GCancellable             *cancellable,
                                      GAsyncReadyCallback       callback,
                                      gpointer                  user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(ResolveAllData) data = NULL;
  gsize i;
  g_autofree gchar *refs_str = NULL;
  g_autoptr(GString) finders_str = NULL;

  g_return_if_fail (finders != NULL && finders[0] != NULL);
  g_return_if_fail (is_valid_ref_array (refs));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  refs_str = g_strjoinv (", ", (gchar **) refs);
  finders_str = g_string_new ("");
  for (i = 0; finders[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append (finders_str, ", ");
      g_string_append (finders_str, g_type_name (G_TYPE_FROM_INSTANCE (finders[i])));
    }

  g_debug ("%s: Resolving refs [%s] with finders [%s]", G_STRFUNC,
           refs_str, finders_str->str);

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_resolve_all_async);

  data = g_new0 (ResolveAllData, 1);
  data->n_finders_pending = 1;  /* while setting up the loop */
  data->results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);
  g_task_set_task_data (task, data, (GDestroyNotify) resolve_all_data_free);

  /* Start all the asynchronous queries in parallel. */
  for (i = 0; finders[i] != NULL; i++)
    {
      OstreeRepoFinder *finder = OSTREE_REPO_FINDER (finders[i]);
      OstreeRepoFinderInterface *iface;

      iface = OSTREE_REPO_FINDER_GET_IFACE (finder);
      g_assert (iface->resolve_async != NULL);
      iface->resolve_async (finder, refs, cancellable, resolve_all_cb, g_object_ref (task));
      data->n_finders_pending++;
    }

  resolve_all_finished_one (task);
  data = NULL;  /* passed to the GTask above */
}

/* Modifies both arrays in place. */
static void
array_concatenate_steal (GPtrArray *array,
                         GPtrArray *to_concatenate)  /* (transfer full) */
{
  g_autoptr(GPtrArray) array_to_concatenate = to_concatenate;
  gsize i;

  for (i = 0; i < array_to_concatenate->len; i++)
    {
      /* Sanity check that the arrays do not contain any %NULL elements
       * (particularly NULL terminators). */
      g_assert (g_ptr_array_index (array_to_concatenate, i) != NULL);
      g_ptr_array_add (array, g_steal_pointer (&g_ptr_array_index (array_to_concatenate, i)));
    }

  g_ptr_array_set_free_func (array_to_concatenate, NULL);
  g_ptr_array_set_size (array_to_concatenate, 0);
}

static void
resolve_all_cb (GObject      *obj,
                GAsyncResult *result,
                gpointer      user_data)
{
  OstreeRepoFinder *finder;
  OstreeRepoFinderInterface *iface;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  g_autoptr(GError) local_error = NULL;
  ResolveAllData *data;

  finder = OSTREE_REPO_FINDER (obj);
  iface = OSTREE_REPO_FINDER_GET_IFACE (finder);
  task = G_TASK (user_data);
  data = g_task_get_task_data (task);
  results = iface->resolve_finish (finder, result, &local_error);

  g_assert ((local_error == NULL) != (results == NULL));

  if (local_error != NULL)
    g_debug ("Error resolving refs to repository URI using %s: %s",
             g_type_name (G_TYPE_FROM_INSTANCE (finder)), local_error->message);
  else
    array_concatenate_steal (data->results, g_steal_pointer (&results));

  resolve_all_finished_one (task);
}

static void
resolve_all_finished_one (GTask *task)
{
  ResolveAllData *data;

  data = g_task_get_task_data (task);

  data->n_finders_pending--;

  if (data->n_finders_pending == 0)
    {
      gsize i;
      g_autoptr(GString) results_str = NULL;

      g_ptr_array_sort (data->results, sort_results_cb);

      results_str = g_string_new ("");
      for (i = 0; i < data->results->len; i++)
        {
          const OstreeRepoFinderResult *result = g_ptr_array_index (data->results, i);

          if (i != 0)
            g_string_append (results_str, ", ");
          g_string_append (results_str, ostree_remote_get_name (result->remote));
        }
      if (i == 0)
        g_string_append (results_str, "(none)");

      g_debug ("%s: Finished, results: %s", G_STRFUNC, results_str->str);

      g_task_return_pointer (task, g_steal_pointer (&data->results), (GDestroyNotify) g_ptr_array_unref);
    }
}

/**
 * ostree_repo_finder_resolve_all_finish:
 * @result: #GAsyncResult from the callback
 * @error: return location for a #GError
 *
 * Get the results from a ostree_repo_finder_resolve_all_async() operation.
 *
 * Returns: (transfer full) (element-type OstreeRepoFinderResult): array of zero
 *    or more results
 * Since: 2017.6
 */
GPtrArray *
ostree_repo_finder_resolve_all_finish (GAsyncResult  *result,
                                       GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * ostree_repo_finder_result_new:
 * @remote: (transfer none): TODO
 * @finder: (transfer none): TODO
 * @priority: TODO
 * @ref_to_checksum: (element-type utf8 utf8): TODO
 * @summary_last_modified: TODO
 *
 * TODO: Docs
 *
 * Returns: (transfer full): a new #OstreeRepoFinderResult
 * Since: 2017.6
 */
OstreeRepoFinderResult *
ostree_repo_finder_result_new (OstreeRemote        *remote,
                               OstreeRepoFinder    *finder,
                               gint                 priority,
                               GHashTable          *ref_to_checksum,
                               guint64              summary_last_modified)
{
  g_autoptr(OstreeRepoFinderResult) result = NULL;

  g_return_val_if_fail (remote != NULL, NULL);
  g_return_val_if_fail (OSTREE_IS_REPO_FINDER (finder), NULL);
  g_return_val_if_fail (is_valid_ref_map (ref_to_checksum), NULL);

  result = g_new0 (OstreeRepoFinderResult, 1);
  result->remote = ostree_remote_ref (remote);
  result->finder = g_object_ref (finder);
  result->priority = priority;
  result->ref_to_checksum = g_hash_table_ref (ref_to_checksum);
  result->summary_last_modified = summary_last_modified;

  return g_steal_pointer (&result);
}

/**
 * ostree_repo_finder_result_compare:
 * @a: an #OstreeRepoFinderResult
 * @b: an #OstreeRepoFinderResult
 *
 * Compare two #OstreeRepoFinderResult instances to work out which one is better
 * to pull from, and hence needs to be ordered before the other.
 *
 * Returns: <0 if @a is ordered before @b, 0 if they are ordered equally,
 *    >0 if @b is ordered before @a
 * Since: 2017.6
 */
gint
ostree_repo_finder_result_compare (const OstreeRepoFinderResult *a,
                                   const OstreeRepoFinderResult *b)
{
  guint a_n_refs, b_n_refs;

  g_return_val_if_fail (a != NULL, 0);
  g_return_val_if_fail (b != NULL, 0);

  /* TODO: Check if this is really the ordering we want. For example, we
   * probably don’t want a result with 0 refs to be ordered before one with >0
   * refs, just because its priority is higher. */
  if (a->priority != b->priority)
    return a->priority - b->priority;

  if (a->summary_last_modified != 0 && b->summary_last_modified != 0 &&
      a->summary_last_modified != b->summary_last_modified)
    return a->summary_last_modified - b->summary_last_modified;

  a_n_refs = g_hash_table_size (a->ref_to_checksum);
  b_n_refs = g_hash_table_size (b->ref_to_checksum);

  if (a_n_refs != b_n_refs)
    return (gint) a_n_refs - (gint) b_n_refs;

  return g_strcmp0 (a->remote->name, b->remote->name);
}

/**
 * ostree_repo_finder_result_free:
 * @result: (transfer full): an #OstreeRepoFinderResult
 *
 * Free the given @result.
 *
 * Since: 2017.6
 */
void
ostree_repo_finder_result_free (OstreeRepoFinderResult *result)
{
  g_return_if_fail (result != NULL);

  g_hash_table_unref (result->ref_to_checksum);
  g_object_unref (result->finder);
  ostree_remote_unref (result->remote);
  g_free (result);
}

/**
 * ostree_repo_finder_result_freev:
 * @results: (array zero-terminated=1) (transfer full): an #OstreeRepoFinderResult
 *
 * Free the given @results array, freeing each element and the container.
 *
 * Since: 2017.6
 */
void
ostree_repo_finder_result_freev (OstreeRepoFinderResult **results)
{
  gsize i;

  for (i = 0; results[i] != NULL; i++)
    ostree_repo_finder_result_free (results[i]);

  g_free (results);
}

