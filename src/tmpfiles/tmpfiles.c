/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering, Kay Sievers

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <getopt.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <glob.h>
#include <fnmatch.h>
#include <sys/capability.h>

#include "log.h"
#include "util.h"
#include "macro.h"
#include "missing.h"
#include "mkdir.h"
#include "path-util.h"
#include "strv.h"
#include "label.h"
#include "set.h"
#include "conf-files.h"
#include "capability.h"

/* This reads all files listed in /etc/tmpfiles.d/?*.conf and creates
 * them in the file system. This is intended to be used to create
 * properly owned directories beneath /tmp, /var/tmp, /run, which are
 * volatile and hence need to be recreated on bootup. */

typedef enum ItemType {
        /* These ones take file names */
        CREATE_FILE = 'f',
        TRUNCATE_FILE = 'F',
        WRITE_FILE = 'w',
        CREATE_DIRECTORY = 'd',
        TRUNCATE_DIRECTORY = 'D',
        CREATE_FIFO = 'p',
        CREATE_SYMLINK = 'L',
        CREATE_CHAR_DEVICE = 'c',
        CREATE_BLOCK_DEVICE = 'b',

        /* These ones take globs */
        IGNORE_PATH = 'x',
        IGNORE_DIRECTORY_PATH = 'X',
        REMOVE_PATH = 'r',
        RECURSIVE_REMOVE_PATH = 'R',
        RELABEL_PATH = 'z',
        RECURSIVE_RELABEL_PATH = 'Z'
} ItemType;

typedef struct Item {
        ItemType type;

        char *path;
        char *argument;
        uid_t uid;
        gid_t gid;
        mode_t mode;
        usec_t age;

        dev_t major_minor;

        bool uid_set:1;
        bool gid_set:1;
        bool mode_set:1;
        bool age_set:1;

        bool keep_first_level:1;
} Item;

static Hashmap *items = NULL, *globs = NULL;
static Set *unix_sockets = NULL;

static bool arg_create = false;
static bool arg_clean = false;
static bool arg_remove = false;

static const char *arg_prefix = NULL;

static const char conf_file_dirs[] =
        "/etc/tmpfiles.d\0"
        "/run/tmpfiles.d\0"
        "/usr/local/lib/tmpfiles.d\0"
        "/usr/lib/tmpfiles.d\0"
#ifdef HAVE_SPLIT_USR
        "/lib/tmpfiles.d\0"
#endif
        ;

#define MAX_DEPTH 256

static bool needs_glob(ItemType t) {
        return t == IGNORE_PATH || t == IGNORE_DIRECTORY_PATH || t == REMOVE_PATH || t == RECURSIVE_REMOVE_PATH || t == RELABEL_PATH || t == RECURSIVE_RELABEL_PATH;
}

static struct Item* find_glob(Hashmap *h, const char *match) {
        Item *j;
        Iterator i;

        HASHMAP_FOREACH(j, h, i)
                if (fnmatch(j->path, match, FNM_PATHNAME|FNM_PERIOD) == 0)
                        return j;

        return NULL;
}

static void load_unix_sockets(void) {
        FILE _cleanup_fclose_ *f = NULL;
        char line[LINE_MAX];

        if (unix_sockets)
                return;

        /* We maintain a cache of the sockets we found in
         * /proc/net/unix to speed things up a little. */

        unix_sockets = set_new(string_hash_func, string_compare_func);
        if (!unix_sockets)
                return;

        f = fopen("/proc/net/unix", "re");
        if (!f)
                return;

        /* Skip header */
        if (!fgets(line, sizeof(line), f))
                goto fail;

        for (;;) {
                char *p, *s;
                int k;

                if (!fgets(line, sizeof(line), f))
                        break;

                truncate_nl(line);

                p = strchr(line, ':');
                if (!p)
                        continue;

                if (strlen(p) < 37)
                        continue;

                p += 37;
                p += strspn(p, WHITESPACE);
                p += strcspn(p, WHITESPACE); /* skip one more word */
                p += strspn(p, WHITESPACE);

                if (*p != '/')
                        continue;

                s = strdup(p);
                if (!s)
                        goto fail;

                path_kill_slashes(s);

                k = set_put(unix_sockets, s);
                if (k < 0) {
                        free(s);

                        if (k != -EEXIST)
                                goto fail;
                }
        }

        return;

fail:
        set_free_free(unix_sockets);
        unix_sockets = NULL;
}

static bool unix_socket_alive(const char *fn) {
        assert(fn);

        load_unix_sockets();

        if (unix_sockets)
                return !!set_get(unix_sockets, (char*) fn);

        /* We don't know, so assume yes */
        return true;
}

static int dir_is_mount_point(DIR *d, const char *subdir) {
        struct file_handle *h;
        int mount_id_parent, mount_id;
        int r_p, r;

        h = alloca(MAX_HANDLE_SZ);

        h->handle_bytes = MAX_HANDLE_SZ;
        r_p = name_to_handle_at(dirfd(d), ".", h, &mount_id_parent, 0);
        if (r_p < 0)
                r_p = -errno;

        h->handle_bytes = MAX_HANDLE_SZ;
        r = name_to_handle_at(dirfd(d), subdir, h, &mount_id, 0);
        if (r < 0)
                r = -errno;

        /* got no handle; make no assumptions, return error */
        if (r_p < 0 && r < 0)
                return r_p;

        /* got both handles; if they differ, it is a mount point */
        if (r_p >= 0 && r >= 0)
                return mount_id_parent != mount_id;

        /* got only one handle; assume different mount points if one
         * of both queries was not supported by the filesystem */
        if (r_p == -ENOSYS || r_p == -ENOTSUP || r == -ENOSYS || r == -ENOTSUP)
                return true;

        /* return error */
        if (r_p < 0)
                return r_p;
        return r;
}

static int dir_cleanup(
                Item *i,
                const char *p,
                DIR *d,
                const struct stat *ds,
                usec_t cutoff,
                dev_t rootdev,
                bool mountpoint,
                int maxdepth,
                bool keep_this_level)
{
        struct dirent *dent;
        struct timespec times[2];
        bool deleted = false;
        int r = 0;

        while ((dent = readdir(d))) {
                struct stat s;
                usec_t age;
                char _cleanup_free_ *sub_path = NULL;

                if (streq(dent->d_name, ".") ||
                    streq(dent->d_name, ".."))
                        continue;

                if (fstatat(dirfd(d), dent->d_name, &s, AT_SYMLINK_NOFOLLOW) < 0) {

                        if (errno != ENOENT) {
                                log_error("stat(%s/%s) failed: %m", p, dent->d_name);
                                r = -errno;
                        }

                        continue;
                }

                /* Stay on the same filesystem */
                if (s.st_dev != rootdev)
                        continue;

                /* Try to detect bind mounts of the same filesystem instance; they
                 * do not differ in device major/minors. This type of query is not
                 * supported on all kernels or filesystem types though. */
                if (S_ISDIR(s.st_mode) && dir_is_mount_point(d, dent->d_name) > 0)
                        continue;

                /* Do not delete read-only files owned by root */
                if (s.st_uid == 0 && !(s.st_mode & S_IWUSR))
                        continue;

                if (asprintf(&sub_path, "%s/%s", p, dent->d_name) < 0) {
                        r = log_oom();
                        goto finish;
                }

                /* Is there an item configured for this path? */
                if (hashmap_get(items, sub_path))
                        continue;

                if (find_glob(globs, sub_path))
                        continue;

                if (S_ISDIR(s.st_mode)) {

                        if (mountpoint &&
                            streq(dent->d_name, "lost+found") &&
                            s.st_uid == 0)
                                continue;

                        if (maxdepth <= 0)
                                log_warning("Reached max depth on %s.", sub_path);
                        else {
                                DIR _cleanup_closedir_ *sub_dir;
                                int q;

                                sub_dir = xopendirat(dirfd(d), dent->d_name, O_NOFOLLOW|O_NOATIME);
                                if (sub_dir == NULL) {
                                        if (errno != ENOENT) {
                                                log_error("opendir(%s/%s) failed: %m", p, dent->d_name);
                                                r = -errno;
                                        }

                                        continue;
                                }

                                q = dir_cleanup(i, sub_path, sub_dir, &s, cutoff, rootdev, false, maxdepth-1, false);

                                if (q < 0)
                                        r = q;
                        }

                        /* Note: if you are wondering why we don't
                         * support the sticky bit for excluding
                         * directories from cleaning like we do it for
                         * other file system objects: well, the sticky
                         * bit already has a meaning for directories,
                         * so we don't want to overload that. */

                        if (keep_this_level)
                                continue;

                        /* Ignore ctime, we change it when deleting */
                        age = MAX(timespec_load(&s.st_mtim),
                                  timespec_load(&s.st_atim));
                        if (age >= cutoff)
                                continue;

                        if (i->type != IGNORE_DIRECTORY_PATH || !streq(dent->d_name, p)) {
                                log_debug("rmdir '%s'\n", sub_path);

                                if (unlinkat(dirfd(d), dent->d_name, AT_REMOVEDIR) < 0) {
                                        if (errno != ENOENT && errno != ENOTEMPTY) {
                                                log_error("rmdir(%s): %m", sub_path);
                                                r = -errno;
                                        }
                                }
                        }

                } else {
                        /* Skip files for which the sticky bit is
                         * set. These are semantics we define, and are
                         * unknown elsewhere. See XDG_RUNTIME_DIR
                         * specification for details. */
                        if (s.st_mode & S_ISVTX)
                                continue;

                        if (mountpoint && S_ISREG(s.st_mode)) {
                                if (streq(dent->d_name, ".journal") &&
                                    s.st_uid == 0)
                                        continue;

                                if (streq(dent->d_name, "aquota.user") ||
                                    streq(dent->d_name, "aquota.group"))
                                        continue;
                        }

                        /* Ignore sockets that are listed in /proc/net/unix */
                        if (S_ISSOCK(s.st_mode) && unix_socket_alive(sub_path))
                                continue;

                        /* Ignore device nodes */
                        if (S_ISCHR(s.st_mode) || S_ISBLK(s.st_mode))
                                continue;

                        /* Keep files on this level around if this is
                         * requested */
                        if (keep_this_level)
                                continue;

                        age = MAX3(timespec_load(&s.st_mtim),
                                   timespec_load(&s.st_atim),
                                   timespec_load(&s.st_ctim));

                        if (age >= cutoff)
                                continue;

                        log_debug("unlink '%s'\n", sub_path);

                        if (unlinkat(dirfd(d), dent->d_name, 0) < 0) {
                                if (errno != ENOENT) {
                                        log_error("unlink(%s): %m", sub_path);
                                        r = -errno;
                                }
                        }

                        deleted = true;
                }
        }

finish:
        if (deleted) {
                /* Restore original directory timestamps */
                times[0] = ds->st_atim;
                times[1] = ds->st_mtim;

                if (futimens(dirfd(d), times) < 0)
                        log_error("utimensat(%s): %m", p);
        }

        return r;
}

static int item_set_perms(Item *i, const char *path) {
        /* not using i->path directly because it may be a glob */
        if (i->mode_set)
                if (chmod(path, i->mode) < 0) {
                        log_error("chmod(%s) failed: %m", path);
                        return -errno;
                }

        if (i->uid_set || i->gid_set)
                if (chown(path,
                          i->uid_set ? i->uid : (uid_t) -1,
                          i->gid_set ? i->gid : (gid_t) -1) < 0) {

                        log_error("chown(%s) failed: %m", path);
                        return -errno;
                }

        return label_fix(path, false, false);
}

static int write_one_file(Item *i, const char *path) {
        int r, e, fd, flags;
        struct stat st;
        mode_t u;

        flags = i->type == CREATE_FILE ? O_CREAT|O_APPEND :
                i->type == TRUNCATE_FILE ? O_CREAT|O_TRUNC : 0;

        u = umask(0);
        label_context_set(path, S_IFREG);
        fd = open(path, flags|O_NDELAY|O_CLOEXEC|O_WRONLY|O_NOCTTY|O_NOFOLLOW, i->mode);
        e = errno;
        label_context_clear();
        umask(u);
        errno = e;

        if (fd < 0) {
                if (i->type == WRITE_FILE && errno == ENOENT)
                        return 0;

                log_error("Failed to create file %s: %m", path);
                return -errno;
        }

        if (i->argument) {
                ssize_t n;
                size_t l;
                _cleanup_free_ char *unescaped;

                unescaped = cunescape(i->argument);
                if (unescaped == NULL) {
                        close_nointr_nofail(fd);
                        return log_oom();
                }

                l = strlen(unescaped);
                n = write(fd, unescaped, l);

                if (n < 0 || (size_t) n < l) {
                        log_error("Failed to write file %s: %s", path, n < 0 ? strerror(-n) : "Short write");
                        close_nointr_nofail(fd);
                        return n < 0 ? n : -EIO;
                }
        }

        close_nointr_nofail(fd);

        if (stat(path, &st) < 0) {
                log_error("stat(%s) failed: %m", path);
                return -errno;
        }

        if (!S_ISREG(st.st_mode)) {
                log_error("%s is not a file.", path);
                return -EEXIST;
        }

        r = item_set_perms(i, path);
        if (r < 0)
                return r;

        return 0;
}

static int recursive_relabel_children(Item *i, const char *path) {
        DIR _cleanup_closedir_ *d;
        int ret = 0;

        /* This returns the first error we run into, but nevertheless
         * tries to go on */

        d = opendir(path);
        if (!d)
                return errno == ENOENT ? 0 : -errno;

        for (;;) {
                struct dirent *de;
                union dirent_storage buf;
                bool is_dir;
                int r;
                char _cleanup_free_ *entry_path = NULL;

                r = readdir_r(d, &buf.de, &de);
                if (r != 0) {
                        if (ret == 0)
                                ret = -r;
                        break;
                }

                if (!de)
                        break;

                if (streq(de->d_name, ".") || streq(de->d_name, ".."))
                        continue;

                if (asprintf(&entry_path, "%s/%s", path, de->d_name) < 0) {
                        if (ret == 0)
                                ret = -ENOMEM;
                        continue;
                }

                if (de->d_type == DT_UNKNOWN) {
                        struct stat st;

                        if (lstat(entry_path, &st) < 0) {
                                if (ret == 0 && errno != ENOENT)
                                        ret = -errno;
                                continue;
                        }

                        is_dir = S_ISDIR(st.st_mode);

                } else
                        is_dir = de->d_type == DT_DIR;

                r = item_set_perms(i, entry_path);
                if (r < 0) {
                        if (ret == 0 && r != -ENOENT)
                                ret = r;
                        continue;
                }

                if (is_dir) {
                        r = recursive_relabel_children(i, entry_path);
                        if (r < 0 && ret == 0)
                                ret = r;
                }
        }

        return ret;
}

static int recursive_relabel(Item *i, const char *path) {
        int r;
        struct stat st;

        r = item_set_perms(i, path);
        if (r < 0)
                return r;

        if (lstat(path, &st) < 0)
                return -errno;

        if (S_ISDIR(st.st_mode))
                r = recursive_relabel_children(i, path);

        return r;
}

static int glob_item(Item *i, int (*action)(Item *, const char *)) {
        int r = 0, k;
        glob_t g;
        char **fn;

        zero(g);

        errno = 0;
        if ((k = glob(i->path, GLOB_NOSORT|GLOB_BRACE, NULL, &g)) != 0) {

                if (k != GLOB_NOMATCH) {
                        if (errno != 0)
                                errno = EIO;

                        log_error("glob(%s) failed: %m", i->path);
                        return -errno;
                }
        }

        STRV_FOREACH(fn, g.gl_pathv)
                if ((k = action(i, *fn)) < 0)
                        r = k;

        globfree(&g);
        return r;
}

static int create_item(Item *i) {
        int r, e;
        mode_t u;
        struct stat st;

        assert(i);

        switch (i->type) {

        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
                return 0;

        case CREATE_FILE:
        case TRUNCATE_FILE:
                r = write_one_file(i, i->path);
                if (r < 0)
                        return r;
                break;
        case WRITE_FILE:
                r = glob_item(i, write_one_file);
                if (r < 0)
                        return r;

                break;

        case TRUNCATE_DIRECTORY:
        case CREATE_DIRECTORY:

                u = umask(0);
                mkdir_parents_label(i->path, 0755);
                r = mkdir(i->path, i->mode);
                umask(u);

                if (r < 0 && errno != EEXIST) {
                        log_error("Failed to create directory %s: %m", i->path);
                        return -errno;
                }

                if (stat(i->path, &st) < 0) {
                        log_error("stat(%s) failed: %m", i->path);
                        return -errno;
                }

                if (!S_ISDIR(st.st_mode)) {
                        log_error("%s is not a directory.", i->path);
                        return -EEXIST;
                }

                r = item_set_perms(i, i->path);
                if (r < 0)
                        return r;

                break;

        case CREATE_FIFO:

                u = umask(0);
                r = mkfifo(i->path, i->mode);
                umask(u);

                if (r < 0 && errno != EEXIST) {
                        log_error("Failed to create fifo %s: %m", i->path);
                        return -errno;
                }

                if (stat(i->path, &st) < 0) {
                        log_error("stat(%s) failed: %m", i->path);
                        return -errno;
                }

                if (!S_ISFIFO(st.st_mode)) {
                        log_error("%s is not a fifo.", i->path);
                        return -EEXIST;
                }

                r = item_set_perms(i, i->path);
                if (r < 0)
                        return r;

                break;

        case CREATE_SYMLINK: {
                char *x;

                label_context_set(i->path, S_IFLNK);
                r = symlink(i->argument, i->path);
                e = errno;
                label_context_clear();
                errno = e;

                if (r < 0 && errno != EEXIST) {
                        log_error("symlink(%s, %s) failed: %m", i->argument, i->path);
                        return -errno;
                }

                r = readlink_malloc(i->path, &x);
                if (r < 0) {
                        log_error("readlink(%s) failed: %s", i->path, strerror(-r));
                        return -errno;
                }

                if (!streq(i->argument, x)) {
                        free(x);
                        log_error("%s is not the right symlinks.", i->path);
                        return -EEXIST;
                }

                free(x);
                break;
        }

        case CREATE_BLOCK_DEVICE:
        case CREATE_CHAR_DEVICE: {
                mode_t file_type;

                if (have_effective_cap(CAP_MKNOD) == 0) {
                        /* In a container we lack CAP_MKNOD. We
                        shouldnt attempt to create the device node in
                        that case to avoid noise, and we don't support
                        virtualized devices in containers anyway. */

                        log_debug("We lack CAP_MKNOD, skipping creation of device node %s.", i->path);
                        return 0;
                }

                file_type = (i->type == CREATE_BLOCK_DEVICE ? S_IFBLK : S_IFCHR);

                u = umask(0);
                label_context_set(i->path, file_type);
                r = mknod(i->path, i->mode | file_type, i->major_minor);
                e = errno;
                label_context_clear();
                umask(u);
                errno = e;

                if (r < 0 && errno != EEXIST) {
                        log_error("Failed to create device node %s: %m", i->path);
                        return -errno;
                }

                if (stat(i->path, &st) < 0) {
                        log_error("stat(%s) failed: %m", i->path);
                        return -errno;
                }

                if ((st.st_mode & S_IFMT) != file_type) {
                        log_error("%s is not a device node.", i->path);
                        return -EEXIST;
                }

                r = item_set_perms(i, i->path);
                if (r < 0)
                        return r;

                break;
        }

        case RELABEL_PATH:

                r = glob_item(i, item_set_perms);
                if (r < 0)
                        return 0;
                break;

        case RECURSIVE_RELABEL_PATH:

                r = glob_item(i, recursive_relabel);
                if (r < 0)
                        return r;
        }

        log_debug("%s created successfully.", i->path);

        return 0;
}

static int remove_item_instance(Item *i, const char *instance) {
        int r;

        assert(i);

        switch (i->type) {

        case CREATE_FILE:
        case TRUNCATE_FILE:
        case CREATE_DIRECTORY:
        case CREATE_FIFO:
        case CREATE_SYMLINK:
        case CREATE_BLOCK_DEVICE:
        case CREATE_CHAR_DEVICE:
        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case RELABEL_PATH:
        case RECURSIVE_RELABEL_PATH:
        case WRITE_FILE:
                break;

        case REMOVE_PATH:
                if (remove(instance) < 0 && errno != ENOENT) {
                        log_error("remove(%s): %m", instance);
                        return -errno;
                }

                break;

        case TRUNCATE_DIRECTORY:
        case RECURSIVE_REMOVE_PATH:
                /* FIXME: we probably should use dir_cleanup() here
                 * instead of rm_rf() so that 'x' is honoured. */
                r = rm_rf_dangerous(instance, false, i->type == RECURSIVE_REMOVE_PATH, false);
                if (r < 0 && r != -ENOENT) {
                        log_error("rm_rf(%s): %s", instance, strerror(-r));
                        return r;
                }

                break;
        }

        return 0;
}

static int remove_item(Item *i) {
        int r = 0;

        assert(i);

        switch (i->type) {

        case CREATE_FILE:
        case TRUNCATE_FILE:
        case CREATE_DIRECTORY:
        case CREATE_FIFO:
        case CREATE_SYMLINK:
        case CREATE_CHAR_DEVICE:
        case CREATE_BLOCK_DEVICE:
        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case RELABEL_PATH:
        case RECURSIVE_RELABEL_PATH:
        case WRITE_FILE:
                break;

        case REMOVE_PATH:
        case TRUNCATE_DIRECTORY:
        case RECURSIVE_REMOVE_PATH:
                r = glob_item(i, remove_item_instance);
                break;
        }

        return r;
}

static int clean_item_instance(Item *i, const char* instance) {
        DIR _cleanup_closedir_ *d = NULL;
        struct stat s, ps;
        bool mountpoint;
        int r;
        usec_t cutoff, n;

        assert(i);

        if (!i->age_set)
                return 0;

        n = now(CLOCK_REALTIME);
        if (n < i->age)
                return 0;

        cutoff = n - i->age;

        d = opendir(instance);
        if (!d) {
                if (errno == ENOENT || errno == ENOTDIR)
                        return 0;

                log_error("Failed to open directory %s: %m", i->path);
                return -errno;
        }

        if (fstat(dirfd(d), &s) < 0) {
                log_error("stat(%s) failed: %m", i->path);
                return -errno;
        }

        if (!S_ISDIR(s.st_mode)) {
                log_error("%s is not a directory.", i->path);
                return -ENOTDIR;
        }

        if (fstatat(dirfd(d), "..", &ps, AT_SYMLINK_NOFOLLOW) != 0) {
                log_error("stat(%s/..) failed: %m", i->path);
                return -errno;
        }

        mountpoint = s.st_dev != ps.st_dev ||
                     (s.st_dev == ps.st_dev && s.st_ino == ps.st_ino);

        r = dir_cleanup(i, instance, d, &s, cutoff, s.st_dev, mountpoint,
                        MAX_DEPTH, i->keep_first_level);
        return r;
}

static int clean_item(Item *i) {
        int r = 0;

        assert(i);

        switch (i->type) {
        case CREATE_DIRECTORY:
        case TRUNCATE_DIRECTORY:
        case IGNORE_PATH:
                clean_item_instance(i, i->path);
                break;
        case IGNORE_DIRECTORY_PATH:
                r = glob_item(i, clean_item_instance);
                break;
        default:
                break;
        }

        return r;
}

static int process_item(Item *i) {
        int r, q, p;

        assert(i);

        r = arg_create ? create_item(i) : 0;
        q = arg_remove ? remove_item(i) : 0;
        p = arg_clean ? clean_item(i) : 0;

        if (r < 0)
                return r;

        if (q < 0)
                return q;

        return p;
}

static void item_free(Item *i) {
        assert(i);

        free(i->path);
        free(i->argument);
        free(i);
}

static bool item_equal(Item *a, Item *b) {
        assert(a);
        assert(b);

        if (!streq_ptr(a->path, b->path))
                return false;

        if (a->type != b->type)
                return false;

        if (a->uid_set != b->uid_set ||
            (a->uid_set && a->uid != b->uid))
            return false;

        if (a->gid_set != b->gid_set ||
            (a->gid_set && a->gid != b->gid))
            return false;

        if (a->mode_set != b->mode_set ||
            (a->mode_set && a->mode != b->mode))
            return false;

        if (a->age_set != b->age_set ||
            (a->age_set && a->age != b->age))
            return false;

        if ((a->type == CREATE_FILE ||
             a->type == TRUNCATE_FILE ||
             a->type == WRITE_FILE ||
             a->type == CREATE_SYMLINK) &&
            !streq_ptr(a->argument, b->argument))
                return false;

        if ((a->type == CREATE_CHAR_DEVICE ||
             a->type == CREATE_BLOCK_DEVICE) &&
            a->major_minor != b->major_minor)
                return false;

        return true;
}

static int parse_line(const char *fname, unsigned line, const char *buffer) {
        Item *i, *existing;
        char _cleanup_free_
                *mode = NULL, *user = NULL, *group = NULL, *age = NULL;
        char type;
        Hashmap *h;
        int r, n = -1;

        assert(fname);
        assert(line >= 1);
        assert(buffer);

        i = new0(Item, 1);
        if (!i)
                return log_oom();

        r = sscanf(buffer,
                   "%c %ms %ms %ms %ms %ms %n",
                   &type,
                   &i->path,
                   &mode,
                   &user,
                   &group,
                   &age,
                   &n);
        if (r < 2) {
                log_error("[%s:%u] Syntax error.", fname, line);
                r = -EIO;
                goto finish;
        }

        if (n >= 0)  {
                n += strspn(buffer+n, WHITESPACE);
                if (buffer[n] != 0 && (buffer[n] != '-' || buffer[n+1] != 0)) {
                        i->argument = unquote(buffer+n, "\"");
                        if (!i->argument)
                                return log_oom();
                }
        }

        switch(type) {

        case CREATE_FILE:
        case TRUNCATE_FILE:
        case CREATE_DIRECTORY:
        case TRUNCATE_DIRECTORY:
        case CREATE_FIFO:
        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
        case RELABEL_PATH:
        case RECURSIVE_RELABEL_PATH:
                break;

        case CREATE_SYMLINK:
                if (!i->argument) {
                        log_error("[%s:%u] Symlink file requires argument.", fname, line);
                        r = -EBADMSG;
                        goto finish;
                }
                break;

        case WRITE_FILE:
                if (!i->argument) {
                        log_error("[%s:%u] Write file requires argument.", fname, line);
                        r = -EBADMSG;
                        goto finish;
                }
                break;

        case CREATE_CHAR_DEVICE:
        case CREATE_BLOCK_DEVICE: {
                unsigned major, minor;

                if (!i->argument) {
                        log_error("[%s:%u] Device file requires argument.", fname, line);
                        r = -EBADMSG;
                        goto finish;
                }

                if (sscanf(i->argument, "%u:%u", &major, &minor) != 2) {
                        log_error("[%s:%u] Can't parse device file major/minor '%s'.", fname, line, i->argument);
                        r = -EBADMSG;
                        goto finish;
                }

                i->major_minor = makedev(major, minor);
                break;
        }

        default:
                log_error("[%s:%u] Unknown file type '%c'.", fname, line, type);
                r = -EBADMSG;
                goto finish;
        }

        i->type = type;

        if (!path_is_absolute(i->path)) {
                log_error("[%s:%u] Path '%s' not absolute.", fname, line, i->path);
                r = -EBADMSG;
                goto finish;
        }

        path_kill_slashes(i->path);

        if (arg_prefix && !path_startswith(i->path, arg_prefix)) {
                r = 0;
                goto finish;
        }

        if (user && !streq(user, "-")) {
                const char *u = user;

                r = get_user_creds(&u, &i->uid, NULL, NULL, NULL);
                if (r < 0) {
                        log_error("[%s:%u] Unknown user '%s'.", fname, line, user);
                        goto finish;
                }

                i->uid_set = true;
        }

        if (group && !streq(group, "-")) {
                const char *g = group;

                r = get_group_creds(&g, &i->gid);
                if (r < 0) {
                        log_error("[%s:%u] Unknown group '%s'.", fname, line, group);
                        goto finish;
                }

                i->gid_set = true;
        }

        if (mode && !streq(mode, "-")) {
                unsigned m;

                if (sscanf(mode, "%o", &m) != 1) {
                        log_error("[%s:%u] Invalid mode '%s'.", fname, line, mode);
                        r = -ENOENT;
                        goto finish;
                }

                i->mode = m;
                i->mode_set = true;
        } else
                i->mode =
                        i->type == CREATE_DIRECTORY ||
                        i->type == TRUNCATE_DIRECTORY ? 0755 : 0644;

        if (age && !streq(age, "-")) {
                const char *a = age;

                if (*a == '~') {
                        i->keep_first_level = true;
                        a++;
                }

                if (parse_usec(a, &i->age) < 0) {
                        log_error("[%s:%u] Invalid age '%s'.", fname, line, age);
                        r = -EBADMSG;
                        goto finish;
                }

                i->age_set = true;
        }

        h = needs_glob(i->type) ? globs : items;

        existing = hashmap_get(h, i->path);
        if (existing) {

                /* Two identical items are fine */
                if (!item_equal(existing, i))
                        log_warning("Two or more conflicting lines for %s configured, ignoring.", i->path);

                r = 0;
                goto finish;
        }

        r = hashmap_put(h, i->path, i);
        if (r < 0) {
                log_error("Failed to insert item %s: %s", i->path, strerror(-r));
                goto finish;
        }

        i = NULL;
        r = 0;

finish:
        if (i)
                item_free(i);

        return r;
}

static int help(void) {

        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Creates, deletes and cleans up volatile and temporary files and directories.\n\n"
               "  -h --help             Show this help\n"
               "     --create           Create marked files/directories\n"
               "     --clean            Clean up marked directories\n"
               "     --remove           Remove marked files/directories\n"
               "     --prefix=PATH      Only apply rules that apply to paths with the specified prefix\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_CREATE,
                ARG_CLEAN,
                ARG_REMOVE,
                ARG_PREFIX
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "create",    no_argument,       NULL, ARG_CREATE    },
                { "clean",     no_argument,       NULL, ARG_CLEAN     },
                { "remove",    no_argument,       NULL, ARG_REMOVE    },
                { "prefix",    required_argument, NULL, ARG_PREFIX    },
                { NULL,        0,                 NULL, 0             }
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_CREATE:
                        arg_create = true;
                        break;

                case ARG_CLEAN:
                        arg_clean = true;
                        break;

                case ARG_REMOVE:
                        arg_remove = true;
                        break;

                case ARG_PREFIX:
                        arg_prefix = optarg;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }
        }

        if (!arg_clean && !arg_create && !arg_remove) {
                log_error("You need to specify at least one of --clean, --create or --remove.");
                return -EINVAL;
        }

        return 1;
}

static int read_config_file(const char *fn, bool ignore_enoent) {
        FILE *f;
        unsigned v = 0;
        int r;
        Iterator iterator;
        Item *i;

        assert(fn);

        r = search_and_fopen_nulstr(fn, "re", conf_file_dirs, &f);
        if (r < 0) {
                if (ignore_enoent && r == -ENOENT)
                        return 0;

                log_error("Failed to open '%s', ignoring: %s", fn, strerror(-r));
                return r;
        }

        log_debug("apply: %s\n", fn);
        for (;;) {
                char line[LINE_MAX], *l;
                int k;

                if (!(fgets(line, sizeof(line), f)))
                        break;

                v++;

                l = strstrip(line);
                if (*l == '#' || *l == 0)
                        continue;

                if ((k = parse_line(fn, v, l)) < 0)
                        if (r == 0)
                                r = k;
        }

        /* we have to determine age parameter for each entry of type X */
        HASHMAP_FOREACH(i, globs, iterator) {
                Iterator iter;
                Item *j, *candidate_item = NULL;

                if (i->type != IGNORE_DIRECTORY_PATH)
                        continue;

                HASHMAP_FOREACH(j, items, iter) {
                        if (j->type != CREATE_DIRECTORY && j->type != TRUNCATE_DIRECTORY)
                                continue;

                        if (path_equal(j->path, i->path)) {
                                candidate_item = j;
                                break;
                        }

                        if ((!candidate_item && path_startswith(i->path, j->path)) ||
                            (candidate_item && path_startswith(j->path, candidate_item->path) && (fnmatch(i->path, j->path, FNM_PATHNAME | FNM_PERIOD) == 0)))
                                candidate_item = j;
                }

                if (candidate_item) {
                        i->age = candidate_item->age;
                        i->age_set = true;
                }
        }

        if (ferror(f)) {
                log_error("Failed to read from file %s: %m", fn);
                if (r == 0)
                        r = -EIO;
        }

        fclose(f);

        return r;
}

int main(int argc, char *argv[]) {
        int r, k;
        Item *i;
        Iterator iterator;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        label_init(NULL);

        items = hashmap_new(string_hash_func, string_compare_func);
        globs = hashmap_new(string_hash_func, string_compare_func);

        if (!items || !globs) {
                r = log_oom();
                goto finish;
        }

        r = 0;

        if (optind < argc) {
                int j;

                for (j = optind; j < argc; j++) {
                        k = read_config_file(argv[j], false);
                        if (k < 0 && r == 0)
                                r = k;
                }

        } else {
                _cleanup_strv_free_ char **files = NULL;
                char **f;

                r = conf_files_list_nulstr(&files, ".conf", NULL, conf_file_dirs);
                if (r < 0) {
                        log_error("Failed to enumerate tmpfiles.d files: %s", strerror(-r));
                        goto finish;
                }

                STRV_FOREACH(f, files) {
                        k = read_config_file(*f, true);
                        if (k < 0 && r == 0)
                                r = k;
                }
        }

        HASHMAP_FOREACH(i, globs, iterator)
                process_item(i);

        HASHMAP_FOREACH(i, items, iterator)
                process_item(i);

finish:
        while ((i = hashmap_steal_first(items)))
                item_free(i);

        while ((i = hashmap_steal_first(globs)))
                item_free(i);

        hashmap_free(items);
        hashmap_free(globs);

        set_free_free(unix_sockets);

        label_finish();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
