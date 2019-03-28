#ifndef COMMON_H_
#define COMMON_H_

#define CHRONOS_SERVER_SUCCESS  (0)
#define CHRONOS_SERVER_FAIL     (1)

#define server_msg(_prefix, ...) \
  do {                     \
    char _local_buf_[256];             \
    snprintf(_local_buf_, sizeof(_local_buf_), __VA_ARGS__); \
    fprintf(stderr,"%s: %s: at %s:%d", _prefix,_local_buf_, __FILE__, __LINE__);   \
    fprintf(stderr,"\n");              \
  } while(0)

#define server_info(...) \
  server_msg("INFO", __VA_ARGS__)

#define server_error(...) \
  server_msg("ERROR", __VA_ARGS__)

#define server_warning(...) \
  server_msg("WARN", __VA_ARGS__)

extern int server_debug_level;

#define server_debug(level,...) \
  do {                                                         \
    if (server_debug_level >= level) {                        \
      char _local_buf_[256];                                   \
      snprintf(_local_buf_, sizeof(_local_buf_), __VA_ARGS__); \
      fprintf(stderr, "DEBUG %s:%d: %s", __FILE__, __LINE__, _local_buf_);    \
      fprintf(stderr, "\n");     \
    } \
  } while(0)

#endif /* COMMON_H_ */
