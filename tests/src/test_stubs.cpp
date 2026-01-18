#if !defined(__ANDROID__) && !defined(__APPLE__)
#include <cstdlib>
#include <string>

[[noreturn]] void os_DebugBreak()
{
	std::abort();
}

void os_DoEvents()
{
}

void os_RunInstance(int argc, const char *argv[])
{
}

void os_LaunchFromURL(const std::string& url)
{
}

std::string os_GetMachineID()
{
    std::string empty;
    return empty;
}

std::string os_GetConnectionMedium()
{
    std::string empty;
    return empty;
}


#ifdef _WIN32
void os_SetThreadName(const char *name)
{
}
const char *getThreadName()
{
	return "threadname";
}
#endif
#endif
