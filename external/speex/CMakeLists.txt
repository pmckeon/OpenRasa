# libspeex 1.2.1 (xiph.org, BSD-3-Clause - see COPYING), the sources unchanged from the release vcpkg's speex port downloads
# (github.com/xiph/speex, commit 5dceaaf3e23ee7fd17c80cb5f02a838fd6c18e01). Built here because the port's autotools build fails in a
# folder whose path has a space. Floating point, generic C (no SSE), the narrowband / wideband codecs; no Ogg, no binaries.
# The defines are the ones configure.ac / win32/config.h set: FLOATING_POINT, USE_SMALLFT (unused without the Vorbis
# psychoacoustic model), EXPORT empty, alloca (MSVC) or C99 variable-length arrays (elsewhere) for the scratch stack.
add_library(speex STATIC
    libspeex/bits.c libspeex/cb_search.c libspeex/exc_10_16_table.c libspeex/exc_10_32_table.c libspeex/exc_20_32_table.c
    libspeex/exc_5_256_table.c libspeex/exc_5_64_table.c libspeex/exc_8_128_table.c libspeex/filters.c libspeex/gain_table.c
    libspeex/gain_table_lbr.c libspeex/hexc_10_32_table.c libspeex/hexc_table.c libspeex/high_lsp_tables.c libspeex/lpc.c
    libspeex/lsp.c libspeex/lsp_tables_nb.c libspeex/ltp.c libspeex/modes.c libspeex/modes_wb.c libspeex/nb_celp.c
    libspeex/quant_lsp.c libspeex/sb_celp.c libspeex/speex.c libspeex/speex_callbacks.c libspeex/speex_header.c
    libspeex/stereo.c libspeex/vbr.c libspeex/vq.c libspeex/window.c)
# speex_types.h includes "speex_config_types.h" without a folder on systems other than Windows (where it defines the types itself).
target_include_directories(speex PUBLIC include generated generated/speex PRIVATE libspeex)
target_compile_definitions(speex PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=
    SPEEX_VERSION="1.2.1" SPEEX_MAJOR_VERSION=1 SPEEX_MINOR_VERSION=2 SPEEX_MICRO_VERSION=1 SPEEX_EXTRA_VERSION="")
if(MSVC)
    target_compile_definitions(speex PRIVATE USE_ALLOCA inline=__inline _CRT_SECURE_NO_WARNINGS)
    target_compile_options(speex PRIVATE /w)
else()
    target_compile_definitions(speex PRIVATE VAR_ARRAYS)
    target_compile_options(speex PRIVATE -w)
    target_link_libraries(speex PUBLIC m)
endif()
