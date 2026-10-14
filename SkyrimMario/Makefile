# Build in an MSYS2 "MinGW 32-bit" shell:  make
# Or cross-compile on Linux:               make CXX=i686-w64-mingw32-g++
CXX ?= g++
FLAGS = -O2 -std=c++17 -Wall -static -static-libgcc -static-libstdc++
all: bin/Data/SKSE/Plugins/SkyrimMario.dll bin/sm64_test.exe
bin/Data/SKSE/Plugins/SkyrimMario.dll: src/plugin.cpp src/runner.cpp src/overlay.cpp src/collision.cpp src/collision.h src/possess.cpp src/possess.h src/inputblock.cpp src/inputblock.h src/xinput.cpp src/xinput.h src/audio.cpp src/audio.h src/sm64_api.h
	mkdir -p bin/Data/SKSE/Plugins
	$(CXX) $(FLAGS) -shared -o $@ src/plugin.cpp src/runner.cpp src/overlay.cpp src/collision.cpp src/possess.cpp src/inputblock.cpp src/xinput.cpp src/audio.cpp -lwinmm
bin/sm64_test.exe: src/test_main.cpp src/runner.cpp src/sm64_api.h
	$(CXX) $(FLAGS) -o $@ src/test_main.cpp src/runner.cpp
clean:
	rm -rf bin
