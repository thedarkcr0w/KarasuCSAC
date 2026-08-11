from pathlib import Path
import re


root = Path(__file__).with_name("translations")
languages = {
    "ar", "bg", "cs", "da", "de", "el", "en", "es-419", "es-es", "et",
    "fi", "fr", "he", "hr", "hu", "id", "it", "ja", "ko", "lt", "lv",
    "nl", "no", "pl", "pt-br", "pt-pt", "ro", "ru", "sk", "sr", "sv",
    "th", "tr", "uk", "vi", "zh-cn", "zh-tw",
}
entry = re.compile(r'^\s*"([^"\\]+)"\s+"((?:\\.|[^"\\])*)"\s*$')
# These tokens are chat formatting directives, not translation arguments.
placeholder = re.compile(r"\{[A-Za-z0-9_.]+\}")
chat_markup = {"red", "lime", "grey", "blue", "default"}
evidence_template_limit = 700  # Discord allows 1024 characters; dynamic values need the remaining space.
technical_compact_keys = {
    "evidence.aimbot.snap_return",
    "evidence.aimbot.convergence",
    "evidence.aimbot.smooth",
    "evidence.aimbot.humanized",
    "evidence.silentaim",
    "evidence.triggerbot",
    "evidence.antiaim.category_entry",
    "webhook.field.steamid64",
}
aimbot_score_keys = {
    "evidence.aimbot.latest.snap_return",
    "evidence.aimbot.latest.convergence",
    "evidence.aimbot.latest.smooth",
    "evidence.aimbot.latest.humanized",
}


def load(path: Path) -> dict[str, str]:
    phrases: dict[str, str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped in {'"Phrases"', "{", "}"}:
            continue
        match = entry.fullmatch(line)
        if not match:
            raise ValueError(f"{path}:{number}: malformed KeyValues entry")
        key, value = match.groups()
        if key in phrases:
            raise ValueError(f"{path}:{number}: duplicate phrase {key}")
        phrases[key] = value
    return phrases


def placeholders(value: str) -> set[str]:
    return {match[1:-1] for match in placeholder.findall(value) if match[1:-1] not in chat_markup}


files = {path.stem for path in root.glob("*.txt")}
if files != languages:
    raise ValueError(f"language files differ: missing={languages - files}, extra={files - languages}")

english = load(root / "en.txt")
for key in aimbot_score_keys:
    if "score" not in placeholders(english[key]) or "incidents" in placeholders(english[key]):
        raise ValueError(f"{key} must expose the weighted score, not an incident count")
source = "\n".join(path.read_text(encoding="utf-8") for path in root.parent.joinpath("src").rglob("*.cpp"))
source_keys = set(re.findall(r'"((?:announcement|evidence|webhook)\.[A-Za-z0-9_.]+)"', source))
source_keys.discard("webhook.h")
if source_keys != english.keys():
    raise ValueError(
        f"English phrases differ from source: missing={source_keys - english.keys()}, "
        f"unused={english.keys() - source_keys}"
    )

raw_localized_mutations = re.findall(
    r"\.localized\s*(?:\+=|=)|\.localized\.(?:append|assign|insert|replace)\s*\(",
    source,
)
if raw_localized_mutations:
    raise ValueError("localized text must be composed through localization::Text instead of being mutated directly")

watermark_languages = set(re.findall(r'^\s*\{"([^"]+)", "[^"]*\{author\}[^"]*"\},$', source, re.MULTILINE))
if watermark_languages != languages:
    raise ValueError(
        f"hardcoded watermark languages differ: missing={languages - watermark_languages}, "
        f"extra={watermark_languages - languages}"
    )

for path in sorted(root.glob("*.txt")):
    phrases = load(path)
    if phrases.keys() != english.keys():
        raise ValueError(
            f"{path}: phrase keys differ: missing={english.keys() - phrases.keys()}, "
            f"extra={phrases.keys() - english.keys()}"
        )
    for key, value in phrases.items():
        if placeholders(value) != placeholders(english[key]):
            raise ValueError(f"{path}: placeholders differ for {key}")
        if "@{" in value:
            raise ValueError(f"{path}: stray @ before placeholder for {key}")
        if "?" in value:
            raise ValueError(f"{path}: replacement character '?' in {key}")
        if path.stem != "en" and value == english[key] and len(value) > 30 and key not in technical_compact_keys:
            raise ValueError(f"{path}: untranslated English phrase for {key}")
        if key.startswith("evidence.") and len(value) > evidence_template_limit:
            raise ValueError(f"{path}: {key} is too long for a readable Discord evidence field")

print(f"Checked {len(files)} languages with {len(english)} phrases each.")
