/* Built-in modules for the Linux build: PC/config.c's table as the Windows x64 python24 builds it (no audioop, imageop,
   rgbimg there), with posix in place of nt and without the Windows-only msvcrt, _winreg and _subprocess. */
#include "Python.h"

extern void initarray(void);
extern void initbinascii(void);
extern void initcmath(void);
extern void initerrno(void);
extern void initgc(void);
extern void initmath(void);
extern void initmd5(void);
extern void initoperator(void);
extern void initregex(void);
extern void initsignal(void);
extern void initsha(void);
extern void initstrop(void);
extern void initstruct(void);
extern void inittime(void);
extern void initthread(void);
extern void initcStringIO(void);
extern void initcPickle(void);
extern void init_locale(void);
extern void init_codecs(void);
extern void init_weakref(void);
extern void init_hotshot(void);
extern void init_random(void);
extern void init_bisect(void);
extern void init_heapq(void);
extern void inititertools(void);
extern void initcollections(void);
extern void init_symtable(void);
extern void initmmap(void);
extern void init_csv(void);
extern void init_sre(void);
extern void initparser(void);
extern void initdatetime(void);
extern void initxxsubtype(void);
extern void initzipimport(void);
extern void init_multibytecodec(void);
extern void init_codecs_cn(void);
extern void init_codecs_hk(void);
extern void init_codecs_iso2022(void);
extern void init_codecs_jp(void);
extern void init_codecs_kr(void);
extern void init_codecs_tw(void);
extern void initposix(void);
extern void PyMarshal_Init(void);
extern void initimp(void);

struct _inittab _PyImport_Inittab[] = {
    {"posix", initposix},
    {"array", initarray},
    {"binascii", initbinascii},
    {"cmath", initcmath},
    {"errno", initerrno},
    {"gc", initgc},
    {"math", initmath},
    {"md5", initmd5},
    {"operator", initoperator},
    {"regex", initregex},
    {"signal", initsignal},
    {"sha", initsha},
    {"strop", initstrop},
    {"struct", initstruct},
    {"time", inittime},
#ifdef WITH_THREAD
    {"thread", initthread},
#endif
    {"cStringIO", initcStringIO},
    {"cPickle", initcPickle},
    {"_locale", init_locale},
    {"_codecs", init_codecs},
    {"_weakref", init_weakref},
    {"_hotshot", init_hotshot},
    {"_random", init_random},
    {"_bisect", init_bisect},
    {"_heapq", init_heapq},
    {"itertools", inititertools},
    {"collections", initcollections},
    {"_symtable", init_symtable},
    {"mmap", initmmap},
    {"_csv", init_csv},
    {"_sre", init_sre},
    {"parser", initparser},
    {"datetime", initdatetime},
    {"xxsubtype", initxxsubtype},
    {"zipimport", initzipimport},
    {"_multibytecodec", init_multibytecodec},
    {"_codecs_cn", init_codecs_cn},
    {"_codecs_hk", init_codecs_hk},
    {"_codecs_iso2022", init_codecs_iso2022},
    {"_codecs_jp", init_codecs_jp},
    {"_codecs_kr", init_codecs_kr},
    {"_codecs_tw", init_codecs_tw},
    {"marshal", PyMarshal_Init},
    {"imp", initimp},
    {"__main__", NULL},
    {"__builtin__", NULL},
    {"sys", NULL},
    {"exceptions", NULL},
    {0, 0}
};
