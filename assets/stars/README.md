# Stars

`bright_stars.csv` is the Yale Bright Star Catalogue, 5th revised edition (preliminary version): Hoffleit, D. and Warren Jr., W. H., Astronomical Data Center, NSSDC/ADC (1991), as distributed by the CDS (catalogue V/50, https://cdsarc.cds.unistra.fr/viz-bin/cat/V/50). It lists the 9,110 objects brighter than about magnitude 6.5; the 14 without positions (novae and other objects kept only for their numbers) are left out.

Trimmed to what the engine draws, one star a line: the Harvard Revised number, the J2000 right ascension and declination in degrees, the visual magnitude, and the B-V colour index (blank where the catalogue gives none; the loader then uses the sun's, 0.65). The positions are for the year 2000: precession has moved them about a third of a degree since, which the engine leaves out.

The engine reads it with `loadStarCatalogue` (`src/assets/star_catalogue.h`).
