/*
 * Copyright (c) 2005, 2006 Miek Gieben
 * See LICENSE for the license
 *
 * Directory crawler
 */
#include "rdup.h"

extern gboolean opt_onefilesystem;
extern gboolean opt_nobackup;
extern gboolean opt_attr;
extern gint opt_verbose;

static struct entry *
entry_dup(struct entry *f)
{
        struct entry *g;
        g = g_malloc(sizeof(struct entry));

        g->f_name       = g_strdup(f->f_name);
        g->f_name_size  = f->f_name_size;
        g->f_uid        = f->f_uid;
        g->f_gid        = f->f_gid;
        g->f_mode       = f->f_mode;
	g->f_mtime      = f->f_mtime;
	g->f_size       = f->f_size;
        return g;
}

static void
entry_free(struct entry *f)
{
	g_free(f->f_name);
	g_free(f);
}

static uid_t
read_attr_uid(__attribute__((unused))
	char *path, __attribute__((unused)) uid_t u)
{
#ifdef HAVE_ATTR_XATTR_H
	/* linux */
	char buf[ATTR_SIZE + 1];
	uid_t x;
	int r;

	if ((r = lgetxattr(path, "user.r_uid", buf, ATTR_SIZE)) > 0) {
		x = (uid_t)atoi(buf);
		buf[r - 1] = '\0';
		if (x > R_MAX_ID) {
			msg("Too large uid `%zd\' for `%s\', truncating", (size_t)x, 
				path);
			return R_MAX_ID;
		}
		return x;
	} else {
		if (opt_verbose > 0) {
			msg("No uid xattr for `%s\'", path);
		}
		return u;
	}
#elif HAVE_OPENAT
	/* solaris */
	char buf[ATTR_SIZE + 1];
	uid_t x;
	int attfd;
	int r;
	if ((attfd = attropen(path, "r_uid", O_RDONLY)) == -1) {
		if (opt_verbose > 0) {
			msg("No uid xattr for `%s\'", path);
		}
		return u;
	}
	if ((r = read(attfd, buf, ATTR_SIZE)) == -1) {
		return u;
	}
	close(attfd);
	buf[r - 1] = '\0';
	x = (uid_t)atoi(buf);
	return x;
#else
	return u;
#endif /* HAVE_ATTR_XATTR_H, HAVE_OPENAT */
}

static gid_t
read_attr_gid(__attribute__((unused))
	char *path, __attribute__((unused)) gid_t g)
{
#ifdef HAVE_ATTR_XATTR_H
	char buf[ATTR_SIZE + 1];
	gid_t x;
	int r;

	if ((r = lgetxattr(path, "user.r_gid", buf, ATTR_SIZE)) > 0) {
		buf[r - 1] = '\0';
		x = (gid_t)atoi(buf);
		if (x > R_MAX_ID) {
			msg("Too large gid `%zd\' for `%s\', truncating", (size_t)x, 
					path);
			return R_MAX_ID;
		}
		return x;
	} else {
		if (opt_verbose > 0) {
			msg("No gid xattr for `%s\'\n", path);
		}
		return g;
	}
#elif HAVE_OPENAT
	/* solaris */
	char buf[ATTR_SIZE + 1];
	gid_t x;
	int attfd;
	int r;
	if ((attfd = attropen(path, "r_gid", O_RDONLY)) == -1) {
		if (opt_verbose > 0) {
			msg("No gid xattr for `%s\'", path);
		}
		return g;
	}
	if ((r = read(attfd, buf, ATTR_SIZE)) == -1) {
		return g;
	}
	close(attfd);
	buf[r - 1] = '\0';
	x = (uid_t)atoi(buf);
	return x;
#else
	return g;
#endif /* HAVE_ATTR_XATTR_H, HAVE_OPENAT */
}

/**
 * prepend path leading up to backup directory to the tree
 */
gboolean
dir_prepend(GTree *t, char *path)
{
	char *c;
	char *p;
	char *path2;
	size_t len;
	struct stat s;
	struct entry e;

	path2 = g_strdup(path);
	len   = strlen(path);

	/* add closing / */
	if (path2[len - 1] != DIR_SEP) {
		path2 = g_realloc(path2, len + 2);
		path2[len] = DIR_SEP;
		path2[len + 1] = '\0';
	}

	for (p = path2 + 1; (c = strchr(p, DIR_SEP)); p++) {
		*c = '\0';
		if (lstat(path2, &s) != 0) {
			msg("Could not stat path `%s\': %s", path2, strerror(errno));
			return FALSE;
		}
		e.f_name      = path2;
		e.f_name_size = strlen(path2);
		if (opt_attr) {
			e.f_uid = read_attr_uid(e.f_name, s.st_uid);
			e.f_gid = read_attr_gid(e.f_name, s.st_gid);
		} else {
			e.f_uid       = s.st_uid;
			e.f_gid       = s.st_gid;
		}
		e.f_mtime     = s.st_mtime;
		e.f_mode      = s.st_mode;
		e.f_size      = s.st_size;
		g_tree_insert(t, (gpointer) entry_dup(&e), VALUE);
		*c = DIR_SEP;
		p = c++;
	}
	g_free(path2);
	return TRUE;
}

void
dir_crawl(GTree *t, char *path)
{
	DIR 		*dir;
	FILE 		*f;
	struct dirent 	*dent;
	struct entry    *directory;
	char 		*curpath;
	struct stat   	s;
	struct entry	pop;
	struct remove_path rp;
	dev_t 		current_dev;
	size_t 		curpath_len;

	/* dir stack */
	gint32 d = 0;
	gint32 dstack_cnt  = 1;
	struct entry **dirstack =
		g_malloc(dstack_cnt * D_STACKSIZE * sizeof(struct entry *));

	if(!(dir = opendir(path))) {
		/* files are also allowed, check for this, if it isn't
		 * give the error
		 */
		if ((f = fopen(path, "r"))) {
			fclose(f);
			g_free(dirstack);
			return;
		}
		msg("Cannot enter directory `%s\': %s", path, strerror(errno));
		g_free(dirstack);
		return;
	}

	/* get device */
#ifdef HAVE_DIRFD
	if (fstat(dirfd(dir), &s) != 0) {
#else
	if (fstat(rdup_dirfd(dir), &s) != 0) {
#endif
		msg("Cannot determine holding device of the directory `%s\': %s", path, 
				strerror(errno));
		closedir(dir);
		g_free(dirstack);
		return;
	}
	current_dev = s.st_dev;

	while((dent = readdir(dir))) {
		if(!g_ascii_strcasecmp(dent->d_name, ".") || 
				!g_ascii_strcasecmp(dent->d_name, ".."))
			continue;

		curpath = g_strdup_printf("%s%c%s", path, DIR_SEP, dent->d_name);
		curpath_len = strlen(curpath);

		if(lstat(curpath, &s) != 0) {
			msg("Could not stat path `%s\': %s", curpath, strerror(errno));
			g_free(curpath);
			continue;
		}

		if (strchr(curpath, '\n')) {
			msg("Newline (\\n) found in path `%s\', skipping", curpath);
			g_free(curpath);
			continue;
		}

		if (S_ISREG(s.st_mode) || S_ISLNK(s.st_mode)) {
			pop.f_name      = curpath;
			pop.f_name_size = curpath_len;
			if (opt_attr) {
				pop.f_uid       = read_attr_uid(pop.f_name, s.st_uid);
				pop.f_gid       = read_attr_gid(pop.f_name, s.st_gid);
			} else {
				pop.f_uid       = s.st_uid;
				pop.f_gid       = s.st_gid;
			}
			pop.f_mtime     = s.st_mtime;
			pop.f_mode      = s.st_mode;
			pop.f_size      = s.st_size;

			if (opt_nobackup && !strcmp(dent->d_name, NOBACKUP)) {
				/* return after seeing .nobackup */
				if (opt_verbose > 0) {
					msg("" NOBACKUP " in '%s\'", path);
				}
				/* remove all files found in this path */
				rp.tree = t;
				rp.len  = strlen(path);
				rp.path = path;
				g_tree_foreach(t, gfunc_remove_path, (gpointer)&rp);
				/* add .nobackup back */
				g_tree_insert(t, (gpointer) entry_dup(&pop), VALUE);
				g_free(dirstack);
				closedir(dir);
				return;
			}
			g_tree_insert(t, (gpointer) entry_dup(&pop), VALUE);
			g_free(curpath);
			continue;
		} else if(S_ISDIR(s.st_mode)) {
			/* one filesystem */
			if (opt_onefilesystem && s.st_dev != current_dev) {
				msg("Walking into different filesystem");
				g_free(curpath);
				continue;
			}

			dirstack[d] = g_malloc(sizeof(struct entry));
			dirstack[d]->f_name       = g_strdup(curpath); 
			dirstack[d]->f_name_size  = curpath_len;
			if (opt_attr) {
				dirstack[d]->f_uid = 
					read_attr_uid(dirstack[d]->f_name, s.st_uid);
				dirstack[d]->f_gid = 
					read_attr_gid(dirstack[d]->f_name, s.st_gid);
			} else {
				dirstack[d]->f_uid = s.st_uid;
				dirstack[d]->f_gid = s.st_gid;
			}
			dirstack[d]->f_mtime      = s.st_mtime;
			dirstack[d]->f_mode       = s.st_mode;
			dirstack[d]->f_size       = s.st_size;

			if (d++ % D_STACKSIZE == 0) {
				dirstack = g_realloc(dirstack, 
						++dstack_cnt * D_STACKSIZE * 
						sizeof(struct entry *));
			}
			g_free(curpath);
			continue;
		} else {
			if (opt_verbose > 0) {
				msg("Neither file nor directory `%s\'", curpath);
			}
			g_free(curpath);
		}
	}
	closedir(dir);

	while (d > 0) {
		directory = dirstack[--d]; 
		g_tree_insert(t, (gpointer) entry_dup(directory), VALUE);
		/* recurse */
		/* potentially expensive operation. Better would be to when we hit
		 * .nobackup to go up the tree and delete some nodes.... or not */
		dir_crawl(t, directory->f_name);
		entry_free(directory);
	}
	g_free(dirstack);
	return;
}
