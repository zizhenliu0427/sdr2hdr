// -----------------------------------------------------------------------------
// Tiny i18n layer for sdr2hdr.
//
// Design:
//   - Single global `g_lang` picked once at startup (auto-detect + --lang override).
//   - `tr(en, zh)` picks the right literal at call site; both languages live
//     next to each other in source so diff-reviewing translations is trivial.
//   - Scope is limited to user-facing prose: the interactive wizard and
//     `--help` text. Runtime status / error logs stay English because they
//     need to round-trip with external tooling (logs, grep, bug reports).
//
// Auto-detection:
//   - Windows: GetUserDefaultUILanguage(); anything under LANG_CHINESE -> zh.
//   - Otherwise (incl. non-Windows): English.
//   - Environment override SDR2HDR_LANG = "en" | "zh" | "auto" beats auto.
//   - Command-line override `--lang en|zh|auto` beats the env var; parsed
//     EARLY in main() so even --help / unknown-arg errors localise correctly.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace i18n {

enum class Lang { En, Zh };

inline Lang& langRef()
{
    // Default: English. initLang() overrides based on env / OS / CLI.
    static Lang g = Lang::En;
    return g;
}

inline Lang currentLang() { return langRef(); }

inline const char* tr(const char* en, const char* zh)
{
    return langRef() == Lang::Zh ? zh : en;
}

// Returns a newly-constructed std::string for dynamic tr() call sites.
inline std::string trs(const char* en, const char* zh)
{
    return tr(en, zh);
}

inline bool parseLangTag(const char* s, Lang& out)
{
    if (!s || !*s) return false;
    if (!::strcmp(s, "en") || !::strcmp(s, "EN") || !::strcmp(s, "english")) { out = Lang::En; return true; }
    if (!::strcmp(s, "zh") || !::strcmp(s, "ZH") || !::strcmp(s, "chinese")) { out = Lang::Zh; return true; }
    // "auto" is handled by caller (leaves Lang untouched so auto-detect runs).
    return false;
}

inline Lang detectOsLang()
{
#ifdef _WIN32
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_CHINESE) return Lang::Zh;
#endif
    return Lang::En;
}

// cliOverride is whatever the user passed to `--lang` (empty / "auto" -> ignore).
// Honoured precedence: explicit CLI > env var > OS UI language > English default.
inline void initLang(const char* cliOverride = nullptr)
{
    Lang chosen = detectOsLang();

    if (const char* env = std::getenv("SDR2HDR_LANG"))
    {
        Lang fromEnv;
        if (parseLangTag(env, fromEnv))
            chosen = fromEnv;
    }

    if (cliOverride && *cliOverride)
    {
        Lang fromCli;
        if (parseLangTag(cliOverride, fromCli))
            chosen = fromCli;
    }

    langRef() = chosen;
}

} // namespace i18n
