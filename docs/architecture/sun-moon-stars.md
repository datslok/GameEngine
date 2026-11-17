# Sun, moon and stars

Covers: `src/scene/sun`, `src/scene/celestial`, `src/scene/moon`, `src/scene/stars`, `src/scene/sky_object`, `src/assets/star_catalogue`, `assets/stars/`

Where the sky's bodies stand for a place and a time, as plain maths a game calls; the light lab and the forest use it. Drawing them (the sky, the moon's phased disc, the star splats): `sky.md`. Tests: `sun_test`, `celestial_test`, `stars_test`.

Frame: +x east, +y up, -z north, as everywhere.

## The sun (`scene/sun`)

Local solar time; no equation of time, no refraction.

| Function | What |
|---|---|
| `solarDeclination(day)` | -`earthAxialTiltDegrees` (23.44) times the cosine of the year's phase counted from the December solstice (day 355), so it is exact there |
| `solarHourAngle(hours)` | 15 degrees an hour from solar noon |
| `directionTowardsSun(latitude, declination, hourAngle)` | east = -cos δ sin H; north = sin δ cos φ - cos δ cos H sin φ; up = sin δ sin φ + cos δ cos H cos φ |
| `directionTowardsSunOn(latitudeDegrees, day, hours)` | the above from degrees and a date |
| `sunElevation(towardsSun)` | |
| `advanceSolarHours(hours, hoursPerSecond, seconds)` | a game's clock, wrapping at `hoursPerDay`; shared by the labs and the forest |

## Time and frames (`scene/celestial`)

- `daysSinceJ2000(year, dayOfYear, hours)`: Gregorian leap days counted; the game's solar time stands in for Universal Time.
- `eclipticToEquatorial(longitude, latitude)`, `equatorialDirection(rightAscension, declination)`: x the March equinox, z the celestial pole.
- `sunEclipticLongitude(days)`: the Astronomical Almanac's low-precision formula.
- `siderealAngle(days, solarHours)`: the sun's hour angle plus its right ascension, so the stars come round about 4 minutes earlier each day.
- `equatorialToWorld(latitude, siderealAngle)`: rows east (-s, c, 0), up (c cos φ, s cos φ, sin φ), south (c sin φ, s sin φ, -cos φ), with c, s the sidereal angle's cosine and sine. A body lands where `directionTowardsSun(latitude, declination, siderealAngle - rightAscension)` puts it.

## The moon (`scene/moon`)

- `moonEclipticPosition(days)`: Meeus chapter 47's mean elements and largest terms (about 0.3 degrees), in doubles.
- `moonPhaseAngle(towardsSun, towardsMoon)`: pi minus the angle between them.
- `moonPhaseBrightness(phaseAngle)`: Allen's curve, 0.026|a| + 4e-9 a^4 magnitudes fainter than full (a in degrees).
- `moonAngularRadius`: 0.00452 rad.

## The stars (`scene/stars`, `assets/star_catalogue`)

- Catalogue: `assets/stars/bright_stars.csv`, the Yale Bright Star Catalogue's 9,096 stars trimmed (`assets/stars/README.md`).
- `parseStarCatalogue` / `loadStarCatalogue` → `CatalogueStar`s; an unknown B - V reads as the sun's 0.65.
- `makeStar` → `Star`: equatorial direction, magnitude, colour `blackbodyColour(starTemperature(B - V))`.
  - `starTemperature`: Ballesteros's formula.
  - `blackbodyColour`: Planck's law against Wyman, Sloan and Shirley's fit of the CIE 1931 matching functions, in linear sRGB with a luminance of 1.
- `StarrySky`: the component a game places (the first in the world counts): the stars shared (the frame copies the component each frame), `toWorld` (the game sets it from `equatorialToWorld` each frame), `brightness` (magnitude 0's light value) and `contrast` (each magnitude gives 10^(-0.4 contrast m); 1 real, less squeezes the spread).

## Sky objects (`scene/sky_object`)

Discs drawn in the sky (the light lab's hand-made sun disc).

- `skyObjectPose(cameraPosition, towardsObject, distance, angularRadius)`: a fixed offset from the camera, scaled to its angular size, so it shows no parallax.
- `discSolidAngle(r)` = 4π sin²(r/2).
- `discRadiance(light, r)`: π times the light value over the solid angle (light values include Lambert's 1/π), scaled down as a whole colour to `maxSkyRadiance` (30,000, under the half-float limit; full sun exceeds it).
