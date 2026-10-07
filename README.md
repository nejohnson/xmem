# xmem

`xmem` is a small X11 memory usage graph in the style of `xload` and `xdf`. It
plots physical memory and swap usage as percentages on one graph. The vertical
scale always runs from 0 to 100%; horizontal lines mark 25%, 50%, and 75%.

Build it with `make` (a C compiler and Xlib development files are required),
then run:

```sh
./xmem                         # refreshed every 5 seconds
./xmem -update 1 -geometry 240x100
./xmem -noswap -nolabel
./xmem -display :0             # local X screen when DISPLAY is SSH-forwarded
```

Options: `-update`, `-label`, `-nolabel`, `-noswap`, `-display`, `-geometry`,
`-fg`/`-foreground`, `-bg`/`-background`, `-hl`/`-highlight`, `-cachecolor`,
and `-swapcolor`. Run `./xmem -help` for a summary.

What is drawn:

- **Filled area (foreground):** memory in use, `MemTotal - MemAvailable` from
  `/proc/meminfo`, as a percentage of physical memory. Memory the kernel can
  reclaim is not counted as used.
- **Lighter band above it (`-cachecolor`):** reclaimable cache (page cache,
  buffers and reclaimable slab, excluding shared memory), stacked on top of
  the used memory.
- **Line (`-swapcolor`):** swap in use as a percentage of *swap* space. This is
  a percentage of a different total from the memory area, so a 50% swap line
  is not the same amount of data as 50% memory. The label reads `mem 54% (+35%) swap 10%`: used, cache on top, swap.
  The line is omitted if the system has no swap.

The graph retains one sample per pixel of window width and preserves the
newest samples when resized. The label rounds up to a whole percent.

The window sets its own icon (`_NET_WM_ICON`), a miniature of the graph, so
panels and task switchers show it instead of a generic X icon.
