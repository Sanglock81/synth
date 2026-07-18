# Regenerate VersionInfo.h with the CURRENTLY-BUILT commit hash. Runs on EVERY build (unlike a
# configure-time capture, which goes stale the moment you rebuild without re-running cmake) so the
# on-screen banner always names the exact binary you are running. A "+" suffix flags a dirty tree.
execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${SRCDIR}" OUTPUT_VARIABLE GITHASH
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GITHASH)
    set(GITHASH "unknown")
endif()
execute_process(COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${SRCDIR}" OUTPUT_VARIABLE GITDIRTY ERROR_QUIET)
if(GITDIRTY)
    set(GITHASH "${GITHASH}+")
endif()
configure_file("${INFILE}" "${OUTFILE}" @ONLY)
