/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>

#include "ostree-mount-util.h"

static void
do_remount (const char *target)
{
  struct stat stbuf;
  if (lstat (target, &stbuf) < 0)
    return;
  /* Silently ignore symbolic links; we expect these to point to
   * /sysroot, and thus there isn't a bind mount there.
   */
  if (S_ISLNK (stbuf.st_mode))
    return;
  /* If not a mountpoint, skip it */
  struct statvfs stvfsbuf;
  if (statvfs (target, &stvfsbuf) == -1)
    return;
  /* If no read-only flag, skip it */
  if ((stvfsbuf.f_flag & ST_RDONLY) == 0)
    return;
  /* It's a mounted, read-only fs; remount it */
  if (mount (target, target, NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
    {
      /* Also ignore EINVAL - if the target isn't a mountpoint
       * already, then assume things are OK.
       */
      if (errno != EINVAL)
        err (EXIT_FAILURE, "failed to remount %s", target);
    }
  else
    printf ("Remounted: %s\n", target);
}

int
main(int argc, char *argv[])
{
  /* When systemd is in use this is normally created via the generator, but
   * we ensure it's created here as well for redundancy.
   */
  touch_run_ostree ();

  /* The /sysroot mount needs to be private to avoid having a mount for e.g. /var/cache
   * also propagate to /sysroot/ostree/deploy/$stateroot/var/cache
   *
   * Today systemd remounts / (recursively) as shared, so we're undoing that as early
   * as possible.  See also a copy of this in ostree-prepare-root.c.
   */
  if (mount ("none", "/sysroot", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    perror ("warning: While remounting /sysroot MS_PRIVATE");

  if (path_is_on_readonly_fs ("/"))
    {
      /* If / isn't writable, don't do any remounts; we don't want
       * to clear the readonly flag in that case.
       */
      exit (EXIT_SUCCESS);
    }

  do_remount ("/sysroot");
  do_remount ("/var");

  exit (EXIT_SUCCESS);
}
