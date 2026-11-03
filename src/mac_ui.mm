// mac_ui.mm - Objective-C++ (compiled with ARC) for native macOS features.
#include "mac_ui.hpp"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <GameController/GameController.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>

namespace macui {

// The rounded version of the system font (SF Pro Rounded), which suits the
// app's soft night-sky look. Falls back to the normal system font.
static NSFont* uiFont(double pixelSize, bool bold) {
    NSFont* font = bold ? [NSFont systemFontOfSize:pixelSize weight:NSFontWeightBold]
                        : [NSFont systemFontOfSize:pixelSize weight:NSFontWeightMedium];
    NSFontDescriptor* rounded = [font.fontDescriptor fontDescriptorWithDesign:NSFontDescriptorSystemDesignRounded];
    if (rounded) {
        if (NSFont* roundedFont = [NSFont fontWithDescriptor:rounded size:pixelSize]) return roundedFont;
    }
    return font;
}

double measureText(const std::string& utf8, double pixelSize, bool bold) {
    @autoreleasepool {
        NSString* string = [NSString stringWithUTF8String:utf8.c_str()];
        if (!string || string.length == 0) return 0;
        NSFont* font = uiFont(pixelSize, bold);
        NSAttributedString* text = [[NSAttributedString alloc] initWithString:string
                                                                   attributes:@{NSFontAttributeName : font}];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text);
        const double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
        CFRelease(line);
        return std::ceil(width) + 2;
    }
}

TextImage renderText(const std::string& utf8, double pixelSize, bool bold) {
    TextImage image;
    @autoreleasepool {
        NSString* string = [NSString stringWithUTF8String:utf8.c_str()];
        if (!string || string.length == 0) return image;
        NSFont* font = uiFont(pixelSize, bold);
        NSAttributedString* text = [[NSAttributedString alloc] initWithString:string
                                                                   attributes:@{NSFontAttributeName : font}];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text);
        CGFloat ascent = 0, descent = 0, leading = 0;
        const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

        image.width = static_cast<int>(std::ceil(width)) + 2;
        image.height = static_cast<int>(std::ceil(ascent + descent)) + 2;
        image.pixels.assign(static_cast<std::size_t>(image.width) * image.height, 0);

        // Premultiplied BGRA in memory is ARGB8888 on little-endian Macs.
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(
            image.pixels.data(), image.width, image.height, 8, image.width * 4, colorSpace,
            static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
        CGContextSetTextPosition(context, 1, std::ceil(descent) + 1);
        CTLineDraw(line, context);
        CGContextRelease(context);
        CGColorSpaceRelease(colorSpace);
        CFRelease(line);
    }
    // Only the coverage (alpha) matters: make every pixel white so the text
    // can be tinted with SDL_SetTextureColorMod and drawn with normal blending.
    for (auto& pixel : image.pixels) pixel = (pixel & 0xFF000000u) | 0x00FFFFFFu;
    return image;
}

std::vector<std::string> chooseRomFiles() {
    std::vector<std::string> paths;
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = YES;
        panel.message = @"Choose Game Boy Advance ROMs to add to your library";
        panel.prompt = @"Add";
        if (UTType* gba = [UTType typeWithFilenameExtension:@"gba"]) panel.allowedContentTypes = @[ gba ];
        NSWindow* previousKey = NSApp.keyWindow;
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL* url in panel.URLs) paths.emplace_back(url.fileSystemRepresentation);
        }
        [previousKey makeKeyAndOrderFront:nil];  // give keyboard focus back to the game window
    }
    return paths;
}

std::string chooseFolder(const std::string& current) {
    std::string result;
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.message = @"Choose the folder where your games are kept";
        panel.prompt = @"Use Folder";
        if (!current.empty()) {
            panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:current.c_str()]
                                            isDirectory:YES];
        }
        NSWindow* previousKey = NSApp.keyWindow;
        if ([panel runModal] == NSModalResponseOK && panel.URL) result = panel.URL.fileSystemRepresentation;
        [previousKey makeKeyAndOrderFront:nil];
    }
    return result;
}

// Asks macOS not to use the controller's buttons for its own shortcuts (the
// Home / PS button opens the Games overlay or Launchpad) while this app is in
// front, so the app gets the press instead.
static void keepButtonsFromSystem(GCController* controller) {
    for (NSString* name in controller.physicalInputProfile.buttons) {
        GCControllerButtonInput* button = controller.physicalInputProfile.buttons[name];
        if (button.isBoundToSystemGesture) button.preferredSystemGestureState = GCSystemGestureStateDisabled;
    }
}

void keepControllerButtonsFromSystem() {
    @autoreleasepool {
        static id observer = nil;
        if (!observer) {
            // Controllers connected later, including after waking from sleep.
            observer = [[NSNotificationCenter defaultCenter]
                addObserverForName:GCControllerDidConnectNotification
                            object:nil
                             queue:[NSOperationQueue mainQueue]
                        usingBlock:^(NSNotification* note) {
                          if ([note.object isKindOfClass:[GCController class]]) keepButtonsFromSystem(note.object);
                        }];
        }
        for (GCController* controller in [GCController controllers]) keepButtonsFromSystem(controller);
    }
}

void openInFinder(const std::string& folder) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:folder.c_str()];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path isDirectory:YES]];
    }
}

static std::string bookmarkFor(NSURL* url) {
    NSData* data = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                 includingResourceValuesForKeys:nil
                                  relativeToURL:nil
                                          error:nil];
    return data ? std::string([data base64EncodedStringWithOptions:0].UTF8String) : std::string();
}

std::string makeFolderBookmark(const std::string& folder) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:folder.c_str()];
        if (!path) return "";
        return bookmarkFor([NSURL fileURLWithPath:path isDirectory:YES]);
    }
}

std::string openFolderBookmark(std::string& bookmark) {
    @autoreleasepool {
        NSString* text = [NSString stringWithUTF8String:bookmark.c_str()];
        NSData* data = text ? [[NSData alloc] initWithBase64EncodedString:text options:0] : nil;
        if (!data) return "";
        BOOL stale = NO;
        NSURL* url = [NSURL URLByResolvingBookmarkData:data
                                               options:NSURLBookmarkResolutionWithSecurityScope |
                                                       NSURLBookmarkResolutionWithoutUI
                                         relativeToURL:nil
                                   bookmarkDataIsStale:&stale
                                                 error:nil];
        // Access lasts until the app quits, which is what the library needs.
        if (!url || ![url startAccessingSecurityScopedResource]) return "";
        if (stale) {
            if (std::string fresh = bookmarkFor(url); !fresh.empty()) bookmark = fresh;
        }
        return url.fileSystemRepresentation;
    }
}

}  // namespace macui
