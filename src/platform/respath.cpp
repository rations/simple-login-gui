// See respath.h.

#include "respath.h"

#include <sys/stat.h>
#include <unistd.h>

#include <string>

#ifndef XLOGIN_RESOURCE_DIR_DEFAULT
#error "XLOGIN_RESOURCE_DIR_DEFAULT must be defined by the build (the installed share dir)"
#endif

namespace xlogin
{
namespace
{

bool hasFonts(const std::string &dir)
{
    if (dir.empty())
        return false;
    struct stat st;
    return stat((dir + "/fonts").c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The directory the running executable sits in, or empty. Deriving this at run time is fine for
// locating a font; it is never used to find a program to execute, which always comes from a
// compile-time candidate list.
std::string exeDir()
{
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    buf[n] = '\0';
    std::string path(buf);
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string resolve()
{
    const std::string installed(XLOGIN_RESOURCE_DIR_DEFAULT);
    if (hasFonts(installed))
        return installed;

    const std::string exe = exeDir();
    if (!exe.empty()) {
        // A build tree: ./xlogin next to ./resources, or build/xlogin next to ../resources.
        for (const char *rel : {"/resources", "/../resources"}) {
            const std::string dir = exe + rel;
            if (hasFonts(dir))
                return dir;
        }
    }

    return std::string();
}

} // namespace

const std::string &resourceDir()
{
    static const std::string dir = resolve();
    return dir;
}

} // namespace xlogin
