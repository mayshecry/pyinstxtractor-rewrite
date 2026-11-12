# pyinstxtractor (C rewrite)

A safer, cleaner, faster PyInstaller archive extractor, written in C.
It extracts Python modules, packages, entry points, and nested PYZ
archives from PyInstaller-generated executables — no Python installation
required, not even a copy of the archive's Python version.

C rewrite by **mayshecry**, based on the original Python implementation
[pyinstxtractor.py](https://github.com/extremecoders-re/pyinstxtractor) by
**Extreme Coders** (https://0xec.blogspot.com).

## Supported formats

PyInstaller cookie versions 2.0 and 2.1+, which covers every PyInstaller
release from 2.0 through 6.x (the CArchive cookie structure never changed).
Python 3.x bytecode headers are generated automatically for 3.3, 3.7+, and
everything in between.

PYZ tables of contents are unmarshalled in-process, supporting both the
list-of-tuples form used since PyInstaller 3.1 and the dict form used by
older releases. This is the one place where a native implementation has to
reproduce CPython's `marshal` wire format rather than rely on the `marshal`
module — see [Marshal refs](#marshal-refs) below.

## Build

```bash
make
```

Requires `zlib` development headers (`zlib1g-dev` on Debian/Ubuntu,
`zlib-devel` on Fedora/RHEL, `zlib` on macOS/Homebrew).

## Usage

```bash
./pyinstxtractor [options] <executable> [output_directory]
```

If no output directory is given, files are extracted into
`<executable>_extracted/` next to the executable.

| Option | Meaning |
|--------|---------|
| `-v, --verbose` | Print the full CArchive table of contents |
| `-o, --output <dir>` | Extract to `<dir>` instead of the default |
| `-h, --help` | Show help |

Exit status is `0` on success and `1` on any failure, so it composes in
shell pipelines.

```bash
$ ./pyinstxtractor -v my_app
[+] PyInstaller version: 2.1+
[+] Python version: 3.13
[+] Length of package: 20853764 bytes
[+] Found 54 files in CArchive
    [  0] type 'm'      235 ->      289 bytes  struct
    ...
[+] Possible entry point: my_app.pyc
[+] Found 112 files in PYZ archive
[+] Successfully extracted PyInstaller archive: my_app
```

## Design notes

* **Memory safety** — every allocation is checked, every read is
  bounds-checked, and the whole thing is clean under
  `-fsanitize=address,undefined` including leak detection.
* **Path safety** — member names are normalized *by path component*, not by
  substring replacement. Naive `..`-substring scrubbing misses names like
  `"../evil"` (no `/..` substring) and `"....//x"`; this implementation
  splits on separators, drops `.`, and rewrites `..` to `__`, so nothing can
  be written outside the output directory. Regression-tested.
* **No `chdir` side-effects** — paths are built explicitly instead of
  changing the process working directory, so the tool is safe to call as a
  library or from a thread.
* **Proper CLI** — verbose mode that actually prints the TOC, an explicit
  output directory, `--help`, meaningful exit codes.
* **Unique filenames** — collisions get deterministic `_N` suffixes instead
  of random UUIDs, so repeat runs are reproducible.
* **Deterministic `.pyc` headers** — correct PEP 552 (3.7+) or legacy (3.3+)
  headers are generated for the target Python version, and bare `.pyc` files
  are patched in a second pass once the magic is known.
* **No Python version gate** — there is no need to run under the same
  Python version as the build, because marshal is parsed natively, so the
  PYZ is always unpacked.
* **Embedded PYZ unpacking** — a small, purpose-built marshal reader
  extracts nested modules, including correct `pkg/__init__.pyc` placement
  for package entries.
* **Bounded I/O** — only the TOC and each member are read, not the whole
  executable; decompression grows its buffer instead of trusting (possibly
  tampered) size fields.
* **Standard build** — a simple `Makefile`, no dependencies beyond `libz`.

## Marshal refs

CPython's `marshal` deduplicates repeated objects with `TYPE_REF` (`0x72`),
which stores a back-reference index. The index is *not* a count of
previously seen ints; it counts **every object whose type byte has the
`FLAG_REF` (`0x80`) high bit set**. In a PyInstaller TOC the first entry is
emitted with `FLAG_REF` on the list, the tuples and the strings, so a later
tuple can reference `ref 4` for an integer written several objects earlier.

Getting this wrong is subtle: the structure still parses, but `ispkg`
silently becomes the previous entry's `pos`, so packages are written as
`pkg.pyc` instead of `pkg/__init__.pyc`. This implementation keeps a typed
refs table (`MREF_INT` / `MREF_OTHER`) and only resolves `TYPE_REF` to
values that really are ints. Verified against CPython `marshal.loads()` on a
real PyInstaller 6.22 / Python 3.13 build: 112/112 TOC entries match
exactly.

## Tests

```bash
make check
```

This builds an ASan/UBSan instrumented binary and runs:

* `test/smoke.sh` — builds a throwaway PyInstaller executable (or uses
  `PYINST_EXE`), extracts it, and asserts every emitted `.pyc` is valid
  marshal data.
* `test/pathsafety.sh` — extracts a synthetic archive full of traversal
  attempts and asserts all output stays inside the destination directory.

`make asan` builds the instrumented binary on its own if you want to poke at
something by hand.

## Credits

* **Extreme Coders** — original `pyinstxtractor.py` implementation, which
  defined the approach this tool follows. E-mail: extremecoders(at)hotmail(dot)com
  · Web: https://0xec.blogspot.com · Original project:
  https://github.com/extremecoders-re/pyinstxtractor
* **mayshecry** — this C rewrite.

## License

GNU General Public License v3, inherited from the original work. The full
text is in [LICENSE](LICENSE).

The original Python script is © Extreme Coders and is licensed under GPL v3.
Because this C rewrite is derived from it, it is distributed under the same
terms. You are free to modify and redistribute it under the GPL v3.
