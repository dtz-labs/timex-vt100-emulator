/*
 * Default build metadata for non-Makefile builds. The Makefile generates
 * build/build_meta.h and puts build/ before include/ on the quote include path.
 */
#ifndef BUILD_META_H
#define BUILD_META_H

#define APP_VERSION_STR "dev"
#define APP_BUILD_DATE "unknown"
#define APP_GIT_COMMIT "unknown"

#endif /* BUILD_META_H */
