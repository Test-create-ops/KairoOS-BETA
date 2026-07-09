#include "version.h"

const char* os_version_string(void) {
    return OS_VERSION_STR;
}

const char* os_full_name(void) {
    return OS_NAME " " OS_VERSION_STR " (" OS_ARCH ")";
}