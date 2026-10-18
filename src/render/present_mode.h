#pragma once

/*
* How finished frames reach the screen.
* Vsync: each frame waits its turn for a screen refresh. No tearing, but queued frames add latency. Always supported.
* Mailbox: no tearing, and a newer frame replaces one still waiting, so the screen shows the freshest frame. Low latency.
* Immediate: frames are shown as soon as they are ready. Lowest latency, but the image can tear.
*/
enum class PresentMode {
    Vsync,
    Mailbox,
    Immediate
};
