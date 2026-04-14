# SimpleLibrary

Minimal C++ library scaffold using CMake. It can build either a static library or a shared library on macOS, Windows, and Linux.

## Layout

- `include/simplelib/`: public headers
- `src/`: library implementation
- `examples/`: small consumer executable
- `tests/`: Catch2 unit tests

## Build a Static Library

```bash
cmake -S . -B build/static -DBUILD_SHARED_LIBS=OFF
cmake --build build/static
```

## Build a Shared Library

```bash
cmake -S . -B build/shared -DBUILD_SHARED_LIBS=ON
cmake --build build/shared
```

## Build and Run Tests

```bash
cmake -S . -B build/test -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build build/test --target tests
ctest --test-dir build/test --output-on-failure
```

## Notes

- The same `CMakeLists.txt` works across macOS, Windows, and Linux.
- `BUILD_SHARED_LIBS` controls whether `simplelib` is created as static or shared.
- The export macro in `include/simplelib/export.hpp` handles symbol visibility for shared builds.
- `BUILD_TESTING=ON` enables Catch2-based unit tests and creates the `simplelib_tests` and `tests` build targets.
