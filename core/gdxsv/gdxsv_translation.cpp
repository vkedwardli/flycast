//
//  gdxsv_translation.cpp
//  gdxsv
//
//  Created by Edward Li on 15/7/2021.
//  Copyright © 2021 flycast. All rights reserved.
//
#ifdef _WIN32
#define _AMD64_	 // Fixing GitHub runner's winnt.h error
#endif

#include "gdxsv_translation.h"

#include "cfg/option.h"
#include "oslib/i18n.h"

const char* GdxsvTranslation::Text() const {
	switch (GdxsvLanguage::Language()) {
		case GdxsvLanguage::Lang::English:
			return english;
		case GdxsvLanguage::Lang::Cantonese:
			return cantonese;
		case GdxsvLanguage::Lang::Japanese:
			return japanese ? japanese : original;
	}
	return original;
}

GdxsvLanguage::Lang GdxsvLanguage::Language() {
	Lang lang = static_cast<Lang>(config::GdxLanguage.get());
	switch (lang) {
		case Lang::Japanese:
		case Lang::Cantonese:
		case Lang::English:
		case Lang::Disabled:
			return lang;
		case Lang::NOT_SET:
		default:
			lang = LanguageFromOS();
			config::GdxLanguage.set((int)lang);
			return lang;
	}
}

std::string GdxsvLanguage::TextureDirectoryName() {
	switch (Language()) {
		case Lang::English:
			return "English";
		case Lang::Cantonese:
			return "Cantonese";
		default:
			return "Japanese";
	}
}

#ifdef _WIN32
#include <winnls.h>

#include <codecvt>
#include <locale>
#endif

GdxsvLanguage::Lang GdxsvLanguage::LanguageFromOS() {
	std::string locale = i18n::getCurrentLocale();
#ifdef __APPLE__
	time_t ts = 0;
	struct tm t;
	char buf[16];
	localtime_r(&ts, &t);
	strftime(buf, sizeof(buf), "%z%Z", &t);
#endif

	if (locale.find("ja") == 0
#ifdef __APPLE__
		|| strcmp(buf, "+0900JST") == 0
#elif _WIN32
		|| strcmp(_tzname[0], "Tokyo Standard Time") == 0
#endif
	)
		return Lang::Japanese;
	else if (locale.find("yue") == 0 || locale.find("zh") == 0	// Chinese fallback
#ifdef __APPLE__
			 || strcmp(buf, "+0800HKT") == 0  // Cantonese users love using English OS
			 || strcmp(buf, "+0800CST") == 0
#elif _WIN32
			 || strcmp(_tzname[0], "China Standard Time") == 0 || strcmp(_tzname[0], "Taipei Standard Time") == 0
#endif
	)
		return Lang::Cantonese;
	else
		return Lang::English;
}
