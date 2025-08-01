# Profiling

The `Profile` module provides tools to help developers improve the performance of their
code. When used, it takes measurements on running code, and produces output that helps you understand
how much time is spent on individual line(s). The most common usage is to identify "bottlenecks"
as targets for optimization.

`Profile` implements what is known as a "sampling" or [statistical profiler](https://en.wikipedia.org/wiki/Profiling_(computer_programming)).
 It works by periodically taking a backtrace during the execution of any task. Each backtrace
captures the currently-running function and line number, plus the complete chain of function calls
that led to this line, and hence is a "snapshot" of the current state of execution.

If much of your run time is spent executing a particular line of code, this line will show up
frequently in the set of all backtraces. In other words, the "cost" of a given line--or really,
the cost of the sequence of function calls up to and including this line--is proportional to how
often it appears in the set of all backtraces.

A sampling profiler does not provide complete line-by-line coverage, because the backtraces occur
at intervals (by default, 1 ms on Unix systems and 10 ms on Windows, although the actual scheduling
is subject to operating system load). Moreover, as discussed further below, because samples are
collected at a sparse subset of all execution points, the data collected by a sampling profiler
is subject to statistical noise.

Despite these limitations, sampling profilers have substantial strengths:

  * You do not have to make any modifications to your code to take timing measurements.
  * It can profile into Julia's core code and even (optionally) into C and Fortran libraries.
  * By running "infrequently" there is very little performance overhead; while profiling, your code
    can run at nearly native speed.

For these reasons, it's recommended that you try using the built-in sampling profiler before considering
any alternatives.

## Basic usage

Let's work with a simple test case:

```julia-repl
julia> function myfunc()
           A = rand(200, 200, 400)
           maximum(A)
       end
```

It's a good idea to first run the code you intend to profile at least once (unless you want to
profile Julia's JIT-compiler):

```julia-repl
julia> myfunc() # run once to force compilation
```

Now we're ready to profile this function:

```julia-repl
julia> using Profile

julia> @profile myfunc()
```

To see the profiling results, there are several graphical browsers.
One "family" of visualizers is based on [FlameGraphs.jl](https://github.com/timholy/FlameGraphs.jl), with each family member providing a different user interface:
- [VS Code](https://www.julia-vscode.org/) is a full IDE with built-in support for profile visualization
- [ProfileView.jl](https://github.com/timholy/ProfileView.jl) is a stand-alone visualizer based on GTK
- [ProfileVega.jl](https://github.com/davidanthoff/ProfileVega.jl) uses VegaLight and integrates well with Jupyter notebooks
- [StatProfilerHTML.jl](https://github.com/tkluck/StatProfilerHTML.jl) produces HTML and presents some additional summaries, and also integrates well with Jupyter notebooks
- [ProfileSVG.jl](https://github.com/timholy/ProfileSVG.jl) renders SVG
- [PProf.jl](https://github.com/JuliaPerf/PProf.jl) serves a local website for inspecting graphs, flamegraphs and more
- [ProfileCanvas.jl](https://github.com/pfitzseb/ProfileCanvas.jl) is a HTML canvas based profile viewer UI, used by the [Julia VS Code extension](https://www.julia-vscode.org/), but can also generate interactive HTML files.

An entirely independent approach to profile visualization is [PProf.jl](https://github.com/vchuravy/PProf.jl), which uses the external `pprof` tool.

Here, though, we'll use the text-based display that comes with the standard library:

```julia-repl
julia> Profile.print()
80 ./event.jl:73; (::Base.REPL.##1#2{Base.REPL.REPLBackend})()
 80 ./REPL.jl:97; macro expansion
  80 ./REPL.jl:66; eval_user_input(::Any, ::Base.REPL.REPLBackend)
   80 ./boot.jl:235; eval(::Module, ::Any)
    80 ./<missing>:?; anonymous
     80 ./profile.jl:23; macro expansion
      52 ./REPL[1]:2; myfunc()
       38 ./random.jl:431; rand!(::MersenneTwister, ::Array{Float64,3}, ::Int64, ::Type{B...
        38 ./dSFMT.jl:84; dsfmt_fill_array_close_open!(::Base.dSFMT.DSFMT_state, ::Ptr{F...
       14 ./random.jl:278; rand
        14 ./random.jl:277; rand
         14 ./random.jl:366; rand
          14 ./random.jl:369; rand
      28 ./REPL[1]:3; myfunc()
       28 ./reduce.jl:270; _mapreduce(::Base.#identity, ::Base.#scalarmax, ::IndexLinear,...
        3  ./reduce.jl:426; mapreduce_impl(::Base.#identity, ::Base.#scalarmax, ::Array{F...
        25 ./reduce.jl:428; mapreduce_impl(::Base.#identity, ::Base.#scalarmax, ::Array{F...
```

Each line of this display represents a particular spot (line number) in the code. Indentation
is used to indicate the nested sequence of function calls, with more-indented lines being deeper
in the sequence of calls. In each line, the first "field" is the number of backtraces
(samples) taken *at this line or in any functions executed by this line*.
The second field is the file name and line number and the third field is the function name.
Note that the specific line numbers may change as Julia's
code changes; if you want to follow along, it's best to run this example yourself.

In this example, we can see that the top level function called is in the file `event.jl`. This is the
function that runs the REPL when you launch Julia. If you examine line 97 of `REPL.jl`,
you'll see this is where the function `eval_user_input()` is called. This is the function that evaluates
what you type at the REPL, and since we're working interactively these functions were invoked
when we entered `@profile myfunc()`. The next line reflects actions taken in the [`@profile`](@ref)
macro.

The first line shows that 80 backtraces were taken at line 73 of `event.jl`, but it's not that
this line was "expensive" on its own: the third line reveals that all 80 of these backtraces
were actually triggered inside its call to `eval_user_input`, and so on. To find out which operations
are actually taking the time, we need to look deeper in the call chain.

The first "important" line in this output is this one:

```
52 ./REPL[1]:2; myfunc()
```

`REPL` refers to the fact that we defined `myfunc` in the REPL, rather than putting it in a file;
if we had used a file, this would show the file name. The `[1]` shows that the function `myfunc`
was the first expression evaluated in this REPL session. Line 2 of `myfunc()` contains the call to
`rand`, and there were 52 (out of 80) backtraces that occurred at this line. Below that, you can
see a call to `dsfmt_fill_array_close_open!` inside `dSFMT.jl`.

A little further down, you see:

```
28 ./REPL[1]:3; myfunc()
```

Line 3 of `myfunc` contains the call to `maximum`, and there were 28 (out of 80) backtraces taken
here. Below that, you can see the specific places in `base/reduce.jl` that carry out the time-consuming
operations in the `maximum` function for this type of input data.

Overall, we can tentatively conclude that generating the random numbers is approximately twice as expensive
as finding the maximum element. We could increase our confidence in this result by
collecting more samples:

```julia-repl
julia> @profile (for i = 1:100; myfunc(); end)

julia> Profile.print()
[....]
 3821 ./REPL[1]:2; myfunc()
  3511 ./random.jl:431; rand!(::MersenneTwister, ::Array{Float64,3}, ::Int64, ::Type...
   3511 ./dSFMT.jl:84; dsfmt_fill_array_close_open!(::Base.dSFMT.DSFMT_state, ::Ptr...
  310  ./random.jl:278; rand
   [....]
 2893 ./REPL[1]:3; myfunc()
  2893 ./reduce.jl:270; _mapreduce(::Base.#identity, ::Base.#scalarmax, ::IndexLinea...
   [....]
```

In general, if you have `N` samples collected at a line, you can expect an uncertainty on the
order of `sqrt(N)` (barring other sources of noise, like how busy the computer is with other tasks).
The major exception to this rule is garbage collection, which runs infrequently but tends to be
quite expensive. (Since Julia's garbage collector is written in C, such events can be detected
using the `C=true` output mode described below, or by using [ProfileView.jl](https://github.com/timholy/ProfileView.jl).)

This illustrates the default "tree" dump; an alternative is the "flat" dump, which accumulates
counts independent of their nesting:

```julia-repl
julia> Profile.print(format=:flat)
 Count File          Line Function
  6714 ./<missing>     -1 anonymous
  6714 ./REPL.jl       66 eval_user_input(::Any, ::Base.REPL.REPLBackend)
  6714 ./REPL.jl       97 macro expansion
  3821 ./REPL[1]        2 myfunc()
  2893 ./REPL[1]        3 myfunc()
  6714 ./REPL[7]        1 macro expansion
  6714 ./boot.jl      235 eval(::Module, ::Any)
  3511 ./dSFMT.jl      84 dsfmt_fill_array_close_open!(::Base.dSFMT.DSFMT_s...
  6714 ./event.jl      73 (::Base.REPL.##1#2{Base.REPL.REPLBackend})()
  6714 ./profile.jl    23 macro expansion
  3511 ./random.jl    431 rand!(::MersenneTwister, ::Array{Float64,3}, ::In...
   310 ./random.jl    277 rand
   310 ./random.jl    278 rand
   310 ./random.jl    366 rand
   310 ./random.jl    369 rand
  2893 ./reduce.jl    270 _mapreduce(::Base.#identity, ::Base.#scalarmax, :...
     5 ./reduce.jl    420 mapreduce_impl(::Base.#identity, ::Base.#scalarma...
   253 ./reduce.jl    426 mapreduce_impl(::Base.#identity, ::Base.#scalarma...
  2592 ./reduce.jl    428 mapreduce_impl(::Base.#identity, ::Base.#scalarma...
    43 ./reduce.jl    429 mapreduce_impl(::Base.#identity, ::Base.#scalarma...
```

If your code has recursion, one potentially-confusing point is that a line in a "child" function
can accumulate more counts than there are total backtraces. Consider the following function definitions:

```julia
dumbsum(n::Integer) = n == 1 ? 1 : 1 + dumbsum(n-1)
dumbsum3() = dumbsum(3)
```

If you were to profile `dumbsum3`, and a backtrace was taken while it was executing `dumbsum(1)`,
the backtrace would look like this:

```julia
dumbsum3
    dumbsum(3)
        dumbsum(2)
            dumbsum(1)
```

Consequently, this child function gets 3 counts, even though the parent only gets one. The "tree"
representation makes this much clearer, and for this reason (among others) is probably the most
useful way to view the results.

## Accumulation and clearing

Results from [`@profile`](@ref) accumulate in a buffer; if you run multiple pieces of code under
[`@profile`](@ref), then [`Profile.print()`](@ref) will show you the combined results. This can
be very useful, but sometimes you want to start fresh; you can do so with [`Profile.clear()`](@ref).

## Options for controlling the display of profile results

[`Profile.print`](@ref) has more options than we've described so far. Let's see the full declaration:

```julia
function print(io::IO = stdout, data = fetch(); kwargs...)
```

Let's first discuss the two positional arguments, and later the keyword arguments:

  * `io` -- Allows you to save the results to a buffer, e.g. a file, but the default is to print to `stdout`
    (the console).
  * `data` -- Contains the data you want to analyze; by default that is obtained from [`Profile.fetch()`](@ref),
    which pulls out the backtraces from a pre-allocated buffer. For example, if you want to profile
    the profiler, you could say:

    ```julia
    data = copy(Profile.fetch())
    Profile.clear()
    @profile Profile.print(stdout, data) # Prints the previous results
    Profile.print()                      # Prints results from Profile.print()
    ```

The keyword arguments can be any combination of:

  * `format` -- Introduced above, determines whether backtraces are printed
     with (default, `:tree`) or without (`:flat`) indentation indicating tree
     structure.
  * `C` -- If `true`, backtraces from C and Fortran code are shown (normally they are excluded). Try running the introductory
    example with `Profile.print(C = true)`. This can be extremely helpful in deciding whether it's
    Julia code or C code that is causing a bottleneck; setting `C = true` also improves the interpretability
    of the nesting, at the cost of longer profile dumps.
  * `combine` -- Some lines of code contain multiple operations; for example, `s += A[i]` contains both an array
    reference (`A[i]`) and a sum operation. These correspond to different lines in the generated
    machine code, and hence there may be two or more different addresses captured during backtraces
    on this line. `combine = true` lumps them together, and is probably what you typically want, but
    you can generate an output separately for each unique instruction pointer with `combine = false`.
  * `maxdepth` -- Limits frames at a depth higher than `maxdepth` in the `:tree` format.
  * `sortedby` -- Controls the order in `:flat` format. `:filefuncline` (default) sorts by the source
    line, whereas `:count` sorts in order of number of collected samples.
  * `noisefloor` -- Limits frames that are below the heuristic noise floor of the sample (only applies to format `:tree`).
    A suggested value to try for this is 2.0 (the default is 0). This parameter hides samples for which `n <= noisefloor * √N`,
    where `n` is the number of samples on this line, and `N` is the number of samples for the callee.
  * `mincount` -- Limits frames with less than `mincount` occurrences.

File/function names are sometimes truncated (with `...`), and indentation is truncated with a
`+n` at the beginning, where `n` is the number of extra spaces that would have been inserted,
had there been room. If you want a complete profile of deeply-nested code, often a good idea is
to save to a file using a wide `displaysize` in an [`IOContext`](@ref):

```julia
open("/tmp/prof.txt", "w") do s
    Profile.print(IOContext(s, :displaysize => (24, 500)))
end
```

## Configuration

[`@profile`](@ref) just accumulates backtraces, and the analysis happens when you call [`Profile.print()`](@ref).
For a long-running computation, it's entirely possible that the pre-allocated buffer for storing
backtraces will be filled. If that happens, the backtraces stop but your computation continues.
As a consequence, you may miss some important profiling data (you will get a warning when that
happens).

You can obtain and configure the relevant parameters this way:

```julia
Profile.init() # returns the current settings
Profile.init(n = 10^7, delay = 0.01)
```

`n` is the total number of instruction pointers you can store, with a default value of `10^6`.
If your typical backtrace is 20 instruction pointers, then you can collect 50000 backtraces, which
suggests a statistical uncertainty of less than 1%. This may be good enough for most applications.

Consequently, you are more likely to need to modify `delay`, expressed in seconds, which sets
the amount of time that Julia gets between snapshots to perform the requested computations. A
very long-running job might not need frequent backtraces. The default setting is `delay = 0.001`.
Of course, you can decrease the delay as well as increase it; however, the overhead of profiling
grows once the delay becomes similar to the amount of time needed to take a backtrace (~30 microseconds
on the author's laptop).

## Wall-time Profiler

### Introduction & Problem Motivation

The profiler described in the previous section is a sampling CPU profiler. At a high level, the profiler periodically stops all Julia compute threads to collect their backtraces and estimates the time spent in each function based on the number of backtrace samples that include a frame from that function. However, note that only tasks currently running on system threads just before the profiler stops them will have their backtraces collected.

While this profiler is typically well-suited for workloads where the majority of tasks are compute-bound, it is less helpful for systems where most tasks are IO-heavy or for diagnosing contention on synchronization primitives in your code.

Let's consider this simple workload:

```Julia
using Base.Threads
using Profile
using PProf

ch = Channel(1)

const N_SPAWNED_TASKS = (1 << 10)
const WAIT_TIME_NS = 10_000_000

function spawn_a_bunch_of_tasks_waiting_on_channel()
    for i in 1:N_SPAWNED_TASKS
        Threads.@spawn begin
            take!(ch)
        end
    end
end

function busywait()
    t0 = time_ns()
    while true
        if time_ns() - t0 > WAIT_TIME_NS
            break
        end
    end
end

function main()
    spawn_a_bunch_of_tasks_waiting_on_channel()
    for i in 1:N_SPAWNED_TASKS
        put!(ch, i)
        busywait()
    end
end

Profile.@profile main()
```

Our goal is to detect whether there is contention on the `ch` channel—i.e., whether the number of waiters is excessive given the rate at which work items are being produced in the channel.

If we run this, we obtain the following [PProf](https://github.com/JuliaPerf/PProf.jl) flame graph:

![CPU Profile](./img/cpu-profile.png)

This profile provides no information to help determine where contention occurs in the system’s synchronization primitives. Waiters on a channel will be blocked and descheduled, meaning no system thread will be running the tasks assigned to those waiters, and as a result, they won't be sampled by the profiler.

### Wall-time Profiler

Instead of sampling threads—and thus only sampling tasks that are running—a wall-time task profiler samples tasks independently of their scheduling state. For example, tasks that are sleeping on a synchronization primitive at the time the profiler is running will be sampled with the same probability as tasks that were actively running when the profiler attempted to capture backtraces.

This approach allows us to construct a profile where backtraces from tasks blocked on the `ch` channel, as in the example above, are actually represented.

Let's run the same example, but now with a wall-time profiler:


```Julia
using Base.Threads
using Profile
using PProf

ch = Channel(1)

const N_SPAWNED_TASKS = (1 << 10)
const WAIT_TIME_NS = 10_000_000

function spawn_a_bunch_of_tasks_waiting_on_channel()
    for i in 1:N_SPAWNED_TASKS
        Threads.@spawn begin
            take!(ch)
        end
    end
end

function busywait()
    t0 = time_ns()
    while true
        if time_ns() - t0 > WAIT_TIME_NS
            break
        end
    end
end

function main()
    spawn_a_bunch_of_tasks_waiting_on_channel()
    for i in 1:N_SPAWNED_TASKS
        put!(ch, i)
        busywait()
    end
end

Profile.@profile_walltime main()
```

We obtain the following flame graph:

![Wall-time Profile Channel](./img/wall-time-profiler-channel-example.png)

We see that a large number of samples come from channel-related `take!` functions, which allows us to determine that there is indeed an excessive number of waiters in `ch`.

### A Compute-Bound Workload

Despite the wall-time profiler sampling all live tasks in the system and not just the currently running ones, it can still be helpful for identifying performance hotspots, even if your code is compute-bound. Let’s consider a simple example:

```Julia
using Base.Threads
using Profile
using PProf

ch = Channel(1)

const MAX_ITERS = (1 << 22)
const N_TASKS = (1 << 12)

function spawn_a_task_waiting_on_channel()
    Threads.@spawn begin
        take!(ch)
    end
end

function sum_of_sqrt()
    sum_of_sqrt = 0.0
    for i in 1:MAX_ITERS
        sum_of_sqrt += sqrt(i)
    end
    return sum_of_sqrt
end

function spawn_a_bunch_of_compute_heavy_tasks()
    Threads.@sync begin
        for i in 1:N_TASKS
            Threads.@spawn begin
                sum_of_sqrt()
            end
        end
    end
end

function main()
    spawn_a_task_waiting_on_channel()
    spawn_a_bunch_of_compute_heavy_tasks()
end

Profile.@profile_walltime main()
```

After collecting a wall-time profile, we get the following flame graph:

![Wall-time Profile Compute-Bound](./img/wall-time-profiler-compute-bound-example.png)

Notice how many of the samples contain `sum_of_sqrt`, which is the expensive compute function in our example.

### Identifying Task Sampling Failures in your Profile

In the current implementation, the wall-time profiler attempts to sample from tasks that have been alive since the last garbage collection, along with those created afterward. However, if most tasks are extremely short-lived, you may end up sampling tasks that have already completed, resulting in missed backtrace captures.

If you encounter samples containing `failed_to_sample_task_fun` or `failed_to_stop_thread_fun`, this likely indicates a high volume of short-lived tasks, which prevented their backtraces from being collected.

Let's consider this simple example:

```Julia
using Base.Threads
using Profile
using PProf

const N_SPAWNED_TASKS = (1 << 16)
const WAIT_TIME_NS = 100_000

function spawn_a_bunch_of_short_lived_tasks()
    for i in 1:N_SPAWNED_TASKS
        Threads.@spawn begin
            # Do nothing
        end
    end
end

function busywait()
    t0 = time_ns()
    while true
        if time_ns() - t0 > WAIT_TIME_NS
            break
        end
    end
end

function main()
    GC.enable(false)
    spawn_a_bunch_of_short_lived_tasks()
    for i in 1:N_SPAWNED_TASKS
        busywait()
    end
    GC.enable(true)
end

Profile.@profile_walltime main()
```

Notice that the tasks spawned in `spawn_a_bunch_of_short_lived_tasks` are extremely short-lived. Since these tasks constitute the majority in the system, we will likely miss capturing a backtrace for most sampled tasks.

After collecting a wall-time profile, we obtain the following flame graph:

![Task Sampling Failure](./img/task-sampling-failure.png)

The large number of samples from `failed_to_stop_thread_fun` confirms that we have a significant number of short-lived tasks in the system.

## Memory allocation analysis

One of the most common techniques to improve performance is to reduce memory allocation. Julia
provides several tools to measure this:

### `@time`

The total amount of allocation can be measured with [`@time`](@ref), [`@allocated`](@ref) and [`@allocations`](@ref),
and specific lines triggering allocation can often be inferred from profiling via the cost of garbage
collection that these lines incur. However, sometimes it is more efficient to directly measure
the amount of memory allocated by each line of code.

### GC Logging

While [`@time`](@ref) logs high-level stats about memory usage and garbage collection over the course
of evaluating an expression, it can be useful to log each garbage collection event, to get an
intuitive sense of how often the garbage collector is running, how long it's running each time,
and how much garbage it collects each time. This can be enabled with
[`GC.enable_logging(true)`](@ref), which causes Julia to log to stderr every time
a garbage collection happens.

### [Allocation Profiler](@id allocation-profiler)

!!! compat "Julia 1.8"
    This functionality requires at least Julia 1.8.

The allocation profiler records the stack trace, type, and size of each
allocation while it is running. It can be invoked with
[`Profile.Allocs.@profile`](@ref).

This information about the allocations is returned as an array of `Alloc`
objects, wrapped in an `AllocResults` object. The best way to visualize these is
currently with the [PProf.jl](https://github.com/JuliaPerf/PProf.jl) and
[ProfileCanvas.jl](https://github.com/pfitzseb/ProfileCanvas.jl) packages, which
can visualize the call stacks which are making the most allocations.

The allocation profiler does have significant overhead, so a `sample_rate`
argument can be passed to speed it up by making it skip some allocations.
Passing `sample_rate=1.0` will make it record everything (which is slow);
`sample_rate=0.1` will record only 10% of the allocations (faster), etc.

!!! compat "Julia 1.11"

    Older versions of Julia could not capture types in all cases. In older versions of
    Julia, if you see an allocation of type `Profile.Allocs.UnknownType`, it means that
    the profiler doesn't know what type of object was allocated. This mainly happened when
    the allocation was coming from generated code produced by the compiler. See
    [issue #43688](https://github.com/JuliaLang/julia/issues/43688) for more info.

    Since Julia 1.11, all allocations should have a type reported.

For more details on how to use this tool, please see [the talk from JuliaCon 2022](https://www.youtube.com/watch?v=BFvpwC8hEWQ).

##### Allocation Profiler Example

In this simple example, we use PProf to visualize the alloc profile. You could use another
visualization tool instead. We collect the profile (specifying a sample rate), then we visualize it.
```julia
using Profile, PProf
Profile.Allocs.clear()
Profile.Allocs.@profile sample_rate=0.0001 my_function()
PProf.Allocs.pprof()
```

Here is a more in-depth example, showing how we can tune the sample rate. A
good number of samples to aim for is around 1 - 10 thousand. Too many, and the
profile visualizer can get overwhelmed, and profiling will be slow. Too few,
and you don't have a representative sample.


```julia-repl
julia> import Profile

julia> @time my_function()  # Estimate allocations from a (second-run) of the function
  0.110018 seconds (1.50 M allocations: 58.725 MiB, 17.17% gc time)
500000

julia> Profile.Allocs.clear()

julia> Profile.Allocs.@profile sample_rate=0.001 begin   # 1.5 M * 0.001 = ~1.5K allocs.
           my_function()
       end
500000

julia> prof = Profile.Allocs.fetch();  # If you want, you can also manually inspect the results.

julia> length(prof.allocs)  # Confirm we have expected number of allocations.
1515

julia> using PProf  # Now, visualize with an external tool, like PProf or ProfileCanvas.

julia> PProf.Allocs.pprof(prof; from_c=false)  # You can optionally pass in a previously fetched profile result.
Analyzing 1515 allocation samples... 100%|████████████████████████████████| Time: 0:00:00
Main binary filename not available.
Serving web UI on http://localhost:62261
"alloc-profile.pb.gz"
```
Then you can view the profile by navigating to http://localhost:62261, and the profile is saved to disk.
See PProf package for more options.

##### Allocation Profiling Tips

As stated above, aim for around 1-10 thousand samples in your profile.

Note that we are uniformly sampling in the space of _all allocations_, and are not weighting
our samples by the size of the allocation. So a given allocation profile may not give a
representative profile of where most bytes are allocated in your program, unless you had set
`sample_rate=1`.

Allocations can come from users directly constructing objects, but can also come from inside
the runtime or be inserted into compiled code to handle type instability. Looking at the
"source code" view can be helpful to isolate them, and then other external tools such as
[`Cthulhu.jl`](https://github.com/JuliaDebug/Cthulhu.jl) can be useful for identifying the
cause of the allocation.

##### Allocation Profile Visualization Tools

There are several profiling visualization tools now that can all display Allocation
Profiles. Here is a small list of some of the main ones we know about:
- [PProf.jl](https://github.com/JuliaPerf/PProf.jl)
- [ProfileCanvas.jl](https://github.com/pfitzseb/ProfileCanvas.jl)
- VSCode's built-in profile visualizer (`@profview_allocs`) [docs needed]
- Viewing the results directly in the REPL
  - You can inspect the results in the REPL via [`Profile.Allocs.fetch()`](@ref), to view
    the stacktrace and type of each allocation.

#### Line-by-Line Allocation Tracking

An alternative way to measure allocations is to start Julia with the `--track-allocation=<setting>` command-line
option, for which you can choose `none` (the default, do not measure allocation), `user` (measure
memory allocation everywhere except Julia's core code), or `all` (measure memory allocation at
each line of Julia code). Allocation gets measured for each line of compiled code. When you quit
Julia, the cumulative results are written to text files with `.mem` appended after the file name,
residing in the same directory as the source file. Each line lists the total number of bytes
allocated. The [`Coverage` package](https://github.com/JuliaCI/Coverage.jl) contains some elementary
analysis tools, for example to sort the lines in order of number of bytes allocated.

In interpreting the results, there are a few important details. Under the `user` setting, the
first line of any function directly called from the REPL will exhibit allocation due to events
that happen in the REPL code itself. More significantly, JIT-compilation also adds to allocation
counts, because much of Julia's compiler is written in Julia (and compilation usually requires
memory allocation). The recommended procedure is to force compilation by executing all the commands
you want to analyze, then call [`Profile.clear_malloc_data()`](@ref) to reset all allocation counters.
 Finally, execute the desired commands and quit Julia to trigger the generation of the `.mem`
files.

!!! note

    `--track-allocation` changes code generation to log the allocations, and so the allocations may
    be different than what happens without the option. We recommend using the
    [allocation profiler](@ref allocation-profiler) instead.

## External Profiling

Currently Julia supports `Intel VTune`, `OProfile` and `perf` as external profiling tools.

Depending on the tool you choose, compile with `USE_INTEL_JITEVENTS`, `USE_OPROFILE_JITEVENTS` and
`USE_PERF_JITEVENTS` set to 1 in `Make.user`. Multiple flags are supported.

Before running Julia set the environment variable [`ENABLE_JITPROFILING`](@ref ENABLE_JITPROFILING) to 1.

Now you have a multitude of ways to employ those tools!
For example with `OProfile` you can try a simple recording :

```
>ENABLE_JITPROFILING=1 sudo operf -Vdebug ./julia test/fastmath.jl
>opreport -l `which ./julia`
```

Or similarly with `perf` :

```
$ ENABLE_JITPROFILING=1 perf record -o /tmp/perf.data --call-graph dwarf -k 1 ./julia /test/fastmath.jl
$ perf inject --jit --input /tmp/perf.data --output /tmp/perf-jit.data
$ perf report --call-graph -G -i /tmp/perf-jit.data
```

There are many more interesting things that you can measure about your program, to get a comprehensive list
please read the [Linux perf examples page](https://www.brendangregg.com/perf.html).

Remember that perf saves for each execution a `perf.data` file that, even for small programs, can get
quite large. Also the perf LLVM module saves temporarily debug objects in `~/.debug/jit`, remember
to clean that folder frequently.
