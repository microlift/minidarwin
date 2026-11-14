// CoreFoundation, as far as Foundation's Swift half calls it. Its objects are
// opaque here: the framework headers are not part of a Darwin base.
#ifndef _FOUNDATION_DARWIN_SHIMS_H
#define _FOUNDATION_DARWIN_SHIMS_H

#include <stdarg.h>

const void *CFStringCreateWithFormatAndArguments(const void *alloc,
    const void *formatOptions, const void *format, va_list arguments);

#endif
