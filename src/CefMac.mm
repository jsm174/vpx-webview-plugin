// license:GPLv3+

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "include/cef_application_mac.h"

namespace WebView {

static char handlingSendEventKey;

static BOOL IsHandlingSendEvent(id self, SEL)
{
   NSNumber* value = objc_getAssociatedObject(self, &handlingSendEventKey);
   return value.boolValue;
}

static void SetHandlingSendEvent(id self, SEL, BOOL handling)
{
   objc_setAssociatedObject(self, &handlingSendEventKey, @(handling), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// CEF expects the NSApplication to implement CefAppProtocol. VPX's NSApplication comes from SDL, so the two methods are added at runtime.
void PrepareApplication()
{
   NSApplication* app = [NSApplication sharedApplication];
   Class cls = [app class];
   if ([app conformsToProtocol:@protocol(CefAppProtocol)])
      return;
   class_addMethod(cls, @selector(isHandlingSendEvent), reinterpret_cast<IMP>(IsHandlingSendEvent), "c@:");
   class_addMethod(cls, @selector(setHandlingSendEvent:), reinterpret_cast<IMP>(SetHandlingSendEvent), "v@:c");
   class_addProtocol(cls, @protocol(CrAppProtocol));
   class_addProtocol(cls, @protocol(CrAppControlProtocol));
   class_addProtocol(cls, @protocol(CefAppProtocol));
}

}
