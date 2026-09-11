/* ===========================================================================
 * config.h for the vendored LAME encoder (libmp3lame 3.100).
 *
 * LAME normally generates this with autotools, which we do not run -- so this is a
 * hand-written, platform-INDEPENDENT replacement. That is only viable because LAME's
 * ENCODER consults a very small set of macros; the full list it references is:
 *
 *   HAVE_CONFIG_H HAVE_ERRNO_H HAVE_FCNTL_H HAVE_INTTYPES_H HAVE_STDINT_H
 *   HAVE_STRCHR HAVE_MEMCPY HAVE_MPGLIB HAVE_NASM HAVE_XMMINTRIN_H
 *   TAKEHIRO_IEEE754_HACK USE_FAST_LOG USE_GOGO_SUBBAND STDC_HEADERS
 *   LAME_LIBRARY_BUILD
 *
 * Notably it needs NO PACKAGE/VERSION strings, no SIZEOF_* and no endianness probe, so
 * nothing here has to be discovered per platform. Everything defined below is true on
 * every target we build for (Linux/GCC, Windows/MSVC).
 * ===========================================================================*/
#ifndef VASYNTH_LAME_CONFIG_H
#define VASYNTH_LAME_CONFIG_H

#define STDC_HEADERS       1
#define HAVE_ERRNO_H       1
#define HAVE_FCNTL_H       1
#define HAVE_INTTYPES_H    1     /* MSVC has had both since 2013 */
#define HAVE_STDINT_H      1
#define HAVE_STRCHR        1
#define HAVE_MEMCPY        1
#define LAME_LIBRARY_BUILD 1     /* LAME requires this when built as a library */

/* DELIBERATELY NOT DEFINED:
 *   HAVE_MPGLIB          - the mpglib DECODER is not built; we only encode. Every
 *                          reference to it is inside mpglib_interface.c, which the
 *                          CMake source list excludes, and lame.c never calls it.
 *   HAVE_XMMINTRIN_H     - keeps libmp3lame/vector/ out of the build, so there is no
 *                          SSE-intrinsics path to get wrong per compiler/arch.
 *   HAVE_NASM            - no hand-written assembly.
 *   TAKEHIRO_IEEE754_HACK- a quantizer shortcut that type-puns floats. We transcode
 *                          offline, so its speed is irrelevant and portability wins.
 *   USE_FAST_LOG         - an approximation; we prefer accuracy over speed here.
 *   USE_GOGO_SUBBAND     - unmaintained third-party assembly.
 */

/* LAME's configure emits these typedefs when the platform does not already provide the
 * types (they are a LAME-ism, not standard -- glibc's <ieee754.h> does NOT define them).
 * The guards let a platform that does provide them opt out by defining HAVE_*. */
#ifndef HAVE_IEEE754_FLOAT64_T
typedef double ieee754_float64_t;
#endif
#ifndef HAVE_IEEE754_FLOAT32_T
typedef float ieee754_float32_t;
#endif

#endif /* VASYNTH_LAME_CONFIG_H */
