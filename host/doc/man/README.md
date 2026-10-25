# Ubertooth manuals

These roff pages are generated from the Markdown documentation in the parent
directory, which is the source: edit `../<tool>.md`, not the `.1` here.

The generated pages are committed because they are what `make install`
installs, so `man ubertooth-util` works on a machine without ronn. A build
never writes to this directory and never needs ronn.

## Refreshing a page after editing its Markdown

```sh
cmake -S host -B build
cmake --build build --target refresh-manpages
```

That renders every generated page into the build directory and copies the ones
that changed over the files here. Review the diff and commit the `.1` with the
`.md` in the same change. `--target manpages` renders them into the build
directory without touching this one, to see first what a refresh would write.

ronn (`gem install ronn-ng`, or Homebrew) is the only supported generator: the
pages here were originally produced by go-md2man, so the first refresh of a
page rewrites it wholesale in ronn's style.

## Pages kept by hand

`ubertooth-debug.1` has no generated counterpart and is edited here directly,
in step with `../ubertooth-debug.md` in the same change. The list of generated
pages is in `CMakeLists.txt`.

`ubertooth-ducky.md`, `ubertooth-follow.md` and `ubertooth-tx.md` have no page
here at all, so those tools have no `man` entry; the Markdown is the only
documentation for them.
