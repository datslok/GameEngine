#pragma once

#include <chrono>
#include <cstddef>
#include <functional>

/*
* Run work on a thread of its own while this thread calls waiting() at once and then every interval until work
* finishes: a long load (a game building its world) behind a loading screen that keeps drawing and a window that keeps
* answering, as shipped engines load behind theirs. What work throws is thrown here once it ends; what waiting throws
* is thrown once work has finished too, since its thread is never left running. Nothing else may touch what work
* changes meanwhile.
*/
void runWhileWaiting(const std::function<void()>& work, const std::function<void()>& waiting, std::chrono::milliseconds interval);

/*
* Work that must stay on this thread spread over loading frames (meshes uploaded through the GPU queue the loading
* screen draws with): step(done) does the next one or several (a batch of meshes) and returns how many, at least one;
* steps until budgetSeconds have passed since the first (clock read after each step), at least one while any are left,
* the count never past total. Returns how many are done.
*/
std::size_t stepsWithinBudget(std::size_t done, std::size_t total, double budgetSeconds, const std::function<double()>& clock,
                              const std::function<std::size_t(std::size_t)>& step);
