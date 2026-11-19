#pragma once

#include <chrono>
#include <functional>

/*
* Run work on a thread of its own while this thread calls waiting() at once and then every interval until work
* finishes: a long load (a game building its world) behind a loading screen that keeps drawing and a window that keeps
* answering, as shipped engines load behind theirs. What work throws is thrown here once it ends; what waiting throws
* is thrown once work has finished too, since its thread is never left running. Nothing else may touch what work
* changes meanwhile.
*/
void runWhileWaiting(const std::function<void()>& work, const std::function<void()>& waiting, std::chrono::milliseconds interval);
