// Console entry point for the standalone (non-GUI) sdr2hdr build.
//
// All real logic lives in sdr2hdr::cliMain (src/main.cpp). The merged GUI
// binary forwards to that same function from wWinMain, so the conversion
// behaviour is identical whether the user runs the console build or passes
// command-line arguments to the GUI build.

#include "engine.h"

int main(int argc, char** argv)
{
    return sdr2hdr::cliMain(argc, argv);
}
