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
placeholder = re.compile(r"\{[A-Za-z0-9_.]+\}")


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


files = {path.stem for path in root.glob("*.txt")}
if files != languages:
    raise ValueError(f"language files differ: missing={languages - files}, extra={files - languages}")

english = load(root / "en.txt")
source = "\n".join(path.read_text(encoding="utf-8") for path in root.parent.joinpath("src").rglob("*.cpp"))
source_keys = set(re.findall(r'"((?:announcement|evidence|webhook)\.[A-Za-z0-9_.]+)"', source))
source_keys.discard("webhook.h")
if source_keys != english.keys():
    raise ValueError(
        f"English phrases differ from source: missing={source_keys - english.keys()}, "
        f"unused={english.keys() - source_keys}"
    )

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
        if set(placeholder.findall(value)) != set(placeholder.findall(english[key])):
            raise ValueError(f"{path}: placeholders differ for {key}")

print(f"Checked {len(files)} languages with {len(english)} phrases each.")
