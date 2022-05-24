`tabulate` is the 2nd generation of my personal solution to pretty-print my
monthly finances. Supersedes [`spreadsheet`](https://github.com/levirak/spreadsheet).

# Motivation

I had been using `spreadsheet` for a few years before I got tired of some of
its jankiness. Broadly speaking, files designed for the previous solution were
too fragile, and I wanted something more semantic. On top of that, I was
becoming unhappy with how I had structured the code, and I was embarrassed that I
still hadn't gotten around to implementing a real expression parser.

# Solution

I decided that I was sticking too faithfully to a generic spreadsheet, whereas
what I wanted was to work with structured tables of data. By restricting the
problem space in this way, I could implement the semantics I wanted, such as a
way to designate between the head, body, and foot of a table; to designate and
reference a summary cell for a sheet; or to specify that I wanted a sum of a
column to only consider rows from the table's body.

I decided to write a new program instead of continue `spreadsheet` for two
reasons. The first was that it would be refreshing to start from scratch. The
second was that my planed upgrades would not be backwards compatible, so having
both on my system would let me use the old program while I prepared the new
one.

# Language Choice

C is my favorite language because it doesn't do anything for me. I enjoy
working with it because it necessitates me to engage with systems that are
usually abstracted away in other languages, and in turn I find that this helps
me appreciate those other languages more. For similar reasons I have chosen for
my build system to be a hand written makefile.

I usually write for the most recent version of GNU C because I usually have a
newer `gcc` installed. On top of that, I write in a personal dialect of C that
I don't let get too alien. As a taste of that dialect, I use `s32` in place of
`int` and `u64` in place of `unsigned long`, and I have added the keyword
`InvalidCodePath` as a debug trap. All additions I write with are placed in
[`main.h`](src/main.h), which I include in every source file.
