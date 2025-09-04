# amiga-fuse

Ever wanted to mount your old Amiga ADF disk images on your Mac or Linux box and actually edit them? Yeah, me too! I got tired of not being able to easily work with my Amiga disk images on modern systems, so I had an AI assistant help me create this FUSE driver to solve that problem once and for all.

This lets you mount ADF files just like any other disk - you can browse them, edit files, create new ones, whatever you need. No more messing around with emulators just to copy a file or two.

## What it does

- Mounts ADF disk images as regular filesystems
- Full read/write support - create, edit, delete files and folders
- Works on both macOS and Linux (sorry Windows folks, maybe later)
- Handles both OFS and FFS filesystem types
- Actually understands Amiga's weird BCPL strings and filesystem quirks
- Keeps track of free space and all that boring filesystem stuff automatically
- Shows real file sizes that update when you edit things

Coming soon: HDF (hard disk) support! Because why stop at floppies?

## What you need

### macOS
You'll need macFUSE first - grab it from https://osxfuse.github.io/ and install it. Then:

```bash
# Get the build tools
brew install cmake pkg-config
```

### Linux
Most distros have what you need in their repos:

```bash
# Ubuntu/Debian folks:
sudo apt-get install libfuse3-dev cmake pkg-config build-essential

# Fedora/RHEL people:
sudo dnf install fuse3-devel cmake pkgconfig gcc-c++

# Arch users (you know who you are):
sudo pacman -S fuse3 cmake pkgconf base-devel
```

## Building this thing

Just run the build script:
```bash
./build.sh
```

That's it! It'll figure out your platform and build everything. If you want to see what went wrong (or get fancy):

```bash
./build.sh --help          # Show options
./build.sh --debug         # Debug build if things get weird
./build.sh --clean         # Start over
```

If you're the type who likes doing things manually:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Using it

Pretty straightforward - just point it at an ADF file and where you want it mounted:

```bash
./amiga-fuse /path/to/your/disk.adf /path/to/mount/point
```

It'll automatically figure out if you can write to the ADF and mount accordingly. Here's what I usually do:

```bash
# Mount my old Workbench disk
./amiga-fuse workbench13.adf ~/amiga_disk

# See what's on there
ls -la ~/amiga_disk

# Add some files or whatever
echo "Hello from 2024!" > ~/amiga_disk/hello.txt
mkdir ~/amiga_disk/new_stuff

# When you're done, unmount it
fusermount -u ~/amiga_disk  # Linux
umount ~/amiga_disk         # macOS
```

## What works

Pretty much everything you'd expect:
- Create, read, write, delete files
- Make and remove directories  
- Copy files in and out
- Edit text files directly
- All the usual filesystem stuff

The code handles all the weird Amiga filesystem details automatically - block allocation, free space tracking, checksums, BCPL strings, timestamps, all that stuff you don't want to think about.

## The nerdy details

Right now this handles standard 880K floppy ADFs with both OFS and FFS filesystems. It's written in C++23 (because why not use modern stuff), and it's optimized to be small and fast. The binary comes out to about 52KB which is pretty decent.

Internally it uses memory-mapped I/O and caches directory info to keep things snappy. All the endian conversion and Amiga-specific quirks are handled transparently.

## What doesn't work yet

- HD ADF files (coming eventually)
- Some of the more exotic ADF variants
- Windows support (FUSE is a pain there)
- Hard links (but Amiga didn't really use those anyway)

## When things go wrong

Most problems are FUSE-related. If the build fails, make sure you have macFUSE installed on Mac or the FUSE dev libraries on Linux. You can check with:

```bash
pkg-config --exists fuse && echo "FUSE is there" || echo "No FUSE found"
```

If mounting fails with "Device not configured", FUSE isn't working. On Linux make sure the fuse module is loaded (`lsmod | grep fuse`). On Mac, reinstall macFUSE.

If you get "Transport endpoint is not connected", you probably have a stale mount from a previous crash. Just unmount it and try again.

## Hacking on it

The code is all in `amiga-fuse.cpp` - it's a single file because I didn't want to deal with a bunch of headers. The CMake setup handles building on different platforms automatically.

If you want to contribute or fix bugs, just make sure it builds on both Mac and Linux, and test it with a few different ADF files. The Amiga filesystem has some weird edge cases.

## What's next

I'm planning to add HDF support next - those are the hard disk images. Way more useful than floppies for actual work, but the filesystem format is a bit more complex.

Maybe Windows support eventually, but honestly FUSE on Windows is kind of a mess.

## License

Do whatever you want with this code. I just wanted to mount my old Amiga disks without jumping through hoops.

## Acknowledgments

This implementation was developed with substantial assistance from Claude, an AI assistant. The core algorithms, structural fixes, and optimizations were implemented through iterative AI-assisted development.

---

*Now go mount some ADF files and relive the glory days of the Amiga!*