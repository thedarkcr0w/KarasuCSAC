#pragma once

#include "common.h"

namespace localization
{
	struct Argument
	{
		std::string_view name;
		std::string value;
	};

	using Arguments = std::initializer_list<Argument>;

	struct Text
	{
		std::string english;
		std::string localized;
	};

	void Reload(const char *language);
	void Shutdown();
	const char *CurrentLanguage();
	std::string Get(const char *key, const char *english);
	Text Format(const char *key, const char *english, Arguments arguments = {});
	Text Watermark(Arguments arguments = {});
} // namespace localization
