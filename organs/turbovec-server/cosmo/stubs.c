/* Stubs for libc symbols referenced by Rust std but absent from
 * Cosmopolitan Libc. Only code paths that turbovec-server never
 * exercises land here (e.g. pidfd waitid). They fail with ENOSYS. */
#include <errno.h>

int waitid(int idtype, unsigned id, void *infop, int options) {
    (void)idtype; (void)id; (void)infop; (void)options;
    errno = ENOSYS;
    return -1;
}
