/* OpenRasa: the globals PC/dl_nt.c provides in the DLL build, for the static library (no DllMain). getpathp.c reads them;
   with no DLL handle it locates Python from the executable. */
#include "windows.h"
#include "pyconfig.h"
#include "Python.h"

HMODULE PyWin_DLLhModule = NULL;
const char *PyWin_DLLVersionString = "2.4";
