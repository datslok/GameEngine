#include "core/hash.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
    // The budgets the root CLAUDE.md states ("Where new information goes"): what loads into every session is kept
    // small, and a reference file past its budget splits into narrower topics.
    constexpr std::uintmax_t rootClaudeBudget = 8 * 1024;
    constexpr std::uintmax_t folderRuleBudget = 4 * 1024;
    constexpr std::uintmax_t architectureFileBudget = 20 * 1024;
    constexpr std::uintmax_t readmeBudget = 20 * 1024;

    // A part of a long doc (the build log, the guide) is read whole, so it stays well inside what one read takes.
    constexpr std::uintmax_t partBudget = 40 * 1024;

    // Long docs split into parts, each with the index file that lists them (docs/INDEX.md names the index).
    const std::vector<std::pair<std::string, std::string>> partedDocs{{"docs/buildlog", "docs/BUILDLOG.md"},
                                                                       {"docs/guide", "docs/ENGINE_GUIDE.md"}};

    // Longer than about four sentences is a wall of prose in a file Claude reads as rules. Table rows are exempt.
    constexpr std::size_t longestRuleLine = 400;

    // Twelve identical words in a row are almost never a coincidence, while shorter runs catch stock phrases.
    constexpr std::size_t duplicateRunWords = 12;

    // Path prefixes the docs may name that are build outputs or fetched, git-ignored folders.
    const std::vector<std::string> unversionedPrefixes{"build/", "assets/forest", "external/streamline/bin"};

    // Problems found, printed together so one run shows them all.
    using Problems = std::vector<std::string>;

    // A text file with its carriage returns removed (sources and docs are CRLF).
    std::string readText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        assert(file && "docs_test runs from the repo root");
        std::ostringstream contents;
        contents << file.rdbuf();
        std::string text = contents.str();
        std::erase(text, '\r');
        return text;
    }

    std::vector<std::string> splitLines(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream stream(text);

        for (std::string line; std::getline(stream, line);) {
            lines.push_back(line);
        }

        return lines;
    }

    std::string trimmed(std::string_view text) {
        const std::size_t first = text.find_first_not_of(" \t");

        if (first == std::string_view::npos) {
            return {};
        }

        const std::size_t last = text.find_last_not_of(" \t");
        return std::string(text.substr(first, last - first + 1));
    }

    // Every `inline code` span on a line, in order.
    std::vector<std::string> backticked(std::string_view line) {
        std::vector<std::string> spans;
        std::size_t start = line.find('`');

        while (start != std::string_view::npos) {
            const std::size_t end = line.find('`', start + 1);

            if (end == std::string_view::npos) {
                break;
            }

            spans.emplace_back(line.substr(start + 1, end - start - 1));
            start = line.find('`', end + 1);
        }

        return spans;
    }

    // The lines of a doc outside fenced code blocks: commands quoted in two docs, or example paths, are not prose.
    std::vector<std::string> proseLines(const fs::path& doc) {
        std::vector<std::string> lines;
        bool fenced = false;

        for (const std::string& line : splitLines(readText(doc))) {
            if (trimmed(line).starts_with("```")) {
                fenced = !fenced;
            } else if (!fenced) {
                lines.push_back(line);
            }
        }

        return lines;
    }

    std::string slashed(const fs::path& path) {
        return path.generic_string();
    }

    // Every file in src/, walked once: three checks need it, and a walk of its 600 files costs about 50 ms.
    const std::vector<fs::path>& sourceTreeFiles() {
        static const std::vector<fs::path> files = [] {
            std::vector<fs::path> found;

            for (const fs::directory_entry& entry : fs::recursive_directory_iterator("src")) {
                if (entry.is_regular_file()) {
                    found.push_back(entry.path());
                }
            }

            return found;
        }();

        return files;
    }

    std::vector<fs::path> filesIn(const fs::path& folder, std::string_view extension) {
        std::vector<fs::path> files;

        if (!fs::exists(folder)) {
            return files;
        }

        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder)) {
            if (entry.is_regular_file() && entry.path().extension() == extension) {
                files.push_back(entry.path());
            }
        }

        return files;
    }

    std::vector<fs::path> folderRuleFiles() {
        std::vector<fs::path> files;

        for (const fs::path& file : sourceTreeFiles()) {
            if (file.filename() == "CLAUDE.md") {
                files.push_back(file);
            }
        }

        files.push_back("tests/CLAUDE.md");

        for (const fs::path& rule : filesIn(".claude/rules", ".md")) {
            files.push_back(rule);
        }

        return files;
    }

    // The files Claude reads as instructions: CLAUDE.md files, rules and skills.
    std::vector<fs::path> instructionFiles() {
        std::vector<fs::path> files = folderRuleFiles();
        files.push_back("CLAUDE.md");

        for (const fs::path& skill : filesIn(".claude/skills", ".md")) {
            files.push_back(skill);
        }

        return files;
    }

    /*
    * The docs already written to phase 19's style, checked for stale paths and repeated text. The list only grows: each
    * doc joins when its refactor is done, so a clean doc cannot quietly slide back. Once every doc is on it, this list
    * becomes "every doc".
    */
    std::vector<fs::path> checkedDocs() {
        std::vector<fs::path> docs = instructionFiles();
        docs.push_back("README.md");
        docs.push_back("docs/INDEX.md");
        docs.push_back("docs/LAYOUT.md");

        for (const fs::path& file : filesIn("docs/architecture", ".md")) {
            docs.push_back(file);
        }

        for (const fs::path& file : filesIn("docs/guide", ".md")) {
            docs.push_back(file);
        }

        return docs;
    }

    void checkBudget(const fs::path& file, std::uintmax_t budget, Problems& problems) {
        const std::uintmax_t size = fs::file_size(file);

        if (size > budget) {
            problems.push_back(slashed(file) + " is " + std::to_string(size) + " bytes, over its budget of " +
                               std::to_string(budget));
        }
    }

    void checkBudgets(Problems& problems) {
        checkBudget("CLAUDE.md", rootClaudeBudget, problems);
        checkBudget("README.md", readmeBudget, problems);

        for (const fs::path& file : folderRuleFiles()) {
            checkBudget(file, folderRuleBudget, problems);
        }

        for (const fs::path& file : filesIn("docs/architecture", ".md")) {
            checkBudget(file, architectureFileBudget, problems);
        }

        for (const auto& [folder, index] : partedDocs) {
            for (const fs::path& file : filesIn(folder, ".md")) {
                checkBudget(file, partBudget, problems);
            }
        }
    }

    void checkLineLengths(Problems& problems) {
        for (const fs::path& file : instructionFiles()) {
            const std::vector<std::string> lines = proseLines(file);

            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].size() > longestRuleLine && !trimmed(lines[i]).starts_with('|')) {
                    problems.push_back(slashed(file) + ": a line of " + std::to_string(lines[i].size()) +
                                       " characters, over " + std::to_string(longestRuleLine) + ": split it");
                }
            }
        }
    }

    // ---- Layers -----------------------------------------------------------------------------------------------------

    struct LayerTable {
        std::map<std::string, int> layerOf;
        std::map<std::string, std::set<std::string>> neverIncludes;
        std::set<std::pair<std::string, std::string>> exceptions; // (including file, included path), from src
    };

    std::string folderName(std::string span) {
        if (span.ends_with('/')) {
            span.pop_back();
        }

        return span;
    }

    /*
    * docs/LAYOUT.md's table, read rather than copied, so the doc and the check cannot disagree: rows "| layer | `folder/`
    * | holds | `never/`, ... |" and, under "## Known exceptions", lines "- `file` includes `path`".
    */
    LayerTable readLayerTable() {
        LayerTable table;
        bool inExceptions = false;

        for (const std::string& line : splitLines(readText("docs/LAYOUT.md"))) {
            if (line.starts_with("## ")) {
                inExceptions = line == "## Known exceptions";
                continue;
            }

            if (inExceptions && line.starts_with("- ")) {
                const std::vector<std::string> spans = backticked(line);
                assert(spans.size() >= 2 && "a known exception names the including file and the included path");
                table.exceptions.emplace(spans[0], spans[1]);
                continue;
            }

            if (!line.starts_with("| ")) {
                continue;
            }

            std::vector<std::string> cells;
            std::istringstream row(line.substr(1));

            for (std::string cell; std::getline(row, cell, '|');) {
                cells.push_back(trimmed(cell));
            }

            if (cells.size() < 4 || cells[0].empty() || !std::isdigit(static_cast<unsigned char>(cells[0][0]))) {
                continue;
            }

            const std::string folder = folderName(backticked(cells[1]).at(0));
            table.layerOf[folder] = std::stoi(cells[0]);

            for (const std::string& never : backticked(cells[3])) {
                table.neverIncludes[folder].insert(folderName(never));
            }
        }

        return table;
    }

    /*
    * The quoted paths of a source's #include "..." lines. Found by searching the whole text rather than splitting it
    * into lines first: 600 sources a run, so the split was most of the check's time.
    */
    std::vector<std::string> quotedIncludes(const std::string& text) {
        constexpr std::string_view directive = "#include \"";
        std::vector<std::string> paths;

        for (std::size_t at = text.find(directive); at != std::string::npos; at = text.find(directive, at + 1)) {
            const std::size_t lineStart = text.find_last_of('\n', at) + 1; // npos + 1 = 0 on the first line

            if (text.find_first_not_of(" \t", lineStart) != at) {
                continue; // inside a comment or a string, not a directive
            }

            const std::size_t start = at + directive.size();
            const std::size_t end = text.find('"', start);

            if (end != std::string::npos) {
                paths.push_back(text.substr(start, end - start));
            }
        }

        return paths;
    }

    void checkLayers(Problems& problems) {
        const LayerTable table = readLayerTable();
        assert(table.layerOf.size() >= 10 && "LAYOUT.md's table was read");
        std::set<std::pair<std::string, std::string>> exceptionsSeen;

        for (const fs::path& path : sourceTreeFiles()) {
            if (path.extension() != ".h" && path.extension() != ".cpp") {
                continue;
            }

            const std::string file = slashed(path.lexically_relative("src"));
            const std::string folder = file.substr(0, file.find('/'));

            if (!table.layerOf.contains(folder)) {
                problems.push_back("src/" + folder + "/ is missing from docs/LAYOUT.md's table");
                continue;
            }

            for (const std::string& included : quotedIncludes(readText(path))) {
                if (included.find("../") != std::string::npos) {
                    problems.push_back("src/" + file + " includes \"" + included + "\": include from the src root");
                    continue;
                }

                const std::size_t slash = included.find('/');

                if (slash == std::string::npos) {
                    continue;
                }

                const std::string includedFolder = included.substr(0, slash);

                if (includedFolder == folder || !table.layerOf.contains(includedFolder)) {
                    continue; // its own folder, or a library in external/
                }

                const bool forbidden = table.layerOf.at(includedFolder) >= table.layerOf.at(folder) ||
                                       (table.neverIncludes.contains(folder) &&
                                        table.neverIncludes.at(folder).contains(includedFolder));

                if (!forbidden) {
                    continue;
                }

                if (table.exceptions.contains({file, included})) {
                    exceptionsSeen.emplace(file, included);
                } else {
                    problems.push_back("src/" + file + " includes " + included + ": " + folder +
                                       "/ may not include " + includedFolder + "/ (docs/LAYOUT.md)");
                }
            }
        }

        for (const auto& exception : table.exceptions) {
            if (!exceptionsSeen.contains(exception)) {
                problems.push_back("docs/LAYOUT.md's exception " + exception.first + " -> " + exception.second +
                                   " no longer happens: remove it");
            }
        }
    }

    // ---- Paths ------------------------------------------------------------------------------------------------------

    bool hasCodeExtension(std::string_view token) {
        for (std::string_view extension : {".md", ".h", ".cpp", ".glsl", ".comp", ".vert", ".frag", ".ps1", ".py"}) {
            if (token.ends_with(extension)) {
                return true;
            }
        }

        return false;
    }

    /*
    * Whether a code span names a file or folder in the repo. Placeholders (<name>), wildcards, commands (spaces, flags),
    * Windows paths and bare extensions are not paths, and build outputs and fetched folders need not exist.
    */
    bool isRepoPath(std::string_view token) {
        if (token.empty() || token.find_first_of(" <>*{}\\:$=\"") != std::string_view::npos || token.starts_with('-')) {
            return false;
        }

        const bool hasFolder = token.find('/') != std::string_view::npos;

        if (!hasFolder && (token.starts_with('.') || !hasCodeExtension(token))) {
            return false;
        }

        for (const std::string& prefix : unversionedPrefixes) {
            if (token.starts_with(prefix)) {
                return false;
            }
        }

        return true;
    }

    // Every file's name in the folders docs name files from, for bare names like `test_main.cpp`.
    std::set<std::string> repoFileNames() {
        std::set<std::string> names;

        for (const fs::path& file : sourceTreeFiles()) {
            names.insert(file.filename().string());
        }

        for (const char* folder : {"tests", "tools", "docs", "assets/shaders", ".claude"}) {
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder)) {
                if (entry.is_regular_file()) {
                    names.insert(entry.path().filename().string());
                }
            }
        }

        for (const fs::directory_entry& entry : fs::directory_iterator(".")) {
            if (entry.is_regular_file()) {
                names.insert(entry.path().filename().string());
            }
        }

        return names;
    }

    // A path is looked for from the repo root, src/, external/ and the doc's own folder, with or without .h or .cpp
    // (docs name a module by its stem: render/gpu/cloth_gpu).
    bool pathExists(const std::string& token, const fs::path& doc) {
        for (const fs::path& base : {fs::path("."), fs::path("src"), fs::path("external"), doc.parent_path()}) {
            for (const char* suffix : {"", ".h", ".cpp"}) {
                if (fs::exists(base / (token + suffix))) {
                    return true;
                }
            }
        }

        return false;
    }

    void checkPaths(Problems& problems) {
        const std::set<std::string> names = repoFileNames();

        for (const fs::path& doc : checkedDocs()) {
            for (const std::string& line : proseLines(doc)) {
                for (const std::string& token : backticked(line)) {
                    if (!isRepoPath(token)) {
                        continue;
                    }

                    const bool found = token.find('/') == std::string::npos ? names.contains(token)
                                                                             : pathExists(token, doc);

                    if (!found) {
                        problems.push_back(slashed(doc) + " names `" + token + "`, which does not exist");
                    }
                }
            }
        }
    }

    // ---- The index --------------------------------------------------------------------------------------------------

    // Every doc is in docs/INDEX.md, except a long doc's parts, which are in their own index (partedDocs).
    void checkIndex(Problems& problems) {
        const std::string index = readText("docs/INDEX.md");

        for (const fs::path& doc : filesIn("docs", ".md")) {
            const std::string name = slashed(doc);
            std::string listing = "docs/INDEX.md";

            for (const auto& [folder, partIndex] : partedDocs) {
                if (name.starts_with(folder + "/")) {
                    listing = partIndex;
                }
            }

            const std::string listed = listing == "docs/INDEX.md" ? index : readText(listing);

            if (name != "docs/INDEX.md" && listed.find("`" + name + "`") == std::string::npos) {
                problems.push_back(name + " is missing from " + listing);
            }
        }
    }

    // ---- Coverage ---------------------------------------------------------------------------------------------------

    /*
    * Every source and shader is claimed by an architecture file's "Covers:" line (a path or a prefix of one), so each
    * has one place that says how it works, and tools/docs_reminder.ps1 knows which doc a change touches.
    */
    void checkCoverage(Problems& problems) {
        std::vector<std::string> prefixes;

        for (const fs::path& doc : filesIn("docs/architecture", ".md")) {
            for (const std::string& line : splitLines(readText(doc))) {
                if (line.starts_with("Covers:")) {
                    for (const std::string& span : backticked(line)) {
                        prefixes.push_back(span);
                    }

                    break;
                }
            }
        }

        std::vector<std::string> claimable;

        for (const fs::path& file : sourceTreeFiles()) {
            if (file.extension() == ".h" || file.extension() == ".cpp") {
                claimable.push_back(slashed(file));
            }
        }

        for (const std::string_view extension : {".glsl", ".vert", ".frag", ".comp"}) {
            for (const fs::path& shader : filesIn("assets/shaders", extension)) {
                claimable.push_back(slashed(shader));
            }
        }

        for (const std::string& file : claimable) {
            const bool covered = std::ranges::any_of(prefixes, [&](const std::string& prefix) {
                return file.starts_with(prefix);
            });

            if (!covered) {
                problems.push_back(file + " is in no docs/architecture file's Covers: line");
            }
        }
    }

    // ---- Repeated text ----------------------------------------------------------------------------------------------

    // A doc's prose as lowercase words: letters and digits, everything else a separator.
    std::vector<std::string> docWords(const fs::path& doc) {
        std::vector<std::string> words;
        std::string word;

        for (const std::string& line : proseLines(doc)) {
            for (char c : line + "\n") {
                const unsigned char byte = static_cast<unsigned char>(c);

                if (std::isalnum(byte)) {
                    word.push_back(static_cast<char>(std::tolower(byte)));
                } else if (!word.empty()) {
                    words.push_back(word);
                    word.clear();
                }
            }
        }

        return words;
    }

    std::uint64_t runHash(const std::vector<std::string>& words, std::size_t first) {
        Fnv1aHash hash;

        for (std::size_t i = first; i < first + duplicateRunWords; ++i) {
            hash.addBytes(words[i].data(), words[i].size());
            hash.add(' ');
        }

        return hash.value;
    }

    /*
    * The same fact written in two places, found by shingling (Broder 1997, how search engines spot copied pages): every
    * run of twelve consecutive words is hashed, and a run already seen in another doc is a repeat. Reported once per
    * pair of docs, with the first run they share.
    */
    void checkRepeats(Problems& problems) {
        std::unordered_map<std::uint64_t, std::string> firstDocOfRun;
        std::set<std::pair<std::string, std::string>> pairsReported;

        for (const fs::path& path : checkedDocs()) {
            const std::string doc = slashed(path);
            const std::vector<std::string> words = docWords(path);

            for (std::size_t first = 0; first + duplicateRunWords <= words.size(); ++first) {
                const auto [seen, isNew] = firstDocOfRun.try_emplace(runHash(words, first), doc);

                if (isNew || seen->second == doc || !pairsReported.emplace(seen->second, doc).second) {
                    continue;
                }

                std::string run;

                for (std::size_t i = first; i < first + duplicateRunWords; ++i) {
                    run += (i == first ? "" : " ") + words[i];
                }

                problems.push_back(seen->second + " and " + doc + " repeat \"" + run + "\": keep one, link to it");
            }
        }
    }
}

/*
* Keeps the docs from growing needlessly (ROADMAP phase 19): the layers docs/LAYOUT.md states hold for every include,
* instruction files stay within their budgets and line length, the paths the checked docs name exist, every doc is in
* docs/INDEX.md, every source and shader has an architecture file, and no two checked docs repeat the same text.
*/
void testDocs() {
    Problems problems;
    checkLayers(problems);
    checkBudgets(problems);
    checkLineLengths(problems);
    checkPaths(problems);
    checkIndex(problems);
    checkCoverage(problems);
    checkRepeats(problems);

    for (const std::string& problem : problems) {
        std::cerr << "docs: " << problem << "\n";
    }

    assert(problems.empty() && "the docs check found problems (listed above)");
}
