#include "application.h"
#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>

/*
* Starts the application and handles any exceptions so runtime errors are reported cleanly before the program exits.
*/
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    try {
        Application application{1920, 1080};
        application.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}