# This file is a part of Julia. License is MIT: https://julialang.org/license

module Libdl
@doc """
Interface to libdl. Provides dynamic linking support.
""" Libdl

import Base: DL_LOAD_PATH, isdebugbuild

export DL_LOAD_PATH, RTLD_DEEPBIND, RTLD_FIRST, RTLD_GLOBAL, RTLD_LAZY, RTLD_LOCAL,
    RTLD_NODELETE, RTLD_NOLOAD, RTLD_NOW, dlclose, dlopen, dlopen_e, dlsym, dlsym_e,
    dlpath, find_library, dlext, dllist, LazyLibrary, LazyLibraryPath, BundledLazyLibraryPath

"""
    DL_LOAD_PATH

When calling [`dlopen`](@ref), the paths in this list will be searched first, in
order, before searching the system locations for a valid library handle.
"""
DL_LOAD_PATH

# note: constants to match JL_RTLD_* in src/julia.h, translated
#       to system-specific values by JL_RTLD macro in src/dlload.c
const RTLD_LOCAL     = 0x00000001
const RTLD_GLOBAL    = 0x00000002
const RTLD_LAZY      = 0x00000004
const RTLD_NOW       = 0x00000008
const RTLD_NODELETE  = 0x00000010
const RTLD_NOLOAD    = 0x00000020
const RTLD_DEEPBIND  = 0x00000040
const RTLD_FIRST     = 0x00000080

"""
    RTLD_DEEPBIND
    RTLD_FIRST
    RTLD_GLOBAL
    RTLD_LAZY
    RTLD_LOCAL
    RTLD_NODELETE
    RTLD_NOLOAD
    RTLD_NOW

Enum constant for [`dlopen`](@ref). See your platform man page for details, if
applicable.
"""
(RTLD_DEEPBIND, RTLD_FIRST, RTLD_GLOBAL, RTLD_LAZY, RTLD_LOCAL, RTLD_NODELETE, RTLD_NOLOAD, RTLD_NOW)

# The default flags for `dlopen()`
const default_rtld_flags = RTLD_LAZY | RTLD_DEEPBIND

"""
    dlsym(handle, sym; throw_error::Bool = true)

Look up a symbol from a shared library handle, return callable function pointer on success.

If the symbol cannot be found, this method throws an error, unless the keyword argument
`throw_error` is set to `false`, in which case this method returns `nothing`.
"""
function dlsym(hnd::Ptr, s::Union{Symbol,AbstractString}; throw_error::Bool = true)
    hnd == C_NULL && throw(ArgumentError("NULL library handle"))
    val = Ref(Ptr{Cvoid}(0))
    symbol_found = ccall(:jl_dlsym, Cint,
        (Ptr{Cvoid}, Cstring, Ref{Ptr{Cvoid}}, Cint),
        hnd, s, val, Int64(throw_error)
    )
    if symbol_found == 0
        return nothing
    end
    return val[]
end

"""
    dlsym_e(handle, sym)

Look up a symbol from a shared library handle, silently return `C_NULL` on lookup failure.
This method is now deprecated in favor of `dlsym(handle, sym; throw_error=false)`.
"""
function dlsym_e(args...)
    return something(dlsym(args...; throw_error=false), C_NULL)
end

"""
    dlopen(libfile::AbstractString [, flags::Integer]; throw_error:Bool = true)

Load a shared library, returning an opaque handle.

The extension given by the constant `dlext` (`.so`, `.dll`, or `.dylib`)
can be omitted from the `libfile` string, as it is automatically appended
if needed.   If `libfile` is not an absolute path name, then the paths
in the array `DL_LOAD_PATH` are searched for `libfile`, followed by the
system load path.

The optional flags argument is a bitwise-or of zero or more of `RTLD_LOCAL`, `RTLD_GLOBAL`,
`RTLD_LAZY`, `RTLD_NOW`, `RTLD_NODELETE`, `RTLD_NOLOAD`, `RTLD_DEEPBIND`, and `RTLD_FIRST`.
These are converted to the corresponding flags of the POSIX (and/or GNU libc and/or MacOS)
dlopen command, if possible, or are ignored if the specified functionality is not available
on the current platform. The default flags are platform specific. On MacOS the default
`dlopen` flags are `RTLD_LAZY|RTLD_DEEPBIND|RTLD_GLOBAL` while on other platforms the
defaults are `RTLD_LAZY|RTLD_DEEPBIND|RTLD_LOCAL`. An important usage of these flags is to
specify non default behavior for when the dynamic library loader binds library references to
exported symbols and if the bound references are put into process local or global scope. For
instance `RTLD_LAZY|RTLD_DEEPBIND|RTLD_GLOBAL` allows the library's symbols to be available
for usage in other shared libraries, addressing situations where there are dependencies
between shared libraries.

If the library cannot be found, this method throws an error, unless the keyword argument
`throw_error` is set to `false`, in which case this method returns `nothing`.

!!! note
     From Julia 1.6 on, this method replaces paths starting with `@executable_path/` with
     the path to the Julia executable, allowing for relocatable relative-path loads. In
     Julia 1.5 and earlier, this only worked on macOS.
"""
function dlopen end

dlopen(s::Symbol, flags::Integer = default_rtld_flags; kwargs...) =
    dlopen(string(s), flags; kwargs...)

function dlopen(s::AbstractString, flags::Integer = default_rtld_flags; throw_error::Bool = true)
    ret = ccall(:jl_load_dynamic_library, Ptr{Cvoid}, (Cstring,UInt32,Cint), s, flags, Cint(throw_error))
    if !throw_error && ret == C_NULL
        return nothing
    end
    return ret
end

"""
    dlopen(f::Function, args...; kwargs...)

Wrapper for usage with `do` blocks to automatically close the dynamic library once
control flow leaves the `do` block scope.

# Examples
```julia
vendor = dlopen("libblas") do lib
    if Libdl.dlsym(lib, :openblas_set_num_threads; throw_error=false) !== nothing
        return :openblas
    else
        return :other
    end
end
```
"""
function dlopen(f::Function, name, args...; kwargs...)
    hdl = nothing
    try
        hdl = dlopen(name, args...; kwargs...)
        f(hdl)
    finally
        dlclose(hdl)
    end
end

"""
    dlopen_e(libfile::AbstractString [, flags::Integer])

Similar to [`dlopen`](@ref), except returns `C_NULL` instead of raising errors.
This method is now deprecated in favor of `dlopen(libfile::AbstractString [, flags::Integer]; throw_error=false)`.
"""
dlopen_e(args...) = something(dlopen(args...; throw_error=false), C_NULL)

"""
    dlclose(handle)

Close shared library referenced by handle.
"""
function dlclose(p::Ptr)
    0 == ccall(:jl_dlclose, Cint, (Ptr{Cvoid},), p)
end

"""
    dlclose(::Nothing)

For the very common pattern usage pattern of

    try
        hdl = dlopen(library_name)
        ... do something
    finally
        dlclose(hdl)
    end

We define a `dlclose()` method that accepts a parameter of type `Nothing`, so
that user code does not have to change its behavior for the case that `library_name`
was not found.
"""
function dlclose(p::Nothing)
end

"""
    find_library(names [, locations])

Searches for the first library in `names` in the paths in the `locations` list,
`DL_LOAD_PATH`, or system library paths (in that order) which can successfully be dlopen'd.
On success, the return value will be one of the names (potentially prefixed by one of the
paths in locations). This string can be assigned to a `global const` and used as the library
name in future `ccall`'s. On failure, it returns the empty string.
"""
function find_library(libnames, extrapaths=String[])
    for lib in libnames
        for path in extrapaths
            l = joinpath(path, lib)
            p = dlopen(l, RTLD_LAZY; throw_error=false)
            if p !== nothing
                dlclose(p)
                return l
            end
        end
        p = dlopen(lib, RTLD_LAZY; throw_error=false)
        if p !== nothing
            dlclose(p)
            return lib
        end
    end
    return ""
end
find_library(libname::Union{Symbol,AbstractString}, extrapaths=String[]) =
    find_library([string(libname)], extrapaths)

"""
    dlpath(handle::Ptr{Cvoid})

Given a library `handle` from `dlopen`, return the full path.
"""
function dlpath(handle::Ptr{Cvoid})
    p = ccall(:jl_pathname_for_handle, Cstring, (Ptr{Cvoid},), handle)
    s = unsafe_string(p)
    Sys.iswindows() && Libc.free(p)
    return s
end

"""
    dlpath(libname::Union{AbstractString, Symbol})

Get the full path of the library `libname`.

# Examples
```julia-repl
julia> dlpath("libjulia")
```
"""
function dlpath(libname::Union{AbstractString, Symbol})
    handle = dlopen(libname)
    path = dlpath(handle)
    dlclose(handle)
    return path
end

if Sys.isapple()
    const dlext = "dylib"
elseif Sys.iswindows()
    const dlext = "dll"
else
    #assume Sys.islinux, or similar
    const dlext = "so"
end

"""
    dlext

File extension for dynamic libraries (e.g. dll, dylib, so) on the current platform.
"""
dlext

if (Sys.islinux() || Sys.isbsd()) && !Sys.isapple()
    struct dl_phdr_info
        # Base address of object
        addr::Cuint

        # Null-terminated name of object
        name::Ptr{UInt8}

        # Pointer to array of ELF program headers for this object
        phdr::Ptr{Cvoid}

        # Number of program headers for this object
        phnum::Cshort
    end

    # This callback function called by dl_iterate_phdr() on Linux and BSD's
    # DL_ITERATE_PHDR(3) on freebsd
    function dl_phdr_info_callback(di::dl_phdr_info, size::Csize_t, dynamic_libraries::Array{String,1})
        name = unsafe_string(di.name)
        push!(dynamic_libraries, name)
        return Cint(0)
    end
end

"""
    dllist()

Return the paths of dynamic libraries currently loaded in a `Vector{String}`.
"""
function dllist()
    dynamic_libraries = Vector{String}()

    @static if Sys.isapple()
        numImages = ccall(:_dyld_image_count, Cint, ())

        # start at 1 instead of 0 to skip self
        for i in 1:numImages-1
            name = unsafe_string(ccall(:_dyld_get_image_name, Cstring, (UInt32,), i))
            push!(dynamic_libraries, name)
        end
    elseif Sys.islinux() || Sys.isbsd()
        callback = @cfunction(dl_phdr_info_callback, Cint,
                              (Ref{dl_phdr_info}, Csize_t, Ref{Vector{String}}))
        ccall(:dl_iterate_phdr, Cint, (Ptr{Cvoid}, Ref{Vector{String}}), callback, dynamic_libraries)
        popfirst!(dynamic_libraries)
        filter!(!isempty, dynamic_libraries)
    elseif Sys.iswindows()
        ccall(:jl_dllist, Cint, (Any,), dynamic_libraries)
    else
        # unimplemented
    end

    return dynamic_libraries
end


"""
    LazyLibraryPath

Helper type for lazily constructed library paths for use with `LazyLibrary`.
Arguments are passed to `joinpath()`.  Arguments must be able to have
`string()` called on them.

```
libfoo = LazyLibrary(LazyLibraryPath(prefix, "lib/libfoo.so.1.2.3"))
```
"""
struct LazyLibraryPath
    pieces::Tuple{Vararg{Any}}
    LazyLibraryPath(pieces...) = new(pieces)
end
@inline Base.string(llp::LazyLibraryPath) = joinpath(String[string(p) for p in llp.pieces])
Base.cconvert(::Type{Cstring}, llp::LazyLibraryPath) = Base.cconvert(Cstring, string(llp))
# Define `print` so that we can wrap this in a `LazyString`
Base.print(io::IO, llp::LazyLibraryPath) = print(io, string(llp))

# Helper to get `$(private_shlibdir)` at runtime
struct PrivateShlibdirGetter; end
const private_shlibdir = Base.OncePerProcess{String}() do
    libname = ifelse(isdebugbuild(), "libjulia-internal-debug", "libjulia-internal")
    dirname(dlpath(libname))
end
Base.string(::PrivateShlibdirGetter) = private_shlibdir()

"""
    BundledLazyLibraryPath

Helper type for lazily constructed library paths that are stored within the
bundled Julia distribution, primarily for use by Base modules.

```
libfoo = LazyLibrary(BundledLazyLibraryPath("libfoo.so.1.2.3"))
```
"""
BundledLazyLibraryPath(subpath) = LazyLibraryPath(PrivateShlibdirGetter(), subpath)


"""
    LazyLibrary(name, flags = <default dlopen flags>,
                dependencies = LazyLibrary[], on_load_callback = nothing)

Represents a lazily-loaded library that opens itself and its dependencies on first usage
in a `dlopen()`, `dlsym()`, or `ccall()` usage.  While this structure contains the
ability to run arbitrary code on first load via `on_load_callback`, we caution that this
should be used sparingly, as it is not expected that `ccall()` should result in large
amounts of Julia code being run.  You may call `ccall()` from within the
`on_load_callback` but only for the current library and its dependencies, and user should
not call `wait()` on any tasks within the on load callback.
"""
mutable struct LazyLibrary
    # Name and flags to open with
    const path
    const flags::UInt32

    # Dependencies that must be loaded before we can load
    dependencies::Vector{LazyLibrary}

    # Function that get called once upon initial load
    on_load_callback
    const lock::Base.ReentrantLock

    # Pointer that we eventually fill out upon first `dlopen()`
    @atomic handle::Ptr{Cvoid}
    function LazyLibrary(path; flags = default_rtld_flags, dependencies = LazyLibrary[],
                         on_load_callback = nothing)
        return new(
            path,
            UInt32(flags),
            collect(dependencies),
            on_load_callback,
            Base.ReentrantLock(),
            C_NULL,
        )
    end
end

# We support adding dependencies only because of very special situations
# such as LBT needing to have OpenBLAS_jll added as a dependency dynamically.
function add_dependency!(ll::LazyLibrary, dep::LazyLibrary)
    @lock ll.lock begin
        push!(ll.dependencies, dep)
    end
end

# Register `jl_libdl_dlopen_func` so that `ccall()` lowering knows
# how to call `dlopen()`, during bootstrap.
# See  `post_image_load_hooks` for non-bootstrapping.
Base.unsafe_store!(cglobal(:jl_libdl_dlopen_func, Any), dlopen)

function dlopen(ll::LazyLibrary, flags::Integer = ll.flags; kwargs...)
    handle = @atomic :acquire ll.handle
    if handle == C_NULL
        @lock ll.lock begin
            # Check to see if another thread has already run this
            if ll.handle == C_NULL
                # Ensure that all dependencies are loaded
                for dep in ll.dependencies
                    dlopen(dep; kwargs...)
                end

                # Load our library
                handle = dlopen(string(ll.path), flags; kwargs...)
                @atomic :release ll.handle = handle

                # Only the thread that loaded the library calls the `on_load_callback()`.
                if ll.on_load_callback !== nothing
                    ll.on_load_callback()
                end
            end
        end
    else
        # Invoke our on load callback, if it exists
        if ll.on_load_callback !== nothing
            # This empty lock protects against the case where we have updated
            # `ll.handle` in the branch above, but not exited the lock.  We want
            # a second thread that comes in at just the wrong time to have to wait
            # for that lock to be released (and thus for the on_load_callback to
            # have finished), hence the empty lock here. But we want the
            # on_load_callback thread to bypass this, which will be happen thanks
            # to the fact that we're using a reentrant lock here.
            @lock ll.lock begin end
        end
    end

    return handle
end
dlopen(x::Any) = throw(TypeError(:dlopen, "", Union{Symbol,String,LazyLibrary}, x))
dlsym(ll::LazyLibrary, args...; kwargs...) = dlsym(dlopen(ll), args...; kwargs...)
dlpath(ll::LazyLibrary) = dlpath(dlopen(ll))
end # module Libdl
