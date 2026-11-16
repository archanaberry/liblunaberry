/*
 * luna-berry.h — LunaBerry's Berry/BeASM artifact backend.
 *
 * Emits the same kind of module libmochiberry's build-berry.sh produces:
 *
 *   <stem>.lbc    compiled Luna bytecode (runs on lunavm)
 *   <stem>.basm   readable BeASM module with the blob + runtime bridge
 *   <stem>.bex    Berry OS native binary (via tools/beasm)
 *   <stem>.host   host-verification executable (same blob, real engine)
 *
 * The BeASM module imports luna_berry_* from platform/berry, so the Berry
 * loader resolves the runtime at load time.  The host binary proves the
 * exact blob runs on a normal OS.
 */

#ifndef LUNA_BERRY_H
#define LUNA_BERRY_H

#include <stddef.h>

#include "luna.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LunaBerryOptions {
    const char *beasm;       /* beasm path; NULL = auto-discover         */
    const char *out_dir;     /* output dir; NULL = "dist/berry"          */
    const char *include_dir; /* project root; NULL = "."                 */
    const char *lib_dir;     /* dir with liblunaberry.a; NULL = exe dir  */
    int opt_level;           /* optimization ladder 0..10; <0 = default  */
    int emit_host;           /* build the host verification exe (1)      */
    int verbose;
} LunaBerryOptions;

/* Emit all Berry artifacts for `bc`. `stem` is the output base name.
 * Returns 0 on success. A missing beasm is a warning, not a failure:
 * the .lbc and .basm are still produced. */
LUNA_API int luna_berry_emit(const unsigned char *bc, size_t len,
                             const char *stem, const LunaBerryOptions *opts,
                             char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_BERRY_H */
