// A macOS application menu.
//
// SDL3 (which sdl2-compat forwards to) creates the NSApplication and puts the
// app in front, but it does not build a menu bar. An app with no menus is not
// just cosmetically odd: macOS has nothing to show for it in the system bar,
// so it looks like the frontmost app never changed, and -- worse -- there is
// no Quit item, so Command-Q is never translated into an action at all and
// never reaches SDL as SDL_QUIT. That is the whole reason the app appeared
// impossible to quit.
//
// Quit posts SDL_QUIT rather than calling -[NSApp terminate:], so it goes
// through the same path as closing the window and the frontend's own
// confirmation prompt still gets its say.
#import <Cocoa/Cocoa.h>

#include <SDL2/SDL.h>

#include "macos_menu.h"

@interface KnifeEdgeMenuTarget : NSObject
- (void)requestQuit:(id)sender;
@end

@implementation KnifeEdgeMenuTarget
- (void)requestQuit:(id)sender {
    (void)sender;
    SDL_Event quit_event{};
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
}
@end

namespace {

// Held for the process lifetime: NSMenuItem does not retain its target.
KnifeEdgeMenuTarget* menu_target = nil;

NSMenuItem* add_item(NSMenu* menu, NSString* title, SEL action, NSString* key) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key];
    [menu addItem:item];
    return item;
}

}  // namespace

void knife_edge_install_macos_menu(const char* application_name) {
    @autoreleasepool {
        // Safe if SDL has already made one; this only creates NSApp when
        // called before SDL_Init(SDL_INIT_VIDEO).
        [NSApplication sharedApplication];
        if (NSApp == nil) {
            return;
        }
        if ([NSApp mainMenu] != nil) {
            return;  // something already provided a menu; leave it alone
        }

        NSString* name = application_name != nullptr
                       ? [NSString stringWithUTF8String:application_name]
                       : @"Game";
        menu_target = [[KnifeEdgeMenuTarget alloc] init];

        NSMenu* menubar = [[NSMenu alloc] initWithTitle:@""];

        // The application menu. Its title is ignored -- macOS always shows the
        // process name in bold here -- but the items are what make Command-Q,
        // Hide and the Services menu work at all.
        NSMenuItem* app_item = [[NSMenuItem alloc] initWithTitle:name
                                                          action:nil
                                                   keyEquivalent:@""];
        NSMenu* app_menu = [[NSMenu alloc] initWithTitle:name];
        add_item(app_menu, [@"About " stringByAppendingString:name],
                 @selector(orderFrontStandardAboutPanel:), @"");
        [app_menu addItem:[NSMenuItem separatorItem]];

        NSMenu* services = [[NSMenu alloc] initWithTitle:@"Services"];
        NSMenuItem* services_item = add_item(app_menu, @"Services", nil, @"");
        [services_item setSubmenu:services];
        [NSApp setServicesMenu:services];
        [app_menu addItem:[NSMenuItem separatorItem]];

        add_item(app_menu, [@"Hide " stringByAppendingString:name],
                 @selector(hide:), @"h");
        NSMenuItem* hide_others = add_item(app_menu, @"Hide Others",
                                           @selector(hideOtherApplications:), @"h");
        [hide_others setKeyEquivalentModifierMask:
            NSEventModifierFlagOption | NSEventModifierFlagCommand];
        add_item(app_menu, @"Show All", @selector(unhideAllApplications:), @"");
        [app_menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* quit = add_item(app_menu, [@"Quit " stringByAppendingString:name],
                                    @selector(requestQuit:), @"q");
        [quit setTarget:menu_target];

        [app_item setSubmenu:app_menu];
        [menubar addItem:app_item];

        // A Window menu, so full screen and minimise behave the way every
        // other Mac app does.
        NSMenuItem* window_item = [[NSMenuItem alloc] initWithTitle:@"Window"
                                                             action:nil
                                                      keyEquivalent:@""];
        NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
        add_item(window_menu, @"Minimize", @selector(performMiniaturize:), @"m");
        add_item(window_menu, @"Zoom", @selector(performZoom:), @"");
        [window_menu addItem:[NSMenuItem separatorItem]];
        add_item(window_menu, @"Bring All to Front",
                 @selector(arrangeInFront:), @"");
        [window_item setSubmenu:window_menu];
        [menubar addItem:window_item];
        [NSApp setWindowsMenu:window_menu];

        [NSApp setMainMenu:menubar];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        // Setting mainMenu is not enough on its own. SDL runs its own event
        // loop and never calls -[NSApp run], so AppKit's launch sequence stops
        // short of presenting the menu bar: the menu exists on NSApp and the
        // system bar stays empty. This is the step it skips, and without it
        // the app has no menus for macOS to show and no Quit item at all.
        if (![NSApp isRunning]) {
            [NSApp finishLaunching];
        }
    }
}
