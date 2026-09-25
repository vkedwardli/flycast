#import <Foundation/Foundation.h>

#include "gdxsv_update.h"
#include "log/Log.h"
#include "version.h"

bool GdxsvUpdate::InstallMacUpdate(const std::string& source_path, const std::string& app_path) {
	@autoreleasepool {
		NSFileManager* files = [NSFileManager defaultManager];
		NSURL* source = [NSURL fileURLWithPath:[NSString stringWithUTF8String:source_path.c_str()]];
		NSURL* app = [NSURL fileURLWithPath:[NSString stringWithUTF8String:app_path.c_str()]];
		NSError* error = nil;

		// rename() cannot move the downloaded bundle from /tmp to another volume.
		// Stage a complete copy on the app's volume before replacing the old app.
		NSURL* staging_dir = [files URLForDirectory:NSItemReplacementDirectory inDomain:NSUserDomainMask
			appropriateForURL:app create:YES error:&error];
		if (staging_dir == nil) {
			ERROR_LOG(COMMON, "Cannot create update staging directory: %s", [[error description] UTF8String]);
			return false;
		}
		NSURL* staged_app = [staging_dir URLByAppendingPathComponent:[app lastPathComponent]];
		if (![files copyItemAtURL:source toURL:staged_app error:&error]) {
			ERROR_LOG(COMMON, "Cannot copy update to app volume: %s", [[error description] UTF8String]);
			[files removeItemAtURL:staging_dir error:nil];
			return false;
		}

		auto current_version = std::string(GIT_VERSION);
		if (current_version.compare(0, 6, "gdxsv-") == 0)
			current_version.erase(0, 6);
		NSString* versioned_name = [NSString stringWithUTF8String:GetFlycastFileNameWithVersion(current_version).c_str()];
		// Replacement can overwrite an existing backup, so use a unique name
		// during installation, then give it the version-only name if available.
		NSString* backup_name = [NSString stringWithFormat:@"%@-%@.app",
			[versioned_name stringByDeletingPathExtension], [[NSUUID UUID] UUIDString]];
		NSURL* backup = [[app URLByDeletingLastPathComponent] URLByAppendingPathComponent:backup_name];
		if (![files replaceItemAtURL:app withItemAtURL:staged_app backupItemName:backup_name
			options:NSFileManagerItemReplacementUsingNewMetadataOnly | NSFileManagerItemReplacementWithoutDeletingBackupItem
			resultingItemURL:nil error:&error]) {
			ERROR_LOG(COMMON, "Cannot replace app: %s; retaining recovery files in %s", [[error description] UTF8String],
				[[staging_dir path] UTF8String]);
			return false;
		}

		NSURL* versioned_backup = [[app URLByDeletingLastPathComponent] URLByAppendingPathComponent:versioned_name];
		if ([files moveItemAtURL:backup toURL:versioned_backup error:nil])
			backup = versioned_backup;

		// Let macOS choose the volume's Trash instead of assuming ~/.Trash.
		// If Trash is unavailable, the update still succeeds and the backup stays.
		if (![files trashItemAtURL:backup resultingItemURL:nil error:&error]) {
			WARN_LOG(COMMON, "Update installed; old app retained at %s: %s", [[backup path] UTF8String],
				[[error description] UTF8String]);
		}
		[files removeItemAtURL:staging_dir error:nil];
		return true;
	}
}
