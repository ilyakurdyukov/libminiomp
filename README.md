# MiniOMP runtime library

Minimal implementation of the OpenMP runtime library, lightweight replacement of `libgomp` for *Mingw-w64*.  
Good to use in libraries and small applications.

The original project page is [here](https://github.com/ilyakurdyukov/libminiomp).

## Advantages

- Small size with static linking  
It's ~150k less without linking with `libpthread` and `libgomp` in *Mingw-w64*.

- Supports Windows XP  
The latest version of *Mingw-w64* is not, because of `libpthread` dependency, which needs *GetTickCount64*.

- Portable  
Although written to work with *Mingw-w64*, it also works on Linux using `pthreads`. Can be ported to different platforms.

- *CLANG* support  
You can link object files from GCC and CLANG together with the same library.

## OpenMP features

### Supported

- *GCC* compiler (`GOMP_*` interface)
- *CLANG* compiler (`__kmpc_*` interface)
- `parallel`
- `for`
- `num_threads`
- `schedule(static)` for *CLANG*
- `schedule(dynamic)`
- `chunk-size`
- loop with index decrement
- 32/64-bit loop index
- `critical` (unnamed only)
- `barrier`
- `single`
- `sections` for *GCC*
- features implemented by the compiler (inlined in object code)  
`schedule(static)` implemented by *GCC*  
`sections` implemented by *CLANG*

### Unsupported

- `for nowait`
- `for ordered`
- `schedule(runtime)`
- named `critical`

And other rarely used features.

### Limitations

- `guided` work same as `dynamic`

- the maximum number of threads is limited at compile time  
(`MAX_THREADS` define)

- always `monotonic`

- limited overflow checking for the loop indexes  
Can overflow with extreme `chunk-size` and increment values.

- you can run only one parallel block at a time  
Use locks if this situation can occur. This implementation uses static resources that are shared by all threads in the process. But for static linking, it is independent from other static linkages and different OpenMP runtime implementations.

