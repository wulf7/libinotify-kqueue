/*******************************************************************************
  Copyright (c) 2014-2018 Vladimir Kondratyev <vladimir@kondratyev.su>
  SPDX-License-Identifier: MIT

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*******************************************************************************/

#include "compat.h"

#include <sys/types.h>
#include <sys/stat.h>  /* fstat */

#include <assert.h>    /* assert */
#include <errno.h>     /* errno */
#include <fcntl.h>     /* AT_FDCWD */
#include <stdbool.h>   /* false */
#include <stdlib.h>    /* calloc, free */
#include <string.h>    /* strcmp */
#include <unistd.h>    /* close */

#include "sys/inotify.h"

#include "inotify-watch.h"
#include "utils.h"
#include "watch-set.h"
#include "watch.h"

#ifdef SKIP_SUBFILES
static const char *skip_fs_types[] = { SKIP_SUBFILES };

/**
 * Check if watch descriptor belongs a filesystem
 * where opening of subfiles is inwanted.
 *
 * @param[in] fd A file descriptor of a watched entry.
 * @return 1 if watching for subfiles is unwanted, 0 otherwise.
 **/
static int
iwatch_want_skip_subfiles (int fd)
{
    struct STATFS st;
    size_t i;

    memset (&st, 0, sizeof (st));
    if (FSTATFS (fd, &st) == -1) {
        perror_msg ("fstatfs failed on %d", fd);
        return false;
    }

    for (i = 0; i < nitems (skip_fs_types); i++) {
        if (strcmp (st.f_fstypename, skip_fs_types[i]) == 0) {
            return 1;
        }
    }

    return 0;
}
#endif

/**
 * Preform minimal initialization required for opening watch descriptor
 *
 * @param[in] path  Path to watch.
 * @param[in] flags A combination of inotify event flags.
 * @return A inotify watch descriptor on success, -1 otherwise.
 **/
int
iwatch_open (const char *path, uint32_t flags)
{
    int fd = watch_open (AT_FDCWD, path, flags);
    if (fd == -1) {
        perror_msg ("Failed to open inotify watch %s", path);
    }

    return fd;
}

/**
 * Initialize inotify watch.
 *
 * This function creates and initializes additional watches for a directory.
 *
 * @param[in] wrk    A pointer to #worker.
 * @param[in] fd     A file descriptor of a watched entry.
 * @param[in] flags  A combination of inotify event flags.
 * @return A pointer to a created #i_watch on success NULL otherwise
 **/
i_watch *
iwatch_init (worker *wrk, int fd, uint32_t flags)
{
    assert (wrk != NULL);
    assert (fd != -1);

    struct stat st;
    if (fstat (fd, &st) == -1) {
        perror_msg ("fstat failed on %d", fd);
        return NULL;
    }

    i_watch *iw = calloc (1, sizeof (i_watch));
    if (iw == NULL) {
        perror_msg ("Failed to allocate inotify watch");
        return NULL;
    }

    iw->wrk = wrk;
    iw->fd = fd;
    iw->flags = flags;
    iw->inode = st.st_ino;
    iw->dev = st.st_dev;
    iw->is_closed = 0;

    watch_set_init (&iw->watches);
    dl_init (&iw->deps);

    if (S_ISDIR (st.st_mode)) {
        struct chg_list *deps = dl_listing (fd, NULL);
        if (deps == NULL) {
            perror_msg ("Directory listing of %d failed", fd);
            iwatch_free (iw);
            return NULL;
        }
        dl_join (&iw->deps, deps);
#ifdef SKIP_SUBFILES
        iw->skip_subfiles = iwatch_want_skip_subfiles (fd);
#endif
    }

    watch *parent = watch_init (iw, WATCH_USER, fd, &st);
    if (parent == NULL) {
        iwatch_free (iw);
        return NULL;
    }

    if (watch_add_dep (parent, DI_PARENT) == NULL) {
        iwatch_free (iw);
        return NULL;
    }

    watch_set_insert (&iw->watches, parent);

    if (S_ISDIR (st.st_mode)) {

        dep_item *iter;
        DL_FOREACH (iter, &iw->deps) {
            iwatch_add_subwatch (iw, iter);
        }
    }
    return iw;
}

/**
 * Free an inotify watch.
 *
 * @param[in] iw      A pointer to #i_watch to remove.
 **/
void
iwatch_free (i_watch *iw)
{
    assert (iw != NULL);

    /* unwatch subfiles */
    dep_item *iter;
    DL_FOREACH (iter, &iw->deps) {
        iwatch_del_subwatch (iw, iter);
    }

    /* unwatch parent */
    watch *w = watch_set_find (&iw->watches, iw->inode);
    if (w != NULL) {
        assert (!watch_deps_empty (w));
        watch_del_dep (w, DI_PARENT);
    }

    dl_free (&iw->deps);
    free (iw);
}

/**
 * Start watching a file or a directory.
 *
 * @param[in] iw A pointer to #i_watch.
 * @param[in] di A dependency item with relative path to watch.
 * @return A pointer to a created watch.
 **/
watch*
iwatch_add_subwatch (i_watch *iw, dep_item *di)
{
    assert (iw != NULL);
    assert (di != NULL);

    if (iw->is_closed) {
        return NULL;
    }

#ifdef SKIP_SUBFILES
    if (iw->skip_subfiles) {
        goto lstat;
    }
#endif

    watch *w = watch_set_find (&iw->watches, di->inode);
    if (w != NULL) {
        di_settype (di, w->flags);
        goto hold;
    }

    /* Don`t open a watches with empty kqueue filter flags */
    if (!S_ISUNK (di->type) &&
        inotify_to_kqueue (iw->flags, di->type, false) == 0) {
        return NULL;
    }

    int fd = watch_open (iw->fd, di->path, IN_DONT_FOLLOW);
    if (fd == -1) {
        perror_msg ("Failed to open file %s", di->path);
        goto lstat;
    }

    struct stat st;
    if (fstat (fd, &st) == -1) {
        perror_msg ("Failed to stat subwatch %s", di->path);
        close (fd);
        goto lstat;
    }

    di_settype (di, st.st_mode);

    /* Correct inode number if opened file is not a listed one */
    if (di->inode != st.st_ino) {
        if (iw->dev != st.st_dev) {
            /* It`s a mountpoint. Keep underlying directory inode number */
            st.st_ino = di->inode;
        } else {
            /* Race detected. Use new inode number and try to find watch again */
            perror_msg ("%s has been replaced after directory listing", di->path);
            di->inode = st.st_ino;
            w = watch_set_find (&iw->watches, di->inode);
            if (w != NULL) {
                close (fd);
                goto hold;
            }
        }
    }

    w = watch_init (iw, WATCH_DEPENDENCY, fd, &st);
    if (w == NULL) {
        close (fd);
        return NULL;
    }

    watch_set_insert (&iw->watches, w);

hold:
    if (watch_add_dep (w, di) == NULL && watch_deps_empty (w)) {
        watch_set_delete (&iw->watches, w);
    }
    return w;

lstat:
    if (S_ISUNK (di->type)) {
        if (fstatat (iw->fd, di->path, &st, AT_SYMLINK_NOFOLLOW) != -1) {
            di_settype (di, st.st_mode);
        } else {
            perror_msg ("Failed to lstat subwatch %s", di->path);
        }
    }
    return NULL;
}

/**
 * Remove a watch from worker by its path.
 *
 * @param[in] iw A pointer to the #i_watch.
 * @param[in] di A dependency list item to remove watch.
 **/
void
iwatch_del_subwatch (i_watch *iw, const dep_item *di)
{
    assert (iw != NULL);
    assert (di != NULL);

    watch *w = watch_set_find (&iw->watches, di->inode);
    if (w != NULL) {
        assert (!watch_deps_empty (w));
        watch_del_dep (w, di);
    }
}

/**
 * Update a inotify watch from worker by its old and new paths.
 *
 * @param[in] iw      A pointer to the #i_watch.
 * @param[in] di_from A old name & inode number of the file.
 * @param[in] di_to   A new name & inode number of the file.
 **/
void
iwatch_move_subwatch (i_watch *iw,
                      const dep_item *di_from,
                      const dep_item *di_to)
{
    assert (iw != NULL);
    assert (di_from != NULL);
    assert (di_to != NULL);
    assert (di_from->inode == di_to->inode);

    watch *w = watch_set_find (&iw->watches, di_to->inode);
    if (w != NULL && !watch_deps_empty (w)) {
        watch_chg_dep (w, di_from, di_to);
    }
}

/**
 * Update inotify watch flags.
 *
 * When called for a directory watch, update also the flags of all the
 * dependent (child) watches.
 *
 * @param[in] iw    A pointer to #i_watch.
 * @param[in] flags A combination of the inotify watch flags.
 **/
void
iwatch_update_flags (i_watch *iw, uint32_t flags)
{
    assert (iw != NULL);

    /* merge flags if IN_MASK_ADD flag is set */
    if (flags & IN_MASK_ADD) {
        flags |= iw->flags;
    }

    iw->flags = flags;

    /* update parent kqueue watch */
    watch *w = watch_set_find (&iw->watches, iw->inode);
    assert (w != NULL);
    assert (!watch_deps_empty (w));
    uint32_t fflags = inotify_to_kqueue (flags, w->flags, true);
    watch_register_event (w, fflags);

    /* update kqueue subwatches or close those we dont need to watch */
    dep_item *iter;
    DL_FOREACH (iter, &iw->deps) {
        w = watch_set_find (&iw->watches, iter->inode);
        if (w == NULL || watch_find_dep (w, iter) == NULL) {
            /* try to watch  unwatched subfiles */
            iwatch_add_subwatch (iw, iter);
        } else {
            fflags = inotify_to_kqueue (flags, w->flags, false);
            if (fflags == 0) {
                watch_del_dep (w, iter);
            } else {
                watch_register_event (w, fflags);
            }
        }
    }
}
