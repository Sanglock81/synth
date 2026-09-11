# ===========================================================================
# Vendored libmp3lame 3.100 -- the EMBEDDED MP3 encoder.
#
# MP3 is the one export format JUCE cannot encode (it ships an MP3 decoder only), and we
# refuse to make it depend on a `lame`/`ffmpeg` binary the user has to install: on a fresh
# Windows box that turned "Save as MP3" into an error message. So LAME is compiled INTO
# the plugin. One static library, no runtime dependency, identical on Linux and Windows.
#
# LAME ships no CMake build, so this is it. Two things make it small and portable:
#
#   * Only the ENCODER is built -- the 19 .c files directly under libmp3lame/. Excluded:
#     mpglib/ and mpglib_interface.c (the decoder; entirely inside #ifdef HAVE_MPGLIB and
#     never called by lame.c), libmp3lame/vector/ (SSE intrinsics, gated on
#     HAVE_XMMINTRIN_H) and libmp3lame/i386/ (assembly, gated on HAVE_NASM).
#   * cmake/lame-config.h replaces the autotools-generated config.h. See that file for
#     why a single hand-written header works on every target.
#
# Pinned by URL + SHA256 so the build is reproducible and a compromised mirror cannot
# substitute different source. Bump both together, deliberately.
# ===========================================================================
include(FetchContent)
FetchContent_Declare(
    lame
    URL      https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
    URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# LAME's tarball has no CMakeLists.txt, so MakeAvailable populates it WITHOUT calling
# add_subdirectory -- which is exactly what we want, and avoids the deprecated
# FetchContent_Populate().
FetchContent_MakeAvailable(lame)

# The encoder sources, listed explicitly. An explicit list (rather than a glob) means a
# future LAME bump that adds or moves a file fails loudly at configure time instead of
# silently dropping code from the encoder.
set(VASYNTH_LAME_SOURCES
    bitstream.c encoder.c fft.c gain_analysis.c id3tag.c lame.c newmdct.c presets.c
    psymodel.c quantize.c quantize_pvt.c reservoir.c set_get.c tables.c takehiro.c
    util.c vbrquantize.c VbrTag.c version.c)
list(TRANSFORM VASYNTH_LAME_SOURCES PREPEND ${lame_SOURCE_DIR}/libmp3lame/)
foreach (src ${VASYNTH_LAME_SOURCES})
    if (NOT EXISTS ${src})
        message(FATAL_ERROR "vendored LAME: expected source is missing: ${src}\n"
                            "A LAME version bump probably moved or renamed it.")
    endif()
endforeach()

add_library(vasynth_mp3lame STATIC ${VASYNTH_LAME_SOURCES})

# config.h comes from cmake/; lame.h + the private headers from the LAME tree.
target_include_directories(vasynth_mp3lame
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}                 # our lame-config.h, included as config.h
            ${lame_SOURCE_DIR}/libmp3lame
    PUBLIC  ${lame_SOURCE_DIR}/include)               # <lame.h> for our own code
# LAME does `#ifdef HAVE_CONFIG_H / #include <config.h>`, so it needs a header literally
# named config.h on its include path. Copy ours into a PRIVATE directory used by this
# target only, so a file called config.h can never leak onto another target's include path.
set(VASYNTH_LAME_CONFIG_DIR ${CMAKE_CURRENT_BINARY_DIR}/vasynth-lame-config)
configure_file(${CMAKE_CURRENT_LIST_DIR}/lame-config.h
               ${VASYNTH_LAME_CONFIG_DIR}/config.h COPYONLY)
target_include_directories(vasynth_mp3lame PRIVATE ${VASYNTH_LAME_CONFIG_DIR})
target_compile_definitions(vasynth_mp3lame PRIVATE HAVE_CONFIG_H)

# LAME is 1990s C and warns copiously. Silence it for THIS target only -- our own strict
# warning flags are untouched, and vendored code we do not patch must not be able to fail
# our build on style.
if (MSVC)
    target_compile_options(vasynth_mp3lame PRIVATE /W0 /wd4996)
else()
    target_compile_options(vasynth_mp3lame PRIVATE -w)
endif()
set_target_properties(vasynth_mp3lame PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON)                     # linked into a shared-object VST3

# Same third-party sanitizer policy as the vendored Xiph codecs: LAME's bit-twiddling is
# not ours to fix, and UB found inside it is noise that would redden our gate. Scoped to
# this target alone -- all of Source/ stays fully checked.
if (VASYNTH_UBSAN)
    target_compile_options(vasynth_mp3lame PRIVATE -fno-sanitize=undefined)
endif()
