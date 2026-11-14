/*
 * xcselect.h: the public libxcselect API, as macOS's SDK declares it.
 *
 * Apple's header takes CF_ENUM and CF_SWIFT_NAME from
 * <CoreFoundation/CFBase.h>. MiniDarwin has no CoreFoundation, so the enum is
 * a plain uint32_t with the same values.
 */
#ifndef __XCSELECT_H__
#define __XCSELECT_H__

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/_types/_errno_t.h>

__BEGIN_DECLS

typedef uint32_t xcselect_host_sdk_policy_t;
enum {
  /* An SDK for the host's major OS version if there is one, else the latest. */
  XCSELECT_HOST_SDK_POLICY_MATCHING_PREFERRED = 1,
  /* An SDK for the host's major OS version. */
  XCSELECT_HOST_SDK_POLICY_MATCHING_ONLY = 2,
  /* The latest macOS SDK. */
  XCSELECT_HOST_SDK_POLICY_LATEST = 3,
};

/* On success, *path is a malloc'd SDK path for the caller to free. Returns 0,
 * EINVAL for an invalid policy or a NULL path, or ENOENT when no SDK
 * satisfies the policy. */
__attribute__((visibility("default")))
errno_t xcselect_host_sdk_path(xcselect_host_sdk_policy_t sdk_policy,
                               char **path);

__END_DECLS

#endif /* __XCSELECT_H__ */
