# tenc32 CPU
This is the reference implementation for the tenc32 specification.
The implementation is provided in the form of a library, it is expected for a consumer to write their
own "framework" around the CPU to get it to load. Bootup constants are found in `crow.crowcpu_arch`.
See `crow.crowcpu_runner` for a simple framework that loads a rom into memory and executes it at startup.
