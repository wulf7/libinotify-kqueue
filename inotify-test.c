/*******************************************************************************
  Copyright (c) 2011 Dmitry Matveev <me@dmitrymatveev.co.uk>
  Copyright (c) 2024 Serenity Cyber Security, LLC
                     Author: Gleb Popov <arrowd@FreeBSD.org>

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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <sys/inotify.h>

void get_event (int fd, const char * target);
#ifdef IN_DIRECT
void get_event_direct (int fd, const char * target);
#endif
void handle_error (int error);

/* ----------------------------------------------------------------- */

int main (int argc, char *argv[])
{
    char target[FILENAME_MAX];
    int ev_count = 0;
    int fd;
    int wd;   /* watch descriptor */
    int flags = 0;

    struct rlimit rl;
    rl.rlim_cur = 3072;
    rl.rlim_max = 8172;
    setrlimit (RLIMIT_NOFILE, &rl);

    if (argc < 2) {
        fprintf (stderr, "Watching the current directory\n");
        strcpy (target, ".");
    }
    else {
        fprintf (stderr, "Watching %s\n", argv[1]);
        strcpy (target, argv[1]);
    }
#ifdef IN_DIRECT
    if (argc == 3 && !strcmp(argv[2], "direct"))
        flags |= IN_DIRECT;
#endif

    fd = inotify_init1(flags);
    if (fd < 0) {
        printf("inotify_init failed\n");
        handle_error (errno);
        return 1;
    }

    wd = inotify_add_watch (fd, target, IN_ALL_EVENTS);
    if (wd < 0) {
        printf("add_watch failed\n");
        handle_error (errno);
        return 1;
    }

    while (1) {
        /* if (ev_count == 3) { */
        /*     printf("stopping watch\n"); */
        /*     inotify_rm_watch (fd, wd); */
        /* } */

        ++ev_count;
#ifdef IN_DIRECT
        if (flags & IN_DIRECT)
            get_event_direct(fd, target);
        else
#endif
            get_event(fd, target);
    }

    return 0;
}

/* ----------------------------------------------------------------- */

static void dump_event (struct inotify_event *pevent, char* action)
{
    if (pevent->mask & IN_ACCESS)
        strcat(action, " was read");
    if (pevent->mask & IN_ATTRIB)
        strcat(action, " Metadata changed");
    if (pevent->mask & IN_CLOSE_WRITE)
        strcat(action, " opened for writing was closed");
    if (pevent->mask & IN_CLOSE_NOWRITE)
        strcat(action, " not opened for writing was closed");
    if (pevent->mask & IN_CREATE)
        strcat(action, " created in watched directory");
    if (pevent->mask & IN_DELETE)
        strcat(action, " deleted from watched directory");
    if (pevent->mask & IN_DELETE_SELF)
        strcat(action, " watched file/directory was itself deleted");
    if (pevent->mask & IN_MODIFY)
        strcat(action, " was modified");
    if (pevent->mask & IN_MOVE_SELF)
        strcat(action, " watched file/directory was itself moved");
    if (pevent->mask & IN_MOVED_FROM)
        strcat(action, " moved out of watched directory");
    if (pevent->mask & IN_MOVED_TO)
        strcat(action, " moved into watched directory");
    if (pevent->mask & IN_OPEN)
        strcat(action, " was opened");
    if (pevent->mask & IN_IGNORED)
        strcat(action, " was ignored");
    if (pevent->mask & IN_UNMOUNT)
        strcat(action, " was unmounted");

    /*
        printf ("wd=%d mask=%x cookie=%d len=%d dir=%s\n",
        pevent->wd, pevent->mask, pevent->cookie, pevent->len,
        (pevent->mask & IN_ISDIR)?"yes":"no");

        if (pevent->len) printf ("name=%s\n", pevent->name);
    */

    printf ("%s [%s]\n", action, pevent->len ? pevent->name : "");
}

#define BUFF_SIZE (16*1024)

void get_event (int fd, const char * target)
{
    ssize_t len, i = 0;
    char buff[BUFF_SIZE] = {0};

    len = read (fd, buff, BUFF_SIZE);

    while (i < len) {
        struct inotify_event *pevent = (struct inotify_event *)&buff[i];
        char action[81+FILENAME_MAX] = {0};

        if (pevent->len)
            strcpy (action, pevent->name);
        else
            strcpy (action, target);

        dump_event (pevent, action);

        i += sizeof(struct inotify_event) + pevent->len;

    }

}  /* get_event */

#ifdef IN_DIRECT
void get_event_direct (int fd, const char * target)
{
    struct iovec *received[5];
    int num_events = libinotify_direct_readv (fd, received, 5, 0);

    for (int i = 0; i < num_events; i++) {
        struct iovec *cur_event = received[i];
        while (cur_event->iov_base) {
            struct inotify_event *pevent = (struct inotify_event *) cur_event->iov_base;
            char action[81+FILENAME_MAX] = {0};

            if (pevent->len)
                strcpy (action, pevent->name);
            else
                strcpy (action, target);

            dump_event (pevent, action);

            cur_event++;

        }
        libinotify_free_iovec (received[i]);
    }
}  /* get_event */
#endif

/* ----------------------------------------------------------------- */

void handle_error (int error)
{
    fprintf (stderr, "Error: %s\n", strerror(error));

}  /* handle_error */

/* ----------------------------------------------------------------- */
