#ifndef SV_COMPAT_PLATFORM_H
#define SV_COMPAT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>
  #include <process.h>

  typedef SOCKET sv_socket_t;
  #define SV_INVALID_SOCKET INVALID_SOCKET
  #define sv_close_socket(s) closesocket(s)
  #define strcasecmp _stricmp
  #define strncasecmp _strnicmp

  static inline int sv_net_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
  }

  static inline void sv_net_cleanup(void) {
    WSACleanup();
  }

  static inline int sv_set_nonblocking(sv_socket_t fd) {
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
  }

  static inline int sv_get_executable_dir(char *out, size_t size) {
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)size);
    if (len == 0 || len >= size) return -1;
    for (int i = (int)len - 1; i >= 0; i--) {
      if (out[i] == '\\' || out[i] == '/') {
        out[i] = '\0';
        return 0;
      }
    }
    return -1;
  }

  static inline void sv_usleep(unsigned long usec) {
    SDL_Delay((Uint32)((usec + 999) / 1000));
  }

#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <strings.h>

  typedef int sv_socket_t;
  #define SV_INVALID_SOCKET (-1)
  #define sv_close_socket(s) close(s)

  static inline int sv_net_init(void) { return 0; }
  static inline void sv_net_cleanup(void) {}

  static inline int sv_set_nonblocking(sv_socket_t fd) {
    int flags = fcntl(fd, F_GETFL);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  static inline int sv_get_executable_dir(char *out, size_t size) {
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    return 0;
  }

  static inline void sv_usleep(unsigned long usec) {
    usleep(usec);
  }
#endif

#endif // SV_COMPAT_PLATFORM_H
