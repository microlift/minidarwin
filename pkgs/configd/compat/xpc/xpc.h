/*
 * MiniDarwin: libxpc's interface, for building Apple's open source against.
 *
 * Apple has never released libxpc or its headers. This declares the subset of
 * the public API that the released projects here call, as plain C: objects
 * are opaque pointers, the type and error globals are compared by address,
 * and handlers are blocks. The rootfs ships no libxpc; the functions are
 * provided by whatever runs the tree (Rosemary's libSystem).
 */
#ifndef __XPC_H__
#define __XPC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <bsm/audit.h>
#include <dispatch/dispatch.h>

__BEGIN_DECLS

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef xpc_object_t xpc_endpoint_t;
typedef const struct _xpc_type_s *xpc_type_t;

typedef void (^xpc_handler_t)(xpc_object_t object);
typedef void (*xpc_finalizer_t)(void *value);
typedef bool (^xpc_dictionary_applier_t)(const char *key, xpc_object_t value);
typedef bool (^xpc_array_applier_t)(size_t index, xpc_object_t value);

#define XPC_TYPE(type) const struct _xpc_type_s type
#define XPC_GLOBAL_OBJECT(object) ((xpc_object_t)&(object))

extern XPC_TYPE(_xpc_type_connection);
extern XPC_TYPE(_xpc_type_endpoint);
extern XPC_TYPE(_xpc_type_null);
extern XPC_TYPE(_xpc_type_bool);
extern XPC_TYPE(_xpc_type_int64);
extern XPC_TYPE(_xpc_type_uint64);
extern XPC_TYPE(_xpc_type_double);
extern XPC_TYPE(_xpc_type_date);
extern XPC_TYPE(_xpc_type_data);
extern XPC_TYPE(_xpc_type_string);
extern XPC_TYPE(_xpc_type_uuid);
extern XPC_TYPE(_xpc_type_fd);
extern XPC_TYPE(_xpc_type_shmem);
extern XPC_TYPE(_xpc_type_array);
extern XPC_TYPE(_xpc_type_dictionary);
extern XPC_TYPE(_xpc_type_error);

#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_ENDPOINT (&_xpc_type_endpoint)
#define XPC_TYPE_NULL (&_xpc_type_null)
#define XPC_TYPE_BOOL (&_xpc_type_bool)
#define XPC_TYPE_INT64 (&_xpc_type_int64)
#define XPC_TYPE_UINT64 (&_xpc_type_uint64)
#define XPC_TYPE_DOUBLE (&_xpc_type_double)
#define XPC_TYPE_DATE (&_xpc_type_date)
#define XPC_TYPE_DATA (&_xpc_type_data)
#define XPC_TYPE_STRING (&_xpc_type_string)
#define XPC_TYPE_UUID (&_xpc_type_uuid)
#define XPC_TYPE_FD (&_xpc_type_fd)
#define XPC_TYPE_SHMEM (&_xpc_type_shmem)
#define XPC_TYPE_ARRAY (&_xpc_type_array)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_ERROR (&_xpc_type_error)

extern const struct _xpc_bool_s _xpc_bool_true;
extern const struct _xpc_bool_s _xpc_bool_false;
#define XPC_BOOL_TRUE XPC_GLOBAL_OBJECT(_xpc_bool_true)
#define XPC_BOOL_FALSE XPC_GLOBAL_OBJECT(_xpc_bool_false)

extern const struct _xpc_dictionary_s _xpc_error_connection_interrupted;
extern const struct _xpc_dictionary_s _xpc_error_connection_invalid;
extern const struct _xpc_dictionary_s _xpc_error_termination_imminent;
#define XPC_ERROR_CONNECTION_INTERRUPTED XPC_GLOBAL_OBJECT(_xpc_error_connection_interrupted)
#define XPC_ERROR_CONNECTION_INVALID XPC_GLOBAL_OBJECT(_xpc_error_connection_invalid)
#define XPC_ERROR_TERMINATION_IMMINENT XPC_GLOBAL_OBJECT(_xpc_error_termination_imminent)

extern const char *const _xpc_error_key_description;
#define XPC_ERROR_KEY_DESCRIPTION _xpc_error_key_description
extern const char *const _xpc_event_key_name;
#define XPC_EVENT_KEY_NAME _xpc_event_key_name

#define XPC_ARRAY_APPEND ((size_t)(-1))
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
#define XPC_CONNECTION_MACH_SERVICE_PRIVILEGED (1 << 1)

/* Objects. */
xpc_object_t xpc_retain(xpc_object_t object);
void xpc_release(xpc_object_t object);
xpc_type_t xpc_get_type(xpc_object_t object);
const char *xpc_type_get_name(xpc_type_t type);
xpc_object_t xpc_copy(xpc_object_t object);
bool xpc_equal(xpc_object_t object1, xpc_object_t object2);
size_t xpc_hash(xpc_object_t object);
char *xpc_copy_description(xpc_object_t object);

/* Scalars. */
xpc_object_t xpc_null_create(void);
xpc_object_t xpc_bool_create(bool value);
bool xpc_bool_get_value(xpc_object_t xbool);
xpc_object_t xpc_int64_create(int64_t value);
int64_t xpc_int64_get_value(xpc_object_t xint);
xpc_object_t xpc_uint64_create(uint64_t value);
uint64_t xpc_uint64_get_value(xpc_object_t xuint);
xpc_object_t xpc_double_create(double value);
double xpc_double_get_value(xpc_object_t xdouble);
xpc_object_t xpc_date_create(int64_t interval);
xpc_object_t xpc_date_create_from_current(void);
int64_t xpc_date_get_value(xpc_object_t xdate);
xpc_object_t xpc_data_create(const void *bytes, size_t length);
const void *xpc_data_get_bytes_ptr(xpc_object_t xdata);
size_t xpc_data_get_length(xpc_object_t xdata);
size_t xpc_data_get_bytes(xpc_object_t xdata, void *buffer, size_t off, size_t length);
xpc_object_t xpc_string_create(const char *string);
const char *xpc_string_get_string_ptr(xpc_object_t xstring);
size_t xpc_string_get_length(xpc_object_t xstring);
xpc_object_t xpc_uuid_create(const unsigned char *uuid);
const uint8_t *xpc_uuid_get_bytes(xpc_object_t xuuid);

/* Dictionaries. */
xpc_object_t xpc_dictionary_create(const char *const *keys,
    const xpc_object_t *values, size_t count);
xpc_object_t xpc_dictionary_create_empty(void);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
xpc_connection_t xpc_dictionary_get_remote_connection(xpc_object_t xdict);
void xpc_dictionary_set_value(xpc_object_t xdict, const char *key, xpc_object_t value);
xpc_object_t xpc_dictionary_get_value(xpc_object_t xdict, const char *key);
size_t xpc_dictionary_get_count(xpc_object_t xdict);
bool xpc_dictionary_apply(xpc_object_t xdict, xpc_dictionary_applier_t applier);
void xpc_dictionary_set_bool(xpc_object_t xdict, const char *key, bool value);
bool xpc_dictionary_get_bool(xpc_object_t xdict, const char *key);
void xpc_dictionary_set_int64(xpc_object_t xdict, const char *key, int64_t value);
int64_t xpc_dictionary_get_int64(xpc_object_t xdict, const char *key);
void xpc_dictionary_set_uint64(xpc_object_t xdict, const char *key, uint64_t value);
uint64_t xpc_dictionary_get_uint64(xpc_object_t xdict, const char *key);
void xpc_dictionary_set_double(xpc_object_t xdict, const char *key, double value);
double xpc_dictionary_get_double(xpc_object_t xdict, const char *key);
void xpc_dictionary_set_date(xpc_object_t xdict, const char *key, int64_t value);
int64_t xpc_dictionary_get_date(xpc_object_t xdict, const char *key);
void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
const char *xpc_dictionary_get_string(xpc_object_t xdict, const char *key);
void xpc_dictionary_set_data(xpc_object_t xdict, const char *key, const void *bytes, size_t length);
const void *xpc_dictionary_get_data(xpc_object_t xdict, const char *key, size_t *length);
void xpc_dictionary_set_uuid(xpc_object_t xdict, const char *key, const unsigned char *uuid);
const uint8_t *xpc_dictionary_get_uuid(xpc_object_t xdict, const char *key);
xpc_object_t xpc_dictionary_get_dictionary(xpc_object_t xdict, const char *key);
xpc_object_t xpc_dictionary_get_array(xpc_object_t xdict, const char *key);

/* Arrays. */
xpc_object_t xpc_array_create(const xpc_object_t *objects, size_t count);
xpc_object_t xpc_array_create_empty(void);
void xpc_array_append_value(xpc_object_t xarray, xpc_object_t value);
void xpc_array_set_value(xpc_object_t xarray, size_t index, xpc_object_t value);
xpc_object_t xpc_array_get_value(xpc_object_t xarray, size_t index);
size_t xpc_array_get_count(xpc_object_t xarray);
bool xpc_array_apply(xpc_object_t xarray, xpc_array_applier_t applier);
void xpc_array_set_bool(xpc_object_t xarray, size_t index, bool value);
bool xpc_array_get_bool(xpc_object_t xarray, size_t index);
void xpc_array_set_int64(xpc_object_t xarray, size_t index, int64_t value);
int64_t xpc_array_get_int64(xpc_object_t xarray, size_t index);
void xpc_array_set_uint64(xpc_object_t xarray, size_t index, uint64_t value);
uint64_t xpc_array_get_uint64(xpc_object_t xarray, size_t index);
void xpc_array_set_double(xpc_object_t xarray, size_t index, double value);
double xpc_array_get_double(xpc_object_t xarray, size_t index);
void xpc_array_set_date(xpc_object_t xarray, size_t index, int64_t value);
int64_t xpc_array_get_date(xpc_object_t xarray, size_t index);
void xpc_array_set_string(xpc_object_t xarray, size_t index, const char *string);
const char *xpc_array_get_string(xpc_object_t xarray, size_t index);
void xpc_array_set_data(xpc_object_t xarray, size_t index, const void *bytes, size_t length);
const void *xpc_array_get_data(xpc_object_t xarray, size_t index, size_t *length);
xpc_object_t xpc_array_get_dictionary(xpc_object_t xarray, size_t index);
xpc_object_t xpc_array_get_array(xpc_object_t xarray, size_t index);

/* Connections. */
xpc_connection_t xpc_connection_create(const char *name, dispatch_queue_t targetq);
xpc_connection_t xpc_connection_create_mach_service(const char *name,
    dispatch_queue_t targetq, uint64_t flags);
xpc_connection_t xpc_connection_create_from_endpoint(xpc_endpoint_t endpoint);
xpc_endpoint_t xpc_endpoint_create(xpc_connection_t connection);
void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
void xpc_connection_set_target_queue(xpc_connection_t connection, dispatch_queue_t targetq);
void xpc_connection_resume(xpc_connection_t connection);
void xpc_connection_activate(xpc_connection_t connection);
void xpc_connection_suspend(xpc_connection_t connection);
void xpc_connection_cancel(xpc_connection_t connection);
void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
void xpc_connection_send_message_with_reply(xpc_connection_t connection,
    xpc_object_t message, dispatch_queue_t replyq, xpc_handler_t handler);
xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t connection,
    xpc_object_t message);
void xpc_connection_send_barrier(xpc_connection_t connection, dispatch_block_t barrier);
const char *xpc_connection_get_name(xpc_connection_t connection);
pid_t xpc_connection_get_pid(xpc_connection_t connection);
uid_t xpc_connection_get_euid(xpc_connection_t connection);
gid_t xpc_connection_get_egid(xpc_connection_t connection);
au_asid_t xpc_connection_get_asid(xpc_connection_t connection);
void xpc_connection_set_context(xpc_connection_t connection, void *context);
void *xpc_connection_get_context(xpc_connection_t connection);
void xpc_connection_set_finalizer_f(xpc_connection_t connection, xpc_finalizer_t finalizer);

__END_DECLS

#endif
