#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BBPKG_ROOT
#define BBPKG_ROOT ""
#endif
#define DB_DIR BBPKG_ROOT "/var/lib/bbpkg/installed"
#define ROOT_DIR BBPKG_ROOT "/"

static int run(const char *const argv[], int output) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        if (output >= 0 && dup2(output, STDOUT_FILENO) < 0) _exit(127);
        execvp(argv[0], (char *const *)argv);
        perror(argv[0]);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return 1; }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int valid_name(const char *name) {
    size_t n = strlen(name);
    if (!n || n > 128 || !isalnum((unsigned char)name[0])) return 0;
    for (size_t i = 1; i < n; ++i)
        if (!isalnum((unsigned char)name[i]) && !strchr("._+-", name[i])) return 0;
    return 1;
}

static int make_dir(const char *path) {
    struct stat st;
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    fprintf(stderr, "bbpkg: cannot create directory %s\n", path);
    return 1;
}

static void manifest_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, DB_DIR "/%s.files", name);
}

/* Reject traversal and symlinked parent directories, including during removal. */
static char *full_path(const char *entry) {
    while (strncmp(entry, "./", 2) == 0) entry += 2;
    if (!*entry || !strcmp(entry, ".")) return strdup(ROOT_DIR);
    if (*entry == '/') return NULL;
    char *full = malloc(strlen(ROOT_DIR) + strlen(entry) + 1);
    if (!full) return NULL;
    strcpy(full, ROOT_DIR);
    strcat(full, entry);
    char *start = full + strlen(ROOT_DIR);
    for (char *p = start;; ++p) {
        if (*p && *p != '/') continue;
        size_t n = (size_t)(p - start);
        if (!n || (n == 1 && start[0] == '.') ||
            (n == 2 && !strncmp(start, "..", 2))) goto invalid;
        if (!*p) break;
        *p = '\0';
        struct stat st;
        int rc = lstat(full, &st);
        *p = '/';
        if ((rc == 0 && !S_ISDIR(st.st_mode)) || (rc < 0 && errno != ENOENT)) goto invalid;
        if (!p[1]) break;
        start = p + 1;
    }
    return full;
invalid:
    free(full);
    return NULL;
}

static int longer_first(const void *a, const void *b) {
    size_t x = strlen(*(char *const *)a), y = strlen(*(char *const *)b);
    return (x < y) - (x > y);
}

static void free_paths(char **paths, size_t count) {
    for (size_t i = 0; i < count; ++i) free(paths[i]);
    free(paths);
}

static int read_paths(FILE *f, char ***result, size_t *count) {
    char **paths = NULL, *line = NULL;
    size_t cap = 0, used = 0, allocated = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[--len] = '\0';
        if ((size_t)len != strlen(line)) goto fail;
        char *full = full_path(line);
        if (!full) { fputs("bbpkg: unsafe path or allocation failure\n", stderr); goto fail; }
        if (!strcmp(full, ROOT_DIR)) { free(full); continue; }
        if (used == allocated) {
            size_t next = allocated ? allocated * 2 : 64;
            char **grown = realloc(paths, next * sizeof(*paths));
            if (!grown) { free(full); goto fail; }
            paths = grown;
            allocated = next;
        }
        paths[used++] = full;
    }
    if (ferror(f)) goto fail;
    free(line);
    *result = paths;
    *count = used;
    return 0;
fail:
    free(line);
    free_paths(paths, used);
    return 1;
}

static int cmd_install(const char *name, const char *archive) {
    char manifest[sizeof(DB_DIR) + 140], temp[sizeof(DB_DIR) + 32];
    manifest_path(manifest, sizeof(manifest), name);
    if (access(manifest, F_OK) == 0) {
        fprintf(stderr, "bbpkg: '%s' is already installed\n", name);
        return 1;
    }
    snprintf(temp, sizeof(temp), DB_DIR "/.pending-XXXXXX");
    int fd = mkstemp(temp);
    if (fd < 0) { perror("manifest"); return 1; }
    const char *list[] = {"tar", "-tzf", archive, NULL};
    int rc = run(list, fd);
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); unlink(temp); return 1; }
    char **paths = NULL;
    size_t count = 0;
    if (fseek(f, 0, SEEK_SET) < 0 || rc || read_paths(f, &paths, &count)) rc = 1;
    fclose(f);
    free_paths(paths, count);
    if (!rc) {
        const char *extract[] = {"tar", "-xzf", archive, "-C", ROOT_DIR, NULL};
        rc = run(extract, -1);
    }
    if (!rc && rename(temp, manifest) < 0) { perror("rename manifest"); rc = 1; }
    if (rc) {
        unlink(temp);
        fputs("bbpkg: installation failed; extracted files may need manual cleanup\n", stderr);
        return 1;
    }
    printf("bbpkg: installed '%s'\n", name);
    return 0;
}

static int cmd_remove(const char *name) {
    char manifest[sizeof(DB_DIR) + 140];
    manifest_path(manifest, sizeof(manifest), name);
    FILE *f = fopen(manifest, "r");
    if (!f) { perror(manifest); return 1; }
    char **paths = NULL;
    size_t count = 0;
    int rc = read_paths(f, &paths, &count);
    fclose(f);
    if (rc) return rc;
    /* Children precede parents regardless of archive entry order. */
    if (count > 1) qsort(paths, count, sizeof(*paths), longer_first);
    for (size_t i = 0; i < count; ++i) {
        struct stat st;
        if (lstat(paths[i], &st) < 0) {
            if (errno != ENOENT) { perror(paths[i]); rc = 1; }
            continue;
        }
        int result = S_ISDIR(st.st_mode) ? rmdir(paths[i]) : unlink(paths[i]);
        if (result < 0 && errno != ENOENT &&
            !(S_ISDIR(st.st_mode) && (errno == ENOTEMPTY || errno == EEXIST))) {
            perror(paths[i]); rc = 1;
        }
    }
    free_paths(paths, count);
    if (!rc && unlink(manifest) < 0) { perror(manifest); rc = 1; }
    if (!rc) printf("bbpkg: removed '%s'\n", name);
    return rc;
}

static int cmd_list(void) {
    DIR *d = opendir(DB_DIR);
    if (!d) {
        if (errno == ENOENT) { puts("(no packages installed)"); return 0; }
        perror(DB_DIR); return 1;
    }
    struct dirent *entry;
    int any = 0;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        if (n > 6 && !strcmp(entry->d_name + n - 6, ".files")) {
            printf("%.*s\n", (int)(n - 6), entry->d_name);
            any = 1;
        }
    }
    closedir(d);
    if (!any) puts("(no packages installed)");
    return 0;
}

static int cmd_info(const char *name) {
    char manifest[sizeof(DB_DIR) + 140];
    manifest_path(manifest, sizeof(manifest), name);
    FILE *f = fopen(manifest, "r");
    if (!f) { perror(manifest); return 1; }
    printf("Files owned by '%s':\n", name);
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) printf("  /%s", line);
    int rc = ferror(f) ? 1 : 0;
    free(line);
    fclose(f);
    return rc;
}

int main(int argc, char **argv) {
    int install = argc == 4 && !strcmp(argv[1], "install");
    int remove_cmd = argc == 3 && !strcmp(argv[1], "remove");
    int info = argc == 3 && !strcmp(argv[1], "info");
    int list = argc == 2 && !strcmp(argv[1], "list");
    if (!(install || remove_cmd || info || list)) {
        fputs("usage: bbpkg install <name> <archive.tar.gz>\n"
              "       bbpkg remove <name>\n       bbpkg list\n       bbpkg info <name>\n", stderr);
        return 1;
    }
    if (!list && !valid_name(argv[2])) {
        fputs("bbpkg: invalid package name (maximum 128 characters)\n", stderr);
        return 1;
    }
    int lock = -1;
    if (install || remove_cmd) {
        if (make_dir(BBPKG_ROOT "/var") || make_dir(BBPKG_ROOT "/var/lib") ||
            make_dir(BBPKG_ROOT "/var/lib/bbpkg") || make_dir(DB_DIR)) return 1;
        lock = open(DB_DIR "/.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX) < 0) { perror("package lock"); return 1; }
    }
    int rc = install ? cmd_install(argv[2], argv[3]) :
        remove_cmd ? cmd_remove(argv[2]) : info ? cmd_info(argv[2]) : cmd_list();
    if (lock >= 0) close(lock);
    return rc;
}
