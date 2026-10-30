#ifndef SV_COMPAT_DIRENT_H
#define SV_COMPAT_DIRENT_H

#ifndef _WIN32
  #include <dirent.h>
#else
  #include <windows.h>
  #include <stdlib.h>
  #include <string.h>

  struct dirent {
    char d_name[MAX_PATH];
  };

  typedef struct DIR {
    HANDLE handle;
    WIN32_FIND_DATAA find_data;
    struct dirent ent;
    int first;
  } DIR;

  static inline DIR *opendir(const char *path) {
    if (!path) return NULL;
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    DIR *dir = (DIR *)calloc(1, sizeof(DIR));
    if (!dir) return NULL;

    dir->handle = FindFirstFileA(pattern, &dir->find_data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
      free(dir);
      return NULL;
    }
    dir->first = 1;
    return dir;
  }

  static inline struct dirent *readdir(DIR *dir) {
    if (!dir || dir->handle == INVALID_HANDLE_VALUE) return NULL;
    if (dir->first) {
      dir->first = 0;
      strncpy(dir->ent.d_name, dir->find_data.cFileName, sizeof(dir->ent.d_name) - 1);
      dir->ent.d_name[sizeof(dir->ent.d_name) - 1] = '\0';
      return &dir->ent;
    }
    if (!FindNextFileA(dir->handle, &dir->find_data)) {
      return NULL;
    }
    strncpy(dir->ent.d_name, dir->find_data.cFileName, sizeof(dir->ent.d_name) - 1);
    dir->ent.d_name[sizeof(dir->ent.d_name) - 1] = '\0';
    return &dir->ent;
  }

  static inline int closedir(DIR *dir) {
    if (!dir) return -1;
    if (dir->handle != INVALID_HANDLE_VALUE) {
      FindClose(dir->handle);
    }
    free(dir);
    return 0;
  }
#endif

#endif // SV_COMPAT_DIRENT_H
