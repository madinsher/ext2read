# Ext2Read

> **Fork notice.** This is a fork of [mregmi/ext2read](https://github.com/mregmi/ext2read) that builds with
> **MSVC + Qt 6** and reads current Linux (e.g. Ubuntu) ext4 partitions correctly. See
> [Changes in this fork](#changes-in-this-fork). All credit for Ext2Read goes to Manish Regmi and the upstream
> contributors; the GPLv2 license is unchanged.

**Ext2Read** is an explorer like utility to explore ext2/ext3/ext4 files.
It runs directly without any installation.
It can be used to view and copy files and folders.
It can recursively copy entire folders.
It can also be used to view and copy disk and file.
It now supports LVM2 and EXT4 extents. 

# Features
- Simple UI designed using Qt4/Qt5
- View/Read ext2/ext3/ext4 partitions
- Linux LVM2 Support
- Ext4 Large File support (untested)
- Recursively Copy the entire folder or even `/`
- Support for external USB disks
- Support for disk and filesystem  images
  For e.g. Wubi users can just open their root.disk file through this program
- LRU based Block cache for faster access
- Unicode support

# Usage

The executables and sources can be downloaded from http://ext2read.sf.net

## Notice
- This program must be run as Administrator in order to directly access partitions. If you are not automatically asked to elevate, right-click the file and select _Run as Administrator_.
- This is not a transparent file system driver, just a user space tool.
- The LVM2 metadata can be complex because of the wide variety of configuration possibilities. All configurations has not been tested. If LVM2 does not work in your system, please file a bug with your LVM2 metadata.

# Contributing

If you find any bugs, have any questions or comments, please let us know.

If any of you are interested in joing this project, you can join the mailing list and discuss. Join from here https://lists.sourceforge.net/lists/listinfo/ext2read-devel

## Building

### Windows

Building for Windows requires _MinGW_ compiler.

**NOTE:** Either copy the dependent Qt DLLs into the same folder as the built `ext2explore.exe` or setup your PATH environment variable correctly!
Common mistakes are other PATH entries which bundle an old version of Qt or the wrong bitness (32/64 Bit).

#### Windows (32bit)

Qt officially supports MinGW 32bit, so just download their official release, run Qt Creator and build.

#### Windows (64bit)

Qt does not officially support MinGW 64bit. Therefor we need to setup the toolchain on our own:
1. Download and extract a compiler: [MinGW-W64 (e.g. posix-seh)](https://sourceforge.net/projects/mingw-w64/files/Toolchains%20targetting%20Win64/Personal%20Builds/mingw-builds/)
2. Download and extract prebuilt Qt libraries: [Qt64-NG (e.g. posix-seh)](https://sourceforge.net/projects/qt64ng/files/qt/x86-64/)  
   **NOTE:** Unfortunately this project is closed after Qt version 5.5.0. If you need a newer version, [build it yourself](https://wiki.qt.io/Building_Qt_Desktop_for_Windows_with_MinGW#Build_Qt_with_MinGW_for_a_x64_.28x86_64.29_target).
   1. Run `qtbinpatcher.exe` whenever you move your folder.
3. Download and install [Qt-Creator](https://www.qt.io/download-open-source/#section-2)
4. [Setup Qt Creator](http://doc.qt.io/qtcreator/creator-configuring.html#checking-build-and-run-settings)
   in **Tools > Options > Build & Run**
   1. Add Compiler (C: `gcc.exe`, C++: `g++.exe`, 32/64 Bit: select _ABI_)
   2. Add Debugger (`gdb.exe`)
   3. Add Qt version (`qmake.exe`)
   4. Add Kit

Afterwards, you can start the build process.

#### Windows (MSVC + Qt 6)

This fork also builds with Visual Studio (Build Tools 2022 is enough) and an official Qt 6 `msvc2019_64` kit,
no MinGW needed. From a command prompt:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Qt\6.7.3\msvc2019_64\bin;%PATH%
mkdir build & cd build
qmake ..\ext2explore.pro CONFIG+=release
nmake release
windeployqt --release release\ext2explore.exe
```

`ext2explore.exe` asks for Administrator rights (UAC) because it reads the physical disks directly.
Run it from its own folder: it writes `ext2explorelog.log` into the working directory, which is the quickest way
to see what the scan found if something does not show up.

# Changes in this fork

Based on upstream commit `a728e9f`. Everything else (UI, features, license) is as upstream.

## Bug fixes

- **"Unable to read disk. Please make sure you are running this application as an Administrator" even when
  running as Administrator.** `\\.\PhysicalDriveN` was opened with `FILE_SHARE_READ` only. On current Windows
  the disk is already open for writing elsewhere, so that fails with `ERROR_SHARING_VIOLATION` (error code 0x20).
  It is now opened with `FILE_SHARE_READ | FILE_SHARE_WRITE`; access is still `GENERIC_READ`, the program
  never writes to a disk.
- **Crash while scanning partitions with MSVC.** `LOG` calls used the GCC/glibc-only format `%Ld`, which is
  invalid in the MSVC runtime and terminates the process. Replaced with `%llu` and an explicit cast.
- **ext4 with the `64bit` feature (the default of `mkfs.ext4` for years, e.g. on Ubuntu).** Group descriptors
  are 64 bytes there (`s_desc_size`), but they were read as 32-byte structures. Only block group 0 was read
  correctly, so every inode stored in another group was garbage: the root directory was listed, but its
  sub-folders looked empty. The descriptor size, the high half of the block count and of the inode table
  address are now taken from the superblock, and the number of block groups is rounded up instead of down.
- **Truncated directory listings.** Listing stopped at the first directory entry with inode 0. With
  `metadata_csum` every directory block ends with such an entry (the checksum tail), and htree interior blocks
  consist of one, so multi-block directories lost all entries after the first block. Unused entries are now
  skipped, and a corrupt `rec_len` can no longer make the reader loop or run backwards.

## Portability (so it builds with MSVC and Qt 6)

- `__attribute__((packed))` replaced by `#pragma pack` plus the existing `PACKED` macro; the on-disk
  superblock layout is checked at compile time (`sizeof == 1024`, offsets of `s_desc_size` and
  `s_blocks_count_hi`).
- Qt 6 API changes: `QTextCodec` removed (`QString::fromUtf8`), `QRegExp` replaced by `QRegularExpression`,
  `QDateTime::setTime_t` replaced by `setSecsSinceEpoch` (guarded by a Qt version check),
  `qVariantFromValue` replaced by `QVariant::fromValue`.
- `__builtin_bswap64` wrapped for MSVC (`_byteswap_uint64`), the unused `<dirent.h>` include removed,
  `HANDLE < 0` comparison replaced by an `INVALID_FILE_HANDLE` macro.
- `ext2explore.pro`: `-static-libgcc -static-libstdc++` only for GCC, and for MSVC the linker no longer embeds
  its own manifest, so the project's `resource/manifest.xml` (`requireAdministrator`) is the one that ends up in
  the executable.

## Tested and not tested

Tested: Windows 11, Visual Studio Build Tools 2022 (MSVC 14.39), Qt 6.7.3 `msvc2019_64`; one GPT NVMe disk with an
Ubuntu ext4 partition (4 KiB blocks, 256-byte inodes, `64bit`, `metadata_csum`). Browsing from `/` through
`/home` to a user's home folder works, with no read errors in the log.

Not tested: copying files and folders out of the partition, `inline_data`, sparse files and uninitialized
extents (`ee_len > 32768` is not handled), LVM2, MBR/extended partition layouts, 32-bit builds, and building
with MinGW/GCC or Qt 5 (the changes avoid GCC-only or Qt 6-only constructs where possible, but only MSVC +
Qt 6.7.3 was actually built).

## Known limitations

- Windows only, read-only, and only reads partitions of physical disks and raw disk/filesystem images.
  A WSL2 `ext4.vhdx` is a VHDX container, not a raw image, so it cannot be opened directly.
