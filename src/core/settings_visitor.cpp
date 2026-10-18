#include "core/settings_visitor.h"

#include <cctype>

/*
* Words are runs of letters and digits; everything else (spaces, brackets) only separates them. The first word is
* lowered whole, so an abbreviation leading the label ("GPU validation") does not start the key in capitals; later
* words keep their case apart from a capital first letter, so "Target FPS" stays readable as targetFPS.
*/
std::string settingKey(const char* label) {
    std::string key;
    bool inFirstWord = true;
    bool startOfWord = true;

    for (const char* character = label; *character != '\0'; ++character) {
        const unsigned char letter = static_cast<unsigned char>(*character);

        if (!std::isalnum(letter)) {
            startOfWord = true;

            if (!key.empty()) {
                inFirstWord = false;
            }

            continue;
        }

        if (inFirstWord) {
            key += static_cast<char>(std::tolower(letter));
        }
        else if (startOfWord) {
            key += static_cast<char>(std::toupper(letter));
        }
        else {
            key += static_cast<char>(letter);
        }

        startOfWord = false;
    }

    return key;
}
