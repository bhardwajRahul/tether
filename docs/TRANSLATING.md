# Translating Tether

Tether uses GNU gettext. Catalogs live in `po/`, one `.po` per language, all in the
`tether` domain.

## Adding a language

1. Add the language code to `po/LINGUAS`.
2. Run `make pot`. It regenerates `po/tether.pot`, merges it into every existing
   catalog, and creates a new empty `.po` for the language you just added.
3. Translate `po/<lang>.po` with Poedit, Weblate, or any text editor.
4. Rebuild. `po/CMakeLists.txt` compiles each catalog and installs it to
   `${CMAKE_INSTALL_LOCALEDIR}/<lang>/LC_MESSAGES/tether.mo`.

## Updating after source changes

`make pot` re-extracts from the files listed in `po/POTFILES.in`. A source file with
new `_()` strings must be listed there, or its strings are never extracted.

## Testing without installing

```sh
make debug
export TETHER_LOCALEDIR=$PWD/build/debug/locale
LANGUAGE=es LC_ALL=en_US.UTF-8 ./build/debug/tether --help
LANGUAGE=de LC_ALL=en_US.UTF-8 ./build/debug/tether-gtk
```

`LANGUAGE` is only honored when the locale is not `C`/`POSIX`, so pair it with a real
`LC_ALL` that exists on the machine (`locale -a` lists them). An untranslated string
falls back to English; that is correct behavior, not a failure.

## Rules for translators

- **Keep every format placeholder.** `%s`, `%zu`, `{}`, `{0}` must all survive. Dropping
  one crashes or corrupts the output. `msgfmt --check-format` catches the printf kinds;
  `ctest -R po_` runs it over every catalog.
- **Reorder with indices, not by moving text.** Use `%1$s` / `%2$s` for printf strings
  and `{0}` / `{1}` for the `{}` kind when the target language needs another word order.
- **Preserve leading and trailing whitespace and newlines.** Do not add padding to
  align columns: the CLI computes its own column widths from the translated labels
  (`tether::display_width` measures terminal columns, so CJK glyphs count as two).
  A translation that pads by hand will be misaligned.
- **Keep Pango markup out of your way.** Markup tags are applied around translated
  strings in code, so no msgid should contain `<b>` — if you find one, report it.
- **Do not translate** protocol and product tokens: `Bluetooth`, `Wi-Fi`, `iPhone`,
  `BR/EDR`, `LE`, `ANCS`, `MAP`, `PBAP`, `OTP`, `BlueZ`, `bluetoothd`, D-Bus names such
  as `org.bluez.Bearer.LE1`, package names, and CLI flags like `--bt-setup`.
- **Match Apple's own wording** for anything naming an iPhone setting ("Show Message
  Notifications", "Sync Contacts", "Share System Notifications", "Forget This Device").
  Use the strings Apple ships in that language rather than a literal translation, so the
  advice matches what the user actually sees on the phone.

## Non-Latin scripts

Tether bundles no fonts; glyphs come from the system's fontconfig. Most desktop distros
ship Noto CJK and friends, but a minimal install may render boxes for `ja`, `ko`, or
`zh_CN`. That is a missing font package on the host, not a catalog problem.

## Continuous integration

`.github/workflows/i18n.yml` fails a change that adds a `_()` string without re-running
`make pot`, and fails any catalog that is not fully translated. `ctest -R po_`, which
runs inside the normal build, checks every catalog's format specifiers.

## Status of the shipped catalogs

All fifteen catalogs — `cs`, `de`, `es`, `fr`, `it`, `ja`, `ko`, `nl`, `pl`, `pt_BR`,
`ru`, `sv`, `tr`, `uk`, `zh_CN` — are machine-assisted drafts that have not been
reviewed by native speakers. Corrections are welcome.
