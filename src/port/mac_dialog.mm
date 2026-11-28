// Native macOS file and folder picker (NSOpenPanel), replacing the osascript
// helper so .app bundles are selectable as files and the dialog runs in-process
// in front of the SDL window.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

namespace port {

std::string MacPickPath(bool folder, bool image, bool keys) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.allowsMultipleSelection = NO;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;

        if (folder) {
            panel.canChooseFiles = NO;
            panel.canChooseDirectories = YES;
            panel.canCreateDirectories = YES;
        } else {
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            if (image) {
                panel.allowedContentTypes = @[ UTTypePNG, UTTypeJPEG ];
            } else if (keys) {
                panel.message = @"prod.keys";
            } else {
                // Programs: explicitly include UTTypeApplicationBundle / UTTypeApplication
                // WITHOUT UTTypeItem, because NSOpenPanel greys out .app bundles when
                // allowedContentTypes is empty or includes the wildcard UTTypeItem.
                panel.allowedContentTypes = @[ UTTypeApplicationBundle, UTTypeApplication, UTTypeUnixExecutable ];
                panel.directoryURL = [NSURL fileURLWithPath:@"/Applications" isDirectory:YES];
            }
        }

        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return {};
        NSURL* url = panel.URL;
        if (!url || !url.fileURL) return {};
        const char* fsPath = url.fileSystemRepresentation;
        return fsPath ? std::string(fsPath) : std::string();
    }
}

} // namespace port
