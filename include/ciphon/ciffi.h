// ciffi markers.
// a library marks its public header with these macros, ciffi_generate in cmake reads them by name
// and generates idiomatic swift and kotlin bindings on top of the plain c api.
// the compiler sees them as empty, so write them literally rather than through other macros.
//
// this file has no dependency, a library may copy it next to its public header and include it with quotes,
// so c programs using the library build without ciffi. every marker is guarded on its own, copies of
// different ciffi versions can meet in one translation unit.
#ifndef CIFFI_H
#define CIFFI_H

// public headers stay valid c11 because importers like swift and c++ do not parse them as c23.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#endif

// a function that is part of the generated api.
#ifndef CIFFI_EXPORT
#define CIFFI_EXPORT
#endif
// an opaque struct exposed as a class, methods are `<type>_<name>(<type> *, ...)`.
#ifndef CIFFI_OBJECT
#define CIFFI_OBJECT
#endif
// a struct copied into a native value type, a `T *x` field followed by `size_t x_count`, `x_len` or `x_size`
// is an array.
#ifndef CIFFI_RECORD
#define CIFFI_RECORD
#endif
// a `{ T *items; size_t count; }` struct copied into a native list.
#ifndef CIFFI_LIST
#define CIFFI_LIST
#endif
// an enum exposed as a native enum.
#ifndef CIFFI_ENUM
#define CIFFI_ENUM
#endif
// the error codes returned by fallible functions, `0` means success.
#ifndef CIFFI_ERROR
#define CIFFI_ERROR
#endif
// `const char *f(void)` describing the last error of the calling thread.
#ifndef CIFFI_ERROR_MESSAGE
#define CIFFI_ERROR_MESSAGE
#endif
// a function creating an object, `<type>_new` becomes the primary constructor.
#ifndef CIFFI_CONSTRUCTOR
#define CIFFI_CONSTRUCTOR
#endif
// the function freeing an object, or releasing the memory held by a record or a list.
#ifndef CIFFI_DESTRUCTOR
#define CIFFI_DESTRUCTOR
#endif
// a value that may be absent: a `NULL` string, object or record pointer, a `const T *` param,
// or a field paired with a `bool has_<name>` field.
#ifndef CIFFI_OPTIONAL
#define CIFFI_OPTIONAL
#endif
// a `{ <enum> kind; union { ... }; }` struct, a value whose payload depends on its kind.
#ifndef CIFFI_UNION
#define CIFFI_UNION
#endif
// a `{ void *userdata; <function pointers>; void (*release)(void *userdata); }` struct implemented by the bindings.
#ifndef CIFFI_CALLBACK
#define CIFFI_CALLBACK
#endif
// a blocking function the bindings call off the calling thread, `async` in swift and `suspend` in kotlin.
#ifndef CIFFI_ASYNC
#define CIFFI_ASYNC
#endif
// an object safe to use from several threads at once, the bindings skip their own locking.
#ifndef CIFFI_THREADSAFE
#define CIFFI_THREADSAFE
#endif
// a method freeing its object, the bindings refuse further calls on it.
#ifndef CIFFI_CONSUMES
#define CIFFI_CONSUMES
#endif
// an `int64_t` holding milliseconds since the unix epoch, a date in the bindings.
#ifndef CIFFI_TIMESTAMP
#define CIFFI_TIMESTAMP
#endif
