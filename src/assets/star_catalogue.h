#pragma once

#include "scene/stars.h"

#include <istream>
#include <string>
#include <vector>

// The light lab's star catalogue: the Yale Bright Star Catalogue's 9,096 stars down to magnitude 6.5, trimmed
// (assets/stars/README.md).
inline constexpr const char* brightStarCataloguePath = "assets/stars/bright_stars.csv";

// The colour index a star is given when the catalogue lists none (a few hundred faint ones): the sun's.
inline constexpr float unknownColourIndex = 0.65f;

/*
* Read a star catalogue: one star a line, "number,right ascension,declination,magnitude,colour index", the angles in
* degrees (J2000), the colour index B - V blank when unknown; lines starting with # are comments. Throws on a line it
* cannot read, naming it.
*/
std::vector<CatalogueStar> parseStarCatalogue(std::istream& input);

// The same from a file; throws if it cannot be opened.
std::vector<CatalogueStar> loadStarCatalogue(const std::string& path);
