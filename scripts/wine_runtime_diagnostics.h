/* Optional Wine/OHOS instrumentation. Initialize before installing handlers. */
#ifndef WINE_OHOS_RUNTIME_DIAGNOSTICS_H
#define WINE_OHOS_RUNTIME_DIAGNOSTICS_H
#include <errno.h>
#include <signal.h>
#include <unistd.h>

static inline int ohos_diagnostics_flag( const char *value )
{
    return value && value[0] == '1' && !value[1];
}

#ifdef __OHOS__
extern volatile sig_atomic_t ohos_runtime_diagnostics_enabled;
static inline void ohos_runtime_diagnostics_write( int fd, const char *text, size_t size )
{
    int saved_errno;
    if (!ohos_runtime_diagnostics_enabled) return;
    saved_errno = errno;
    write( fd, text, size );
    errno = saved_errno;
}
#else
static inline void ohos_runtime_diagnostics_write( int fd, const char *text, size_t size )
{
    (void)fd; (void)text; (void)size;
}
#endif
#endif
