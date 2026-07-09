#ifndef VERSION_H
#define VERSION_H

#define OS_NAME "Viteza OS"
#define OS_VERSION_MAJOR 1
#define OS_VERSION_MINOR 0
#define OS_VERSION_PATCH 0
#define OS_VERSION_STR "1.0.0"
#define OS_CODENAME "Kairo"
#define OS_ARCH "x86_64"
#define OS_AUTHOR "KairoDev"

const char* os_version_string(void);
const char* os_full_name(void);

#endif