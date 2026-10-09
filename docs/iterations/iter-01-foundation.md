# Iteration 1: Foundation & Toolchain

## Objective

Establish a rock-solid build system, containerized development environment, and project structure before writing any proxy logic.

## Key Changes & Why They Matter

### 1. Multi-stage Dockerfile

We've split the `Dockerfile` into three stages:

* **`dev`**: Contains all build tools (`cmake`, `g++`, `git`, `netcat`, `curl`) and the `wrk` benchmarking tool. It explicitly *does not* copy our source code into the image. Instead, `docker-compose` volume-mounts our local code. The default command is `sleep infinity` to keep the container running.
* **`build`**: Takes the `dev` stage, copies the source code, and performs a release build.
* **`prod`**: A clean, minimal image that only contains the compiled binaries (`loadbalancer` and `dummy_backend`), leaving behind compilers and tools.

*Why:* In dev, we want to edit code on our host and compile inside the container without rebuilding the image every time. In production (or benchmarking), we want a lean, immutable image.

### 2. Hardened CMakeLists.txt

We added strict compiler flags: `-Wall -Wextra -Werror -Wpedantic`.
*Why:* C++ gives you enough rope to hang yourself. Treating all warnings as errors (`-Werror`) forces us to write clean code from day one and catches uninitialized variables or implicit conversions before they become runtime bugs.

### 3. dummy_backend.cpp

We replaced the empty stub with a real, working TCP echo server using bare POSIX sockets (`socket`, `bind`, `listen`, `accept`). It uses `SO_REUSEADDR` to ensure we can restart it rapidly without hitting "Address already in use" errors.

### 4. Common Headers (`common/backend.h` & `common/logger.h`)

* **`backend.h`**: Defines the `Backend` struct. We had to explicitly delete the copy constructor because it contains a `std::atomic<int>` (which is not copyable). This forces us to move or pass backends by reference.
* **`logger.h`**: Provides `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` macros that print to `stderr` with timestamps. Writing to `stderr` is unbuffered by default in many environments, ensuring we don't lose logs if the process crashes.

## How to Verify This Iteration

Run the commands provided to build the environment, launch the dummy backends, and verify you can connect to them!
