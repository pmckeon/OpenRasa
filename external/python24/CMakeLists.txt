# CPython 2.4.6, vendored (Include, Objects, Parser, Python, Modules, PC and LICENSE of the 2.4.6 source release), built as a static
# library. It is the interpreter the original client embedded (python24.dll); OpenRasa runs the game's own compiled scripts with
# it. The standard library is not vendored: the game ships its own (trpython.zip/client/system).
#
# Linux (x86_64, glibc): Linux/pyconfig.h, written by hand from the release's pyconfig.h.in, and Linux/config.c - see those files.
#
# Windows: PC/pyconfig.h and PC/config.c (built-in module table), as PCbuild/pythoncore.vcproj builds python24.dll - its source
# list, without main.c (Py_Main), dl_nt.c (DllMain) and the resource file. Py_NO_ENABLE_SHARED: no dllimport / dllexport.
cmake_minimum_required(VERSION 3.21)
project(python24 C)

set(PY ${CMAKE_CURRENT_SOURCE_DIR})
set(PYTHON24_SOURCES
    Modules/_bisectmodule.c Modules/cjkcodecs/_codecs_cn.c Modules/cjkcodecs/_codecs_hk.c Modules/cjkcodecs/_codecs_iso2022.c
    Modules/cjkcodecs/_codecs_jp.c Modules/cjkcodecs/_codecs_kr.c Modules/cjkcodecs/_codecs_tw.c Modules/_codecsmodule.c
    Modules/_csv.c Modules/_heapqmodule.c Modules/_hotshot.c Modules/_localemodule.c Modules/_randommodule.c Modules/_sre.c
    Modules/_weakref.c Objects/abstract.c Parser/acceler.c Modules/arraymodule.c Modules/binascii.c Parser/bitset.c
    Python/bltinmodule.c Objects/boolobject.c Objects/bufferobject.c Objects/cellobject.c Python/ceval.c Objects/classobject.c
    Modules/cmathmodule.c Objects/cobject.c Python/codecs.c Modules/collectionsmodule.c Python/compile.c Objects/complexobject.c
    Modules/cPickle.c Modules/cStringIO.c Modules/datetimemodule.c Objects/descrobject.c Objects/dictobject.c Objects/enumobject.c
    Modules/errnomodule.c Python/errors.c Python/exceptions.c Objects/fileobject.c Objects/floatobject.c Objects/frameobject.c
    Python/frozen.c Objects/funcobject.c Python/future.c Modules/gcmodule.c Objects/genobject.c Python/getargs.c
    Modules/getbuildinfo.c Python/getcompiler.c Python/getcopyright.c Python/getmtime.c Python/getopt.c Python/getplatform.c
    Python/getversion.c Python/graminit.c Parser/grammar1.c Python/import.c Python/importdl.c Objects/intobject.c
    Objects/iterobject.c Modules/itertoolsmodule.c Parser/listnode.c Objects/listobject.c Objects/longobject.c Python/marshal.c
    Modules/mathmodule.c Modules/md5c.c Modules/md5module.c Parser/metagrammar.c Objects/methodobject.c Modules/mmapmodule.c
    Python/modsupport.c Objects/moduleobject.c Modules/cjkcodecs/multibytecodec.c Parser/myreadline.c Python/mysnprintf.c
    Python/mystrtoul.c Parser/node.c Objects/object.c Objects/obmalloc.c Modules/operator.c Parser/parser.c Modules/parsermodule.c
    Parser/parsetok.c Modules/posixmodule.c Python/pyfpe.c Python/pystate.c Python/pystrtod.c Python/pythonrun.c
    Objects/rangeobject.c Modules/regexmodule.c Modules/regexpr.c Objects/setobject.c Modules/shamodule.c Modules/signalmodule.c
    Objects/sliceobject.c Objects/stringobject.c Modules/stropmodule.c Python/structmember.c Modules/structmodule.c
    Objects/structseq.c Python/symtable.c Modules/symtablemodule.c Python/sysmodule.c Python/thread.c Modules/threadmodule.c
    Modules/timemodule.c Parser/tokenizer.c Python/traceback.c Objects/tupleobject.c Objects/typeobject.c Objects/unicodectype.c
    Objects/unicodeobject.c Objects/weakrefobject.c Modules/xxsubtype.c Modules/yuvconvert.c Modules/zipimport.c
)
if (WIN32)
    list(APPEND PYTHON24_SOURCES
        PC/_subprocess.c PC/_winreg.c PC/config.c Python/dynload_win.c PC/getpathp.c PC/import_nt.c PC/msvcrtmodule.c
        PC/static_nt.c Modules/audioop.c Modules/imageop.c Modules/rgbimgmodule.c
    )
else()
    # Linux: Linux/config.c (the module table), Modules/getpath.c (prefix search; OpenRasa sets sys.path itself), no dynamic
    # loading (dynload_stub.c), pthreads (thread.c picks thread_pthread.h).
    list(APPEND PYTHON24_SOURCES Linux/config.c Modules/getpath.c Python/dynload_stub.c)
endif()

add_library(python24 STATIC ${PYTHON24_SOURCES})
if (WIN32)
    target_include_directories(python24 PUBLIC ${PY}/Include ${PY}/PC PRIVATE ${PY}/Python)
else()
    target_include_directories(python24 PUBLIC ${PY}/Include ${PY}/Linux PRIVATE ${PY}/Python)
endif()
target_compile_definitions(python24 PUBLIC Py_NO_ENABLE_SHARED PRIVATE Py_BUILD_CORE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
if (MSVC)
    # 2006 code: quiet its warnings; keep signed overflow wrapping as the old compilers did.
    target_compile_options(python24 PRIVATE /W0 /wd4996)
endif()
if (WIN32)
    target_link_libraries(python24 PUBLIC ws2_32 advapi32 user32 shell32)
else()
    # As the release's Makefile: -fno-strict-aliasing and wrapping signed arithmetic (intobject.c's overflow tests rely on it);
    # gnu89 for its K&R-era declarations (GCC 15 defaults to C23, where "()" means no arguments).
    target_compile_options(python24 PRIVATE -std=gnu89 -fno-strict-aliasing -fwrapv -w)
    target_compile_definitions(python24 PRIVATE PREFIX="/usr/local" EXEC_PREFIX="/usr/local" VERSION="2.4" VPATH="" PYTHONPATH=":plat-linux2" PLATFORM="linux2")
    find_package(Threads REQUIRED)
    target_link_libraries(python24 PUBLIC Threads::Threads m util)
endif()
