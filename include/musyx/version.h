#ifndef _MUSYX_VERSION
#define _MUSYX_VERSION


#ifndef MUSY_VERSION_CHECK
#define MUSY_VERSION_CHECK(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#endif

#ifndef MUSY_VERSION_MAJOR
#define MUSY_VERSION_MAJOR 2
#endif

#ifndef MUSY_VERSION_MINOR
#define MUSY_VERSION_MINOR 0
#endif

#ifndef MUSY_VERSION_PATCH
#define MUSY_VERSION_PATCH 0
#endif


#ifndef MUSY_VERSION
#define MUSY_VERSION MUSY_VERSION_CHECK(MUSY_VERSION_MAJOR, MUSY_VERSION_MINOR, MUSY_VERSION_PATCH)
#endif

#endif // _MUSYX_VERSION
