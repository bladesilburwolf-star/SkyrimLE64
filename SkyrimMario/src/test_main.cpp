#include "sm64_api.h"
#include <windows.h>
int main()
{
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string dir(path); dir = dir.substr(0, dir.find_last_of("\\/"));
    FILE* log = stdout;
    fprintf(log, "SkyrimMario sm64 self-test (folder: %s)\n", dir.c_str());
    bool ok = RunSelfTest(dir, log, 240);
    fprintf(log, "\nPress Enter to close..."); fflush(log); getchar();
    return ok ? 0 : 1;
}
