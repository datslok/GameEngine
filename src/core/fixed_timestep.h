#pragma once

/*
* Turns variable real frame times into a whole number of fixed simulation ticks.
* Leftover time is kept in an accumulator and carried into the next frame, and the fraction of a tick it represents is used to interpolate rendering between the last two ticks.
*/
class FixedTimestep {
public:
    FixedTimestep(double ticksPerSecond, double maxFrameSeconds);

    // Add one frame of real time and return how many ticks to simulate.
    int advance(double frameSeconds);

    double getTickSeconds() const;

    // The last frame time passed to advance(), after clamping.
    double getFrameSeconds() const;

    // How far the accumulator is between the previous and the next tick, 0..1.
    float getAlpha() const;

private:
    double tickSeconds;
    double maxFrameSeconds;
    double accumulator = 0.0;
    double frameSeconds = 0.0;
};
