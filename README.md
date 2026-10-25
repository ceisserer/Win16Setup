# Win16Setup

A graphical installer for Windows 3.1 / Windows for Workgroups 3.11 programs, configured by an INI file. It is
written in C for Win16 and built with Open Watcom 2.0. A packer script makes a ZIP and a 1.44 MB floppy image of
your product.

* Classic blue Windows 3.1 setup screen, a dialog with directory, optional choice list (for example themes or
  languages) with preview image, progress bar
* Copies the files, writes INI entries, adds a program to `WIN.INI` `load=`, creates a Program Manager group (DDE),
  starts the program
* Optional features as check boxes (`[Options]`): each can change INI entries, also in `SYSTEM.INI` (the old values
  are restored when the option is turned off or the product removed), add programs to `load=`, start a program
  and ask for a restart of Windows
* Products that replace the Windows shell (`SYSTEM.INI` `shell=`) can be updated and removed while they are the
  running shell: Setup finishes after a restart with Program Manager (shell handover)
* Stops your running program before replacing files (or asks the user to close it)
* Silent install (`SETUP /S`) for scripts and networks
* Running `SETUP.EXE` again (it installs itself) changes the options or removes the product; removal deletes the
  files, the `WIN.INI` entries and the Program Manager group
* Reproducible packages: ZIP and floppy image with fixed dates

Requirements on the target: Windows 3.1 or WfW 3.11, Standard or 386 Enhanced mode.

## Using it

1. Put your product's files into a directory (DOS 8.3 names, upper case).
2. Write a `SETUP.INF` (below; `example/SETUP.INF` is a complete one).
3. Package:

   ```sh
   make                                   # build/SETUP.EXE (or take it from a release)
   tools/w16pack.py pack --inf SETUP.INF --files myfiles --setup build/SETUP.EXE \
       --out dist --name myprog-1.0 --label MYPROG --version 1.0
   ```

   `dist/myprog-1.0/` holds the installation files, `dist/myprog-1.0.zip` and `dist/myprog-1.0-disk.img` the
   packages, `dist/SHA256SUMS` their checksums. `w16pack` writes the `[Files]` section of `SETUP.INF` from the
   files and replaces `@VERSION@` with `--version`.

`make example` packages the example product `HELLO.EXE` (`example/`) the same way.

## Command line

```
SETUP                        install; if installed: change the options or remove
SETUP /S [/D=dir] [/<switch>=item] [/[NO]LOAD] [/[NO]GROUP] [/[NO]START] [/[NO]<option>] [/RESTART]
                             silent install with the defaults or these options
SETUP /U [/S] [/RESTART]     remove (asks first unless /S)
SETUP /RESUME                finish a shell handover (the staged copy does this by itself)
```

`/<switch>` is the choice's `Switch=` (default `C`), `/<option>` and `/NO<option>` an option's `Switch=`.
`/NOSTART` also keeps the options' `Run=` programs from starting. Silent runs restart Windows only with `/RESTART`. Setup writes `SETUP.LOG` into the installation directory
(`W16UNINS.LOG` in TEMP when removing, `W16SETUP.LOG` in the Windows directory if the installation directory could
not be created).

## SETUP.INF

All sections except `[Setup]` and `[Files]` are optional; a feature without its section is not offered. Numbered
entries (`1=`, `2=`, ...) are read until the first missing number. Values may use these variables:

| Variable | Value |
|---|---|
| `%DIR%` | installation directory, e.g. `C:\HELLO` |
| `%SRC%` | directory of the running SETUP.EXE |
| `%WINDIR%` | Windows directory |
| `%PRODUCT%`, `%VERSION%` | from `[Setup]` |
| `%CHOICE%` | the chosen item of `[Choice]` |
| `%ITEM%` | the item being shown (in `[Choice]` `NameFrom=` and `Preview=`) |

`\n` in a text starts a new line.

```ini
[Setup]
Product=Hello                 ; name in titles and messages (required)
Version=1.0
DefaultDir=C:\HELLO
Key=Hello                     ; WIN.INI section that records the installation (default: Product)
Title=%PRODUCT% Setup         ; title of the blue screen and the dialogs (default)
Tagline=Version %VERSION%     ; second line on the blue screen (default)
Welcome=...                   ; text in the dialog (first installation)
WelcomeInstalled=...          ; text in the dialog when installed
Done=%PRODUCT% %VERSION% has been installed in %DIR%.

[Files]                       ; written by w16pack: paths relative to the setup directory
1=HELLO.EXE
2=GREET\EN.INI

[Choice]                      ; one item of a list, e.g. a theme or a language
Label=&Greeting:
Switch=G                      ; SETUP /S /G=DE
Default=EN
NameFrom=GREET\%ITEM%.INI,Greeting,Name   ; display name from an INI file (file,section,key) ...
1=EN
2=DE,Deutsch                  ; ... or given after the item
Preview=THEMES\%ITEM%\PREVIEW.BMP         ; optional picture next to the dialog (see below)

[Ini]                         ; private INI entries: file,section,key,value
1=%DIR%\HELLO.INI,Hello,Greeting,%DIR%\GREET\%CHOICE%.INI

[Autostart]                   ; check box: add the program to WIN.INI load=
Program=HELLO.EXE
Label=Start %PRODUCT% with &Windows
Default=1

[Group]                       ; check box: Program Manager group
Name=Hello
1=%DIR%\HELLO.EXE,Hello       ; AddItem: command line,title[,icon file[,icon index]]
2=NOTEPAD.EXE %DIR%\README.TXT,Read Me,NOTEPAD.EXE
Label=Create a Program Manager &group
Default=1

[StartNow]                    ; check box: start a program after installing
Program=HELLO.EXE
Show=normal                   ; or minimized
Label=Start %PRODUCT% &now
Default=1

[Stop]                        ; before files are replaced
Window=Hello                  ; window class that gets WM_CLOSE
Wait=HELLO,HELLODLL           ; module names to wait for (15 s at most)

[Running]                     ; programs the user must close first: module,name
1=HELLOCFG,Hello Configuration

[Uninstall]                   ; files the product creates, deleted on removal
1=HELLO.INI

[Options]                     ; up to 4 optional features: check boxes below the choice
1=Shell
2=Look

[Option.Shell]                ; one section per option: [Option.<name>]
Label=Use %PRODUCT% as the Windows &shell
Switch=SHELL                  ; SETUP /S /SHELL or /NOSHELL (default: the name)
Default=1
Restart=1                     ; offer to restart Windows when the option changes
Ini1=%WINDIR%\SYSTEM.INI,boot,shell,%DIR%\MYSHELL.EXE   ; file,section,key,value
Ini2=%WINDIR%\SYSTEM.INI,boot,taskman.exe,%DIR%\MYSHELL.EXE

[Option.Look]
Label=&Extra look
Ini1=%DIR%\LOOK\LOOK.INI,Look,Theme,%CHOICE%
Load=LOOK\LOOK.EXE            ; added to WIN.INI load= (relative to %DIR%)
Run=LOOK\LOOK.EXE,minimized   ; started after installing
Window=LookWindow             ; closed when the option is turned off or files are replaced
Wait=LOOK                     ; modules waited for (as in [Stop])
```

The choice is stored in `WIN.INI` `[<Key>]` `Choice=` and is the default when Setup runs again.

**Options:** an option that is on writes its `Ini<n>=` entries. The value an entry had before (or that it was
missing) is kept in `WIN.INI` `[<Key>]` `Old.<name>.<n>=`; turning the option off, or removing the product, puts it
back if the entry still has the option's value. The option's state is `Option.<name>=` there.

**Shell handover:** a running shell cannot be stopped (ending it ends Windows), so its files cannot be replaced or
deleted. If `SYSTEM.INI` `shell=` names a program in the installation directory and it is running, Setup copies
itself (and, for an installation, the product's files) to `TEMP\W16STAGE`, keeps the chosen options and the old
`shell=` in `RESUME.INI` there, makes Program Manager the shell, adds the copy to `WIN.INI` `run=` and restarts
Windows. The copy, started by Program Manager, puts `shell=` back, removes itself from `run=`, finishes the
installation or removal and restarts Windows again if the product is the shell. `[Running]` entries for the shell's module are skipped. The next run of any Win16Setup
deletes the staged copy.

**Preview images:** a DIB of at most 64 KB, shown centred in a 320 × 200 area; `tools/w16pack.py preview in.png
out.bmp` converts a screenshot to an 8-bit BMP. Without `Preview=` the dialog is compact.

**Icon:** `make ICON=myicon.ico` builds `SETUP.EXE` with your icon (32 × 32, 16 colours).

## How removal works

A running Win16 program cannot delete its own EXE. When the installed copy removes the product, it copies itself
into the TEMP directory, renames the module in the copy's NE header from `W16SETUP` to `W16UNINS` (otherwise
Windows would start another instance of the module that is already loaded instead of loading the copy), starts it
with `/X=<dir>` and exits. The copy waits until `W16SETUP` has ended and deletes the files. The next run of any
Win16Setup deletes the copy.

## Building

```sh
make WATCOM=/opt/watcom       # build/SETUP.EXE
make example                  # build/example/: HELLO packaged
```

Open Watcom 2.0 (`ow-snapshot.tar.xz` from the open-watcom-v2 releases), Python 3, mtools for floppy images and
Pillow for preview images. GitHub Actions builds both on every push and publishes a release for a `v*` tag.

## Used by

* [Retheme16](https://github.com/ceisserer/win31_theme_engine): the Windows 95 look for Windows 3.1
* [NewShell16](https://github.com/ceisserer/NewShell16): a Calmira-style shell for Windows 3.1 (uses `[Options]` and
  the shell handover, and bundles Retheme16)

## License

Win16Setup is free software under the GNU General Public License, version 3 or (at your option) any later version;
see [LICENSE](LICENSE). Packages made with it may ship `SETUP.EXE` for any product.
