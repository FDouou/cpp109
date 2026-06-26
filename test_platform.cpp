#include "log/platform.hpp"
#include <cstdio>
int main() {
#ifdef CPP109_PLATFORM_LINUX
    std::printf("LINUX defined\n");
#elif defined(CPP109_PLATFORM_WINDOWS)
    std::printf("WINDOWS defined\n");
#else
    std::printf("NEITHER defined\n");
#endif
    std::printf("cpu_count: %u\n", cpp109::platform::cpu_count());
    return 0;
}
