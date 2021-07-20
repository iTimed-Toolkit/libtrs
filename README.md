# libtrace

libtrace is a high performance, open-source library which can be used for
common processing tasks in side channel analysis. It features a series of
composable "transformations", which take side channel trace data and act
on it in some way -- an example includes `tfm_cpa`, which implements a
generic correlation power analysis approach. Included also are various
crypto utilities for determining vulnerable intermediate values, a set
of optimized statistical accumulators (also with GPU support!), and multi
platform support (both Linux and Windows)!

## Building

libtrace requires only a recent OpenSSL and zlib installation in order to
compile. For Linux, simply build as-per-usual for Cmake applications. For
Windows, first download and install an OpenSSL installation (I used version 
`1.1.1k`  from [here](https://slproweb.com/products/Win32OpenSSL.html)). Also
run `git submodule init --recursive` to fetch the zlib sources, which we will
build in-tree.

Configure with `-DSTATS_USE_GPU=1` if you want to use GPU acceleration for
statistics calculation.

## Design