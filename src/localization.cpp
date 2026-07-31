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
		{"ar", "هذا الخادم يشغّل نسخة مخصصة من نظام مكافحة الغش الخاص بـ {author}."},
		{"bg", "Този сървър използва персонализирана версия на анти-чита на {author}."},
		{"cs", "Tento server používá upravenou verzi anti-cheatu od {author}."},
		{"da", "Denne server kører en tilpasset version af {author}s anti-cheat."},
		{"de", "Auf diesem Server läuft eine angepasste Version des Anti-Cheats von {author}."},
		{"el", "Αυτός ο διακομιστής εκτελεί μια προσαρμοσμένη έκδοση του anti-cheat του {author}."},
		{"en", "This server is running a custom version of {author}'s anti-cheat."},
		{"es-419", "Este servidor ejecuta una versión personalizada del anticheat de {author}."},
		{"es-es", "Este servidor ejecuta una versión personalizada del antitrampas de {author}."},
		{"et", "See server kasutab {author} anti-cheati kohandatud versiooni."},
		{"fi", "Tämä palvelin käyttää mukautettua versiota käyttäjän {author} anti-cheatista."},
		{"fr", "Ce serveur utilise une version personnalisée de l'anti-triche de {author}."},
		{"he", "שרת זה מריץ גרסה מותאמת של האנטי-צ'יט של {author}."},
		{"hr", "Ovaj poslužitelj koristi prilagođenu verziju anti-cheata autora {author}."},
		{"hu", "Ez a szerver {author} anti-cheat rendszerének egyedi változatát futtatja."},
		{"id", "Server ini menjalankan versi kustom dari anti-cheat milik {author}."},
		{"it", "Questo server esegue una versione personalizzata dell'anti-cheat di {author}."},
		{"ja", "このサーバーは {author} のアンチチートのカスタム版を実行しています。"},
		{"ko", "이 서버는 {author}의 안티치트 커스텀 버전을 실행 중입니다."},
		{"lt", "Šis serveris naudoja pritaikytą {author} anti-cheat versiją."},
		{"lv", "Šis serveris izmanto pielāgotu {author} anti-cheat versiju."},
		{"nl", "Deze server draait een aangepaste versie van de anti-cheat van {author}."},
		{"no", "Denne serveren kjører en tilpasset versjon av anti-cheaten til {author}."},
		{"pl", "Ten serwer korzysta z niestandardowej wersji antycheata {author}."},
		{"pt-br", "Este servidor executa uma versão personalizada do anticheat de {author}."},
		{"pt-pt", "Este servidor executa uma versão personalizada do anticheat de {author}."},
		{"ro", "Acest server rulează o versiune personalizată a sistemului anti-cheat al lui {author}."},
		{"ru", "На этом сервере работает изменённая версия античита {author}."},
		{"sk", "Tento server používa upravenú verziu anti-cheatu od {author}."},
		{"sr", "Ovaj server koristi prilagođenu verziju anti-cheata autora {author}."},
		{"sv", "Den här servern kör en anpassad version av {author}s anti-cheat."},
		{"th", "เซิร์ฟเวอร์นี้ใช้ระบบป้องกันโกงของ {author} เวอร์ชันที่ปรับแต่ง"},
		{"tr", "Bu sunucu {author} anti-hilesinin özelleştirilmiş bir sürümünü çalıştırıyor."},
		{"uk", "На цьому сервері працює змінена версія античиту {author}."},
		{"vi", "Máy chủ này đang chạy phiên bản tùy chỉnh của hệ thống chống gian lận của {author}."},
		{"zh-cn", "本服务器运行的是 {author} 反作弊系统的定制版本。"},
		{"zh-tw", "本伺服器運行的是 {author} 反作弊系統的自訂版本。"},
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
			if (!name.empty()
				&& std::all_of(name.begin(), name.end(),
							   [](unsigned char character) { return std::isalnum(character) || character == '_' || character == '.'; }))
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
	constexpr std::string_view english = "This server is running a custom version of {author}'s anti-cheat.";
	const auto found =
		std::find_if(std::begin(watermarkPhrases), std::end(watermarkPhrases), [](const auto &phrase) { return phrase.first == currentLanguage; });
	const std::string_view localized = found == std::end(watermarkPhrases) ? english : found->second;
	return {Apply(english, arguments), Apply(localized, arguments)};
}
