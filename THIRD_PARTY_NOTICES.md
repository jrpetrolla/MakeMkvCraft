# Third-Party Notices

MakeMkvCraft binary distributions contain dynamically linked third-party
software. Each component remains subject to its own copyright and license.
The portable archive includes the corresponding license texts in its
`licenses` directory.

## Primary runtime components

| Component | Use | License |
| --- | --- | --- |
| Qt 5 | Desktop user interface | LGPL-3.0-only or an alternative license offered by Qt |
| FFmpeg | Container and stream processing | GPL-3.0-or-later for the distributed MXE build (`--enable-gpl --enable-version3`) |
| libbluray | Blu-ray structure access | LGPL-2.1-or-later |
| libdvdread | DVD structure access | GPL-2.0-or-later |
| libdvdnav | Logical DVD playback | GPL-2.0-or-later |

The official package intentionally does **not** include `libdvdcss`.

## Transitive runtime components

The MXE-built FFmpeg, Qt, TLS, text, image, compression, and compiler runtime
libraries may also include the following projects:

| Components | License family |
| --- | --- |
| Brotli | MIT |
| bzip2 | bzip2 license |
| FreeType | FreeType License or GPL-2.0-or-later |
| GLib | LGPL-2.1-or-later |
| GMP | LGPL-3.0-or-later or GPL-2.0-or-later |
| GnuTLS | LGPL-2.1-or-later |
| HarfBuzz | MIT |
| ICU | Unicode-3.0 |
| GNU gettext runtime | LGPL-2.1-or-later |
| GNU libiconv | LGPL-2.1-or-later |
| GNU libidn2 | LGPL-3.0-or-later |
| libogg and libvorbis | BSD-3-Clause |
| libpng | libpng-2.0 |
| libtasn1 | LGPL-2.1-or-later |
| libunistring | LGPL-3.0-or-later |
| libxml2 | MIT |
| LAME | LGPL-2.0-or-later |
| mpg123 | LGPL-2.1-or-later |
| Nettle and Hogweed | LGPL-3.0-or-later or GPL-2.0-or-later |
| OpenCORE AMR and vo-amrwbenc | Apache-2.0 |
| Opus and Speex | BSD-3-Clause |
| PCRE2 | BSD-3-Clause |
| XZ Utils/liblzma | 0BSD |
| x264, x265, and Xvid | GPL-2.0 license family |
| zlib | Zlib |
| Zstandard | BSD-3-Clause or GPL-2.0-only |
| GCC and libstdc++ runtime libraries | GPL-3.0-or-later with GCC Runtime Library Exception 3.1 |
| MinGW-w64 runtime and winpthreads | Permissive MinGW-w64 runtime licenses |

## Source availability

The MakeMkvCraft source corresponding to each release is available from the
release tag at:

<https://github.com/jrpetrolla/MakeMkvCraft>

Upstream source locations and build recipes for bundled libraries are recorded
by the MXE project: <https://mxe.cc/>.

This notice is informational and does not replace any bundled license text.
