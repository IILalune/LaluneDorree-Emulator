// mac_ui.mm - Objective-C++ (compiled with ARC) for native macOS features.
#include "mac_ui.hpp"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
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

void openInFinder(const std::string& folder) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:folder.c_str()];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path isDirectory:YES]];
    }
}

}  // namespace macui
