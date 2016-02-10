/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>
#include <gio/gunixoutputstream.h>
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-container-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libgsystem.h"


static GOptionEntry init_option_entries[] = {
  { NULL }
};

static GOptionEntry assemble_option_entries[] = {
  { NULL }
};

typedef struct {
  char *userroot_base;
  int userroot_dfd;

  int roots_dfd;
  OstreeRepo *repo;
  RpmOstreeContext *ctx;

  int rpmmd_dfd;
} ROContainerContext;

#define RO_CONTAINER_CONTEXT_INIT { .userroot_dfd = -1, .rpmmd_dfd = -1 }

static gboolean
roc_context_init_core (ROContainerContext *rocctx,
                       GError            **error)
{
  gboolean ret = FALSE;

  rocctx->userroot_base = get_current_dir_name ();
  if (!glnx_opendirat (AT_FDCWD, rocctx->userroot_base, TRUE, &rocctx->userroot_dfd, error))
    goto out;

  { g_autofree char *repo_pathstr = g_strconcat (rocctx->userroot_base, "/repo", NULL);
    g_autoptr(GFile) repo_path = g_file_new_for_path (repo_pathstr);
    rocctx->repo = ostree_repo_new (repo_path);
  }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
roc_context_init (ROContainerContext *rocctx,
                  GError            **error)
{
  gboolean ret = FALSE;
  
  if (!roc_context_init_core (rocctx, error))
    goto out;

  if (!glnx_opendirat (rocctx->userroot_dfd, "roots", TRUE, &rocctx->roots_dfd, error))
    goto out;

  if (!ostree_repo_open (rocctx->repo, NULL, error))
    goto out;

  if (!glnx_opendirat (rocctx->userroot_dfd, "cache/rpm-md", FALSE, &rocctx->rpmmd_dfd, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
roc_context_prepare_for_root (ROContainerContext *rocctx,
                              const char         *target,
                              GError            **error)
{
  gboolean ret = FALSE;
  /* We don't point the install root at the existing directory because
   * libhif tries to inspect it, which is wrong.  So basically make up
   * a fake directory each time.
   */
  g_autofree char *abs_instroot = glnx_fdrel_abspath (rocctx->roots_dfd, target);
  const char *abs_instroot_tmp = glnx_strjoina (abs_instroot, ".ignore");

  rocctx->ctx = rpmostree_context_new_unprivileged (rocctx->userroot_dfd, NULL, error);
  if (!rocctx->ctx)
    goto out;

  if (!rpmostree_context_setup (rocctx->ctx, abs_instroot_tmp, NULL, error))
    goto out;

  if (!glnx_shutil_rm_rf_at (AT_FDCWD, abs_instroot_tmp, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static void
roc_context_deinit (ROContainerContext *rocctx)
{
  g_free (rocctx->userroot_base);
  if (rocctx->userroot_dfd)
    (void) close (rocctx->userroot_dfd);
  g_clear_object (&rocctx->repo);
  if (rocctx->roots_dfd)
    (void) close (rocctx->roots_dfd);
  if (rocctx->rpmmd_dfd)
    (void) close (rocctx->rpmmd_dfd);
  g_clear_object (&rocctx->ctx);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ROContainerContext, roc_context_deinit)

int
rpmostree_container_builtin_init (int             argc,
                                  char          **argv,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  GOptionContext *context = g_option_context_new ("");
  static const char* const directories[] = { "repo", "rpmmd.repos.d", "cache/rpm-md", "roots", "tmp" };
  guint i;
  
  if (!rpmostree_option_context_parse (context,
                                       init_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (!roc_context_init_core (rocctx, error))
    goto out;

  for (i = 0; i < G_N_ELEMENTS (directories); i++)
    {
      if (!glnx_shutil_mkdir_p_at (rocctx->userroot_dfd, directories[i], 0755, cancellable, error))
        goto out;
    }

  if (!ostree_repo_create (rocctx->repo, OSTREE_REPO_MODE_BARE_USER, cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}

/*
 * Like symlinkat() but overwrites (atomically) an existing
 * symlink.
 */
static gboolean
symlink_at_replace (const char    *oldpath,
                    int            parent_dfd,
                    const char    *newpath,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;
  int res;
  /* Possibly in the future generate a temporary random name here,
   * would need to move "generate a temporary name" code into
   * libglnx or glib?
   */
  const char *temppath = glnx_strjoina (newpath, ".tmp");

  /* Clean up any stale temporary links */ 
  (void) unlinkat (parent_dfd, temppath, 0);

  /* Create the temp link */ 
  do
    res = symlinkat (oldpath, parent_dfd, temppath);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Rename it into place */ 
  do
    res = renameat (parent_dfd, temppath, parent_dfd, newpath);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

int
rpmostree_container_builtin_assemble (int             argc,
                                      char          **argv,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("NAME [PKGNAME PKGNAME...]");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  g_autoptr(RpmOstreeInstall) install = {0,};
  const char *name;
  struct stat stbuf;
  g_autofree char**pkgnames = NULL;
  g_autofree char *commit = NULL;
  const char *target_rootdir;
  
  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  name = argv[1];
  { g_autoptr(GPtrArray) pkgnamesv = g_ptr_array_new_with_free_func (NULL);
    if (argc == 2)
      g_ptr_array_add (pkgnamesv, argv[1]);
    else
      {
        guint i;
        for (i = 2; i < argc; i++)
          g_ptr_array_add (pkgnamesv, argv[i]);
      }
    g_ptr_array_add (pkgnamesv, NULL);
    pkgnames = (char**)g_ptr_array_free (pkgnamesv, FALSE);
  }

  if (!roc_context_init (rocctx, error))
    goto out;

  target_rootdir = glnx_strjoina (name, ".0");

  if (fstatat (rocctx->roots_dfd, target_rootdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tree %s already exists", target_rootdir);
      goto out;
    }

  if (!roc_context_prepare_for_root (rocctx, target_rootdir, error))
    goto out;

  /* --- Downloading metadata --- */
  if (!rpmostree_context_download_metadata (rocctx->ctx, cancellable, error))
    goto out;

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare_install (rocctx->ctx, (const char*const*)pkgnames,
                                           &install, cancellable, error))
    goto out;

  /* --- Download and import as necessary --- */
  if (!rpmostree_context_download_import (rocctx->ctx, install,
                                          cancellable, error))
    goto out;

  { glnx_fd_close int tmpdir_dfd = -1;

    if (!glnx_opendirat (rocctx->userroot_dfd, "tmp", TRUE, &tmpdir_dfd, error))
      goto out;
    
    if (!rpmostree_context_assemble_commit (rocctx->ctx, tmpdir_dfd,
                                            name,
                                            install,
                                            &commit,
                                            cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...\n", name, commit);

  { OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    /* For now... to be crash safe we'd need to duplicate some of the
     * boot-uuid/fsync gating at a higher level.
     */
    opts.disable_fsync = TRUE;

    /* Also, what we really want here is some sort of sane lifecycle
     * management with whatever is running in the root.
     */
    if (!glnx_shutil_rm_rf_at (rocctx->roots_dfd, target_rootdir, cancellable, error))
      goto out;

    if (!ostree_repo_checkout_tree_at (rocctx->repo, &opts, rocctx->roots_dfd, target_rootdir,
                                       commit, cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...done\n", name, commit);

  if (!symlink_at_replace (target_rootdir, rocctx->roots_dfd, name,
                           cancellable, error))
    goto out;

  g_print ("Creating current symlink...done\n");

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}

#define APP_VERSION_REGEXP ".+\\.([01])"

static gboolean
parse_app_version (const char *name,
                   guint      *out_version,
                   GError    **error)
{
  gboolean ret = FALSE;
  GMatchInfo *match = NULL;
  static gsize regex_initialized;
  static GRegex *regex;
  int ret_version;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new (APP_VERSION_REGEXP, 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (!g_regex_match (regex, name, 0, &match))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid app link %s", name);
      goto out;
    }

  { g_autofree char *version_str = g_match_info_fetch (match, 1);
    ret_version = g_ascii_strtoull (version_str, NULL, 10);
    
    switch (ret_version)
      {
      case 0:
      case 1:
        break;
      default:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Invalid version in app link %s", name);
        goto out;
      }
  }

  ret = TRUE;
  *out_version = ret_version;
 out:
  return ret;
}

gboolean
rpmostree_container_builtin_upgrade (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("NAME");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  g_autoptr(RpmOstreeInstall) install = NULL;
  const char *name;
  const char *const*pkgnames;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *new_commit_checksum = NULL;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) input_packages_v = NULL;
  g_autoptr(GVariant) input_goal_sha512_v = NULL;
  guint current_version;
  guint new_version;
  g_autofree char *previous_goal_sha512 = NULL;
  const char *target_current_root;
  const char *target_new_root;
  
  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  name = argv[1];

  if (!roc_context_init (rocctx, error))
    goto out;

  target_current_root = glnx_readlinkat_malloc (rocctx->roots_dfd, name, cancellable, error);
  if (!target_current_root)
    {
      g_prefix_error (error, "Reading app link %s: ", name);
      goto out;
    }

  if (!parse_app_version (target_current_root, &current_version, error))
    goto out;

  new_version = current_version == 0 ? 1 : 0;
  /* Yeah, I'm being mildly clever here indexing into the constant
   * array which is a pointless micro-optimization avoiding
   * g_strdup_printf().
   */
  if (new_version == 0)
    target_new_root = glnx_strjoina (name, ".0");
  else
    target_new_root = glnx_strjoina (name, ".1");

  if (!roc_context_prepare_for_root (rocctx, name, error))
    goto out;

  /* --- Downloading metadata --- */
  if (!rpmostree_context_download_metadata (rocctx->ctx, cancellable, error))
    goto out;

  { g_autoptr(GVariantDict) metadata_dict = NULL;

    if (!ostree_repo_resolve_rev (rocctx->repo, name, FALSE, &commit_checksum, error))
      goto out;

    if (!ostree_repo_load_variant (rocctx->repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum,
                                   &commit, error))
      goto out;

    metadata = g_variant_get_child_value (commit, 0);
    metadata_dict = g_variant_dict_new (metadata);

    input_packages_v = _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.input-packages",
                                                                 (GVariantType*)"as", error);
    if (!input_packages_v)
      goto out;

    pkgnames = g_variant_get_strv (input_packages_v, NULL);

    input_goal_sha512_v = _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.nevras-sha512",
                                                                    (GVariantType*)"s", error);
    if (!input_goal_sha512_v)
      goto out;

    previous_goal_sha512 = g_variant_dup_string (input_goal_sha512_v, NULL);
  }
      
  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare_install (rocctx->ctx, pkgnames, &install,
                                          cancellable, error))
    goto out;

  { g_autofree char *new_goal_sha512 = rpmostree_hif_checksum_goal (G_CHECKSUM_SHA512, hif_context_get_goal (rpmostree_context_get_hif (rocctx->ctx)));

    if (strcmp (new_goal_sha512, previous_goal_sha512) == 0)
      {
        g_print ("No changes in inputs to %s (%s)\n", name, commit_checksum);
        exit_status = EXIT_SUCCESS;
        goto out;
      }
  }

  /* --- Download and import as necessary --- */
  if (!rpmostree_context_download_import (rocctx->ctx, install,
                                          cancellable, error))
    goto out;

  { glnx_fd_close int tmpdir_dfd = -1;

    if (!glnx_opendirat (rocctx->userroot_dfd, "tmp", TRUE, &tmpdir_dfd, error))
      goto out;
    
    if (!rpmostree_context_assemble_commit (rocctx->ctx, tmpdir_dfd,
                                            name,
                                            install,
                                            &new_commit_checksum,
                                            cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...\n", name, new_commit_checksum);

  { OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    /* For now... to be crash safe we'd need to duplicate some of the
     * boot-uuid/fsync gating at a higher level.
     */
    opts.disable_fsync = TRUE;

    if (!ostree_repo_checkout_tree_at (rocctx->repo, &opts, rocctx->roots_dfd, target_new_root,
                                       new_commit_checksum, cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...done\n", name, new_commit_checksum);

  if (!symlink_at_replace (target_new_root, rocctx->roots_dfd, name,
                           cancellable, error))
    goto out;

  g_print ("Creating current symlink...done\n");

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}