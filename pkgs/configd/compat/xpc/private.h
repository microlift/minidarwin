/*
 * MiniDarwin: the private libxpc calls the released projects here make, on
 * top of <xpc/xpc.h>. As there, nothing in the rootfs defines them.
 */
#ifndef __XPC_PRIVATE_H__
#define __XPC_PRIVATE_H__

#include <xpc/xpc.h>

__BEGIN_DECLS

/* The value of `entitlement` the peer is signed with, or NULL. */
xpc_object_t xpc_connection_copy_entitlement_value(xpc_connection_t connection,
    const char *entitlement);

/* The XPC objects a binary or XML property list describes, or NULL. */
xpc_object_t xpc_create_from_plist(const void *data, size_t length);

__END_DECLS

#endif
