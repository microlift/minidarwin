/*
 * Not Apple's. Foundation has never been released, and the Swift runtime's
 * Objective-C files (SwiftObject.mm, SwiftValue.mm, ErrorObject.mm, the
 * stubs' NSXXXBase classes, ...) #import <Foundation/Foundation.h>. They
 * reach every Foundation class at run time, by objc_lookUpClass/dlsym, so
 * all they need from the header is enough to send these messages and spell
 * these types. Each declaration is Foundation's public API, unchanged; no
 * class here is referenced at link time (libswift-core.nix checks the
 * dylib imports no _OBJC_CLASS_$_NS* symbol).
 */
#ifndef _MINIDARWIN_FOUNDATION_SHIM_H_
#define _MINIDARWIN_FOUNDATION_SHIM_H_

#import <objc/NSObject.h>
#import <CoreFoundation/CoreFoundation.h>

/* NSZone.h */
typedef struct _NSZone NSZone;

/* NSObject.h (Foundation's half) */
@protocol NSCopying
- (id)copyWithZone:(NSZone *)zone;
@end

@class NSCoder, NSDictionary;

/* NSString.h */
@interface NSString : NSObject
@property (readonly, copy) NSString *decomposedStringWithCanonicalMapping;
@end

/* NSNull.h */
@interface NSNull : NSObject
+ (NSNull *)null;
@end

/* NSError.h */
@interface NSError : NSObject
@end

#endif /* _MINIDARWIN_FOUNDATION_SHIM_H_ */
