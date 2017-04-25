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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static gchar *opt_cache_dir = NULL;
static gboolean opt_disable_fsync = FALSE;

static GOptionEntry options[] =
  {
    { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_cache_dir, "Use custom cache dir", NULL },
    { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
    { NULL }
  };

/* TODO: Move this into ostree-repo.h? */
typedef OstreeRepoFinderResult* RepoFinderResultArray;

static void
repo_finder_result_array_free (RepoFinderResultArray *array)
{
  gsize i;

  for (i = 0; array[i] != NULL; i++)
    ostree_repo_finder_result_free (array[i]);

  g_free (array);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RepoFinderResultArray, repo_finder_result_array_free)

static gchar *
uint64_secs_to_iso8601 (guint64 secs)
{
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc (secs);

  if (dt != NULL)
    return g_date_time_format (dt, "%FT%TZ");
  else
    return g_strdup ("invalid");
}

/* TODO: Move to ostree-remote.c? */
static gchar *
remote_get_uri (OstreeRemote *remote)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *uri = NULL;

  uri = g_key_file_get_string (remote->options, remote->group, "url", &error);
  g_assert_no_error (error);

  return g_steal_pointer (&uri);
}

static void
get_result_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  GAsyncResult **result_out = user_data;
  *result_out = g_object_ref (result);
}

/* TODO: Add a man page. */
gboolean
ostree_builtin_find_remotes (int            argc,
                             char         **argv,
                             GCancellable  *cancellable,
                             GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  gsize i;
  g_autoptr(GAsyncResult) find_result = NULL;
  g_autoptr(RepoFinderResultArray) results = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };

  context = g_option_context_new ("REF [REF...] - Find remotes to serve the given refs");

  /* Parse options. */
  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (!ostree_ensure_repo_writable (repo, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "At least one REF must be specified", error);
      return FALSE;
    }

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (opt_cache_dir &&
      !ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
    return FALSE;

  /* Read in the refs to search for remotes for. */
  refs = g_ptr_array_new_with_free_func (NULL);

  for (i = 1; i < argc; i++)
    g_ptr_array_add (refs, argv[i]);

  g_ptr_array_add (refs, NULL);

  /* Run the operation. */
  glnx_console_lock (&console);

  if (console.is_tty)
    progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

  ostree_repo_find_remotes_async (repo,
                                  (const gchar * const *) refs->pdata,
                                  NULL  /* no options */,
                                  NULL  /* default finders */,  /* TODO: allow this to be customised */
                                  progress, cancellable,
                                  get_result_cb, &find_result);

  while (find_result == NULL)
    g_main_context_iteration (NULL, TRUE);

  results = ostree_repo_find_remotes_finish (repo, find_result, error);

  if (results == NULL)
    return FALSE;

  if (progress)
    ostree_async_progress_finish (progress);

  /* Print results. TODO: Sort by priority or document the sort order we receive? */
  for (i = 0; results[i] != NULL; i++)
    {
      g_autofree gchar *uri = NULL;
      g_autofree gchar *refs_string = NULL;
      g_autofree gchar *last_modified_string = NULL;

      uri = remote_get_uri (results[i]->remote);
      refs_string = g_strjoinv ("\n  - ", results[i]->refs);

      if (results[i]->summary_last_modified > 0)
        last_modified_string = uint64_secs_to_iso8601 (results[i]->summary_last_modified);
      else
        last_modified_string = g_strdup ("unknown");

      g_print ("Result %" G_GSIZE_FORMAT ": %s\n"
               " - Priority: %d\n"
               " - Summary last modified: %s\n"
               " - Refs:\n"
               "  - %s\n",
               i, uri, results[i]->priority, last_modified_string, refs_string);
    }

  if (results[0] == NULL)
    g_print ("No results.\n");

  /* TODO: Print out the refs which weren’t found. */

  return TRUE;
}
