# Ubertooth manuals

These roff pages are generated from the Markdown documentation in the parent
directory, which is the source: edit `../<tool>.md`, not the `.1` here.

The generated pages are committed because they are what `make install`
installs, so `man ubertooth-util` works on a machine without ronn. A build
never writes to this directory and never needs ronn.

## Refreshing a page after editing its Markdown

After editing a `../<tool>.md`, regenerate its page here and commit both:

```sh
cmake -S host -B build
cmake --build build --target refresh-manpages
```

That renders every generated page into the build directory and copies the ones
that changed over the files here. Review the diff and commit the `.1` with the
`.md` in the same change. `--target manpages` renders them into the build
directory without touching this one, to see first what a refresh would write.

The `Man pages` CI job (`.github/workflows/manpages.yml`) runs that refresh on
every pull request that touches `host/doc` and fails if it changes anything, so
a `.md` edit cannot land without its page.

## The generator

ronn-ng **0.10.1** is the one supported generator:

```sh
gem install ronn-ng -v 0.10.1
```

Another version renders the same Markdown differently, so the committed pages
would stop matching a refresh and the CI check would fail on a page nobody
touched. The version is `RONN_VERSION` in `CMakeLists.txt`, which warns at
configure time when the installed ronn is a different one, and the CI job
installs exactly it.

The footer date comes from `MAN_DATE` in `CMakeLists.txt`, not from a file
timestamp, so a refresh writes the same bytes on any machine and in any month.

Two Markdown habits matter here, because ronn drops what it reads as inline
HTML: write a `SYNOPSIS` as an indented block, and put `<placeholder>` inside
backticks in prose. Without that the angle brackets, or the whole line, vanish
from the rendered page.

## Pages kept by hand

`ubertooth-debug.1` has no generated counterpart and is edited here directly,
in step with `../ubertooth-debug.md` in the same change. The list of generated
pages is in `CMakeLists.txt`.

`ubertooth-ducky.md`, `ubertooth-follow.md` and `ubertooth-tx.md` have no page
here at all, so those tools have no `man` entry; the Markdown is the only
documentation for them.
