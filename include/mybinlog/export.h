/**
 * @file export.h
 * @brief Symbol visibility of the public API.
 *
 * The static and the shared library install this same header. The static
 * library and everything compiled against it define MYBINLOG_STATIC_DEFINE
 * (the CMake target and mybinlog-static.pc add it), which leaves MYBINLOG_API
 * empty; otherwise it marks the declaration as exported from the shared
 * library, whose other symbols are hidden.
 */

#ifndef MYBINLOG_EXPORT_H_
#define MYBINLOG_EXPORT_H_

#if defined(MYBINLOG_STATIC_DEFINE)
#define MYBINLOG_API
#else
#define MYBINLOG_API __attribute__((visibility("default")))
#endif

#endif  // MYBINLOG_EXPORT_H_
