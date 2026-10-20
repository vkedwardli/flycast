// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_platform.h"
#include "gdxsv_emu_hooks.h"

#include <string_view>
#ifdef USE_SDL
#include <SDL.h>
#endif

#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>
#import <SystemConfiguration/SystemConfiguration.h>

bool gdxsv_emu_app_init(int argc, const char *const argv[]) {
#if defined(VIDEO_ROUTING) && defined(USE_SDL) && !defined(LIBRETRO)
	// Only the host's explicit worker arguments suppress application activation.
	// The ordinary configuration is not loaded until after AppKit starts.
	std::string_view session, screen;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if ((arg == "-config" || arg == "--config") && i + 1 < argc) {
			const std::string_view option = argv[++i];
			const auto equals = option.find('=');
			if (equals == std::string_view::npos) continue;
			const auto key = option.substr(0, equals);
			const auto value = option.substr(equals + 1);
			if (key == "gdxsv:MultiPovSession")
				session = value;
			else if (key == "gdxsv:MultiPovScreen")
				screen = value;
		} else if (!arg.empty() && arg.front() != '-') {
			break;	// The content path ends Flycast's option parsing.
		}
	}
	if (session.empty() || (screen != "1" && screen != "2" && screen != "3")) return false;
	SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
	if (![[NSApplication sharedApplication] setActivationPolicy:NSApplicationActivationPolicyAccessory])
		NSLog(@"multi-pov: could not hide the guest from the Dock");
	return true;
#else
	return false;
#endif
}

void gdxsv_open_url(const std::string& url) {
    NSString *urlString = [NSString stringWithUTF8String:url.c_str()];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:urlString]];
}

std::string gdxsv_machine_id(){
    io_service_t    platformExpert = IOServiceGetMatchingService(kIOMasterPortDefault,IOServiceMatching("IOPlatformExpertDevice"));
    CFStringRef serialNumberAsCFString = NULL;

    if (platformExpert) {
        serialNumberAsCFString =
        (CFStringRef)IORegistryEntryCreateCFProperty(platformExpert,
                                        CFSTR(kIOPlatformSerialNumberKey),
                                        kCFAllocatorDefault, 0);
        IOObjectRelease(platformExpert);
        if (serialNumberAsCFString) {
            CFIndex bufferSize = CFStringGetLength(serialNumberAsCFString);
            bufferSize = CFStringGetMaximumSizeForEncoding(bufferSize, kCFStringEncodingUTF8);
            char value[bufferSize+1];

            if (CFStringGetCString(serialNumberAsCFString, value, bufferSize, kCFStringEncodingUTF8))
            {
                std::string s;
                s += value;
                return s;
            }
        }
    }
    return "";
}

std::string gdxsv_connection_medium() {
	std::string result = "Unknown";
	@autoreleasepool {
		SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("flycast"), NULL, NULL);
		if (!store) return result;

		auto getVal = [&](CFStringRef key) -> id {
			if (!key) return nil;
			id val = (id)SCDynamicStoreCopyValue(store, key);
			CFRelease(key);
			return [val autorelease];
		};

		NSDictionary *ipv4Global = getVal(CFSTR("State:/Network/Global/IPv4"));
		NSDictionary *ipv6Global = getVal(CFSTR("State:/Network/Global/IPv6"));

		NSDictionary *globalList = nil;
		for (NSDictionary *global : { ipv6Global, ipv4Global }) {
			if (!global) continue;
			NSString* interface = [global objectForKey:@"PrimaryInterface"];
			if ([interface hasPrefix:@"utun"] || [interface hasPrefix:@"ppp"] || [interface hasPrefix:@"tun"] || [interface hasPrefix:@"vpn"]) {
				result = "VPN";
				globalList = global;
				break;
			}
		}

		if (!globalList)
			globalList = ipv6Global ?: ipv4Global;
		
		NSString *bestInterface = [globalList objectForKey:@"PrimaryInterface"];
		NSString *serviceID = [globalList objectForKey:@"PrimaryService"];

		if (result == "VPN" && bestInterface) {
			NSArray *keys = (NSArray *)SCDynamicStoreCopyKeyList(store, CFSTR("Setup:/Network/Service/[^/]+$"));
			NSString *bestMatch = nil;

			for (NSString *key in [keys autorelease]) {
				NSString *sID = [key lastPathComponent];
				
				NSDictionary *idict = getVal(SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainSetup, (CFStringRef)sID, kSCEntNetInterface));
				NSString *type = [idict objectForKey:(NSString *)kSCPropNetInterfaceType];
				NSString *dev = [idict objectForKey:(NSString *)kSCPropNetInterfaceDeviceName];
				
				if (!dev) {
					NSDictionary *sdict = getVal(SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainState, (CFStringRef)sID, CFSTR("IPv4")))
					                   ?: getVal(SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainState, (CFStringRef)sID, CFSTR("IPv6")));
					dev = [sdict objectForKey:@"InterfaceName"];
				}

				if ([dev isEqualToString:bestInterface]) {
					bestMatch = sID;
					break;
				}
				
				if (!([type isEqualToString:@"VPN"] || [type isEqualToString:@"PPP"])) continue;

				SCNetworkConnectionRef conn = SCNetworkConnectionCreateWithServiceID(NULL, (CFStringRef)sID, NULL, NULL);
				if (conn) {
					if (SCNetworkConnectionGetStatus(conn) == kSCNetworkConnectionConnected)
						bestMatch = bestMatch ?: sID;
					CFRelease(conn);
				}
				if (bestMatch) break;
			}

			NSDictionary *nd = getVal(SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainSetup, (CFStringRef)bestMatch, NULL));
			NSString *name = [nd objectForKey:(NSString *)kSCPropUserDefinedName];
			if ([name length] > 0)
				result = "VPN: " + std::string([name UTF8String]);
		}

		if ((result == "Unknown" || result == "VPN") && serviceID) {
			NSDictionary *setupDict = getVal(SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainSetup, (CFStringRef)serviceID, kSCEntNetInterface));
			NSString *hardware = [setupDict objectForKey:(NSString *)kSCPropNetInterfaceHardware];
			if ([hardware isEqualToString:@"AirPort"] || [hardware isEqualToString:@"Bluetooth"] || [hardware isEqualToString:@"IrDA"])
				result = "Wireless";
			else if ([hardware isEqualToString:@"Ethernet"])
				result = "Wired";
		}

		CFRelease(store);
	}
	return result;
}
