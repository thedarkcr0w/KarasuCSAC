#include "localization.h"

#include "KeyValues.h"
#include "filesystem.h"

#include <cctype>

namespace
{
	std::unordered_map<std::string, std::string> phrases;
	std::string currentLanguage {"en"};
	bool fallbackWarningPrinted;
	constexpr std::pair<std::string_view, std::string_view> watermarkPhrases[] = {
		{"ar", "هذا الخادم محمي بنظام مكافحة الغش الخاص بـ {author}."},
		{"bg", "Този сървър е защитен от анти-чита на {author}."},
		{"cs", "Tento server je chráněn anti-cheatem od {author}."},
		{"da", "Denne server beskyttes af {author}s anti-cheat."},
		{"de", "Dieser Server wird durch den Anti-Cheat von {author} geschützt."},
		{"el", "Αυτός ο διακομιστής προστατεύεται από το anti-cheat του {author}."},
		{"en", "This server is protected by {author}'s anti-cheat."},
		{"es-419", "Este servidor está protegido por el anticheat de {author}."},
		{"es-es", "Este servidor está protegido por el antitrampas de {author}."},
		{"et", "Seda serverit kaitseb {author} anti-cheat."},
		{"fi", "Tätä palvelinta suojaa {author} anti-cheat."},
		{"fr", "Ce serveur est protégé par l'anti-triche de {author}."},
		{"he", "שרת זה מוגן על ידי האנטי-צ'יט של {author}."},
		{"hr", "Ovaj je poslužitelj zaštićen anti-cheatom autora {author}."},
		{"hu", "Ezt a szervert {author} anti-cheat rendszere védi."},
		{"id", "Server ini dilindungi oleh anti-cheat milik {author}."},
		{"it", "Questo server è protetto dall'anti-cheat di {author}."},
		{"ja", "このサーバーは {author} のアンチチートで保護されています。"},
		{"ko", "이 서버는 {author}의 안티치트로 보호됩니다."},
		{"lt", "Šį serverį saugo {author} anti-cheat."},
		{"lv", "Šo serveri aizsargā {author} anti-cheat."},
		{"nl", "Deze server wordt beschermd door de anti-cheat van {author}."},
		{"no", "Denne serveren beskyttes av anti-cheaten til {author}."},
		{"pl", "Ten serwer jest chroniony przez antycheat {author}."},
		{"pt-br", "Este servidor é protegido pelo anticheat de {author}."},
		{"pt-pt", "Este servidor está protegido pelo anticheat de {author}."},
		{"ro", "Acest server este protejat de sistemul anti-cheat al lui {author}."},
		{"ru", "Этот сервер защищён античитом {author}."},
		{"sk", "Tento server je chránený anti-cheatom od {author}."},
		{"sr", "Ovaj server je zaštićen anti-cheatom autora {author}."},
		{"sv", "Den här servern skyddas av {author}s anti-cheat."},
		{"th", "เซิร์ฟเวอร์นี้ได้รับการปกป้องโดยระบบป้องกันโกงของ {author}."},
		{"tr", "Bu sunucu {author} anti-hilesi tarafından korunuyor."},
		{"uk", "Цей сервер захищено античитом {author}."},
		{"vi", "Máy chủ này được bảo vệ bởi hệ thống chống gian lận của {author}."},
		{"zh-cn", "本服务器受 {author} 的反作弊系统保护。"},
		{"zh-tw", "本伺服器受 {author} 的反作弊系統保護。"},
	};

	void WarnFallback()
	{
		if (!fallbackWarningPrinted)
		{
			Msg("[CS2AC] Some phrases for language '%s' are missing or invalid. English will be used for them.\n", currentLanguage.c_str());
			fallbackWarningPrinted = true;
		}
	}

	std::string NormalizeLanguage(const char *language)
	{
		std::string result = language ? language : "";
		if (result.empty() || result.size() > 16)
		{
			return {};
		}
		for (char &character : result)
		{
			if (character == '_')
			{
				character = '-';
			}
			else if (std::isalnum(static_cast<unsigned char>(character)))
			{
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}
			else if (character != '-')
			{
				return {};
			}
		}
		return result;
	}

	std::set<std::string> Placeholders(std::string_view text)
	{
		std::set<std::string> result;
		for (std::size_t start = text.find('{'); start != std::string_view::npos; start = text.find('{', start + 1))
		{
			const std::size_t end = text.find('}', start + 1);
			if (end == std::string_view::npos)
			{
				break;
			}
			const std::string_view name = text.substr(start + 1, end - start - 1);
			// Chat color directives are formatting, not translation arguments.
			if (!name.empty()
				&& std::all_of(name.begin(), name.end(),
							   [](unsigned char character) { return std::isalnum(character) || character == '_' || character == '.'; })
				&& name != "red" && name != "lime" && name != "grey" && name != "blue" && name != "default")
			{
				result.emplace(name);
			}
		}
		return result;
	}

	std::string Apply(std::string_view text, localization::Arguments arguments)
	{
		std::string result;
		result.reserve(text.size() + 32);
		for (std::size_t index = 0; index < text.size();)
		{
			if (text[index] != '{')
			{
				result += text[index++];
				continue;
			}
			const std::size_t end = text.find('}', index + 1);
			if (end == std::string_view::npos)
			{
				result.append(text.substr(index));
				break;
			}
			const std::string_view name = text.substr(index + 1, end - index - 1);
			const auto found =
				std::find_if(arguments.begin(), arguments.end(), [name](const localization::Argument &argument) { return argument.name == name; });
			if (found == arguments.end())
			{
				result.append(text.substr(index, end - index + 1));
			}
			else
			{
				result += found->value;
			}
			index = end + 1;
		}
		return result;
	}
} // namespace

void localization::Reload(const char *language)
{
	phrases.clear();
	fallbackWarningPrinted = false;
	currentLanguage = NormalizeLanguage(language);
	if (currentLanguage.empty())
	{
		currentLanguage = "en";
		WarnFallback();
		return;
	}
	if (!g_pFullFileSystem)
	{
		WarnFallback();
		return;
	}

	const std::string path = "addons/cs2ac/translations/" + currentLanguage + ".txt";
	KeyValues file("Phrases");
	if (!file.LoadFromFile(g_pFullFileSystem, path.c_str(), nullptr))
	{
		WarnFallback();
		return;
	}
	for (KeyValues *phrase = file.GetFirstValue(); phrase; phrase = phrase->GetNextValue())
	{
		const char *value = phrase->GetString(nullptr);
		if (value && *value)
		{
			phrases[phrase->GetName()] = value;
		}
	}
}

void localization::Shutdown()
{
	phrases.clear();
	currentLanguage = "en";
	fallbackWarningPrinted = false;
}

const char *localization::CurrentLanguage()
{
	return currentLanguage.c_str();
}

std::string localization::Get(const char *key, const char *english)
{
	const std::string fallback = english ? english : "";
	const auto found = key ? phrases.find(key) : phrases.end();
	if (found == phrases.end() || Placeholders(found->second) != Placeholders(fallback))
	{
		if (found == phrases.end() || found->second != fallback)
		{
			WarnFallback();
		}
		return fallback;
	}
	return found->second;
}

localization::Text localization::Format(const char *key, const char *english, Arguments arguments)
{
	return {Apply(english ? english : "", arguments), Apply(Get(key, english), arguments)};
}

localization::Text localization::Watermark(Arguments arguments)
{
	constexpr std::string_view english = "This server is protected by {author}'s anti-cheat.";
	const auto found =
		std::find_if(std::begin(watermarkPhrases), std::end(watermarkPhrases), [](const auto &phrase) { return phrase.first == currentLanguage; });
	const std::string_view localized = found == std::end(watermarkPhrases) ? english : found->second;
	return {Apply(english, arguments), Apply(localized, arguments)};
}
