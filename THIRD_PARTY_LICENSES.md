# Third-Party Licenses

Move Anything incorporates several third-party libraries and components. This document lists their licenses and attributions.

---

## Flite (Carnegie Mellon University)

**Used in:** TTS (Text-to-Speech) engine for accessibility features
**Location:** Dynamically linked (libflite, libflite_cmu_us_kal, libflite_usenglish, libflite_cmulex)
**Version:** 2.2
**Website:** http://cmuflite.org

**Copyright:**
```
Copyright (c) 1999-2016 Language Technologies Institute,
Carnegie Mellon University
All Rights Reserved.
```

**License:** BSD-style permissive license

```
Permission is hereby granted, free of charge, to use and distribute
this software and its documentation without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of this work, and to
permit persons to whom this work is furnished to do so, subject to
the following conditions:
 1. The code must retain the above copyright notice, this list of
    conditions and the following disclaimer.
 2. Any modifications must be clearly marked as such.
 3. Original authors' names are not deleted.
 4. The authors' names are not used to endorse or promote products
    derived from this software without specific prior written
    permission.

CARNEGIE MELLON UNIVERSITY AND THE CONTRIBUTORS TO THIS WORK
DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT
SHALL CARNEGIE MELLON UNIVERSITY NOR THE CONTRIBUTORS BE LIABLE
FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
```

---

## QuickJS

**Used in:** JavaScript engine for module UI execution
**Location:** `libs/quickjs/`
**Version:** 2025-04-26
**Authors:** Fabrice Bellard, Charlie Gordon
**Website:** https://bellard.org/quickjs/

**Copyright:**
```
Copyright (c) 2017-2025 Fabrice Bellard
Copyright (c) 2017-2025 Charlie Gordon
```

**License:** MIT License

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## stb_image.h

**Used in:** Image loading for display graphics
**Location:** `src/lib/stb_image.h`
**Version:** 2.30
**Author:** Sean Barrett
**Website:** https://github.com/nothings/stb

**License:** Public Domain / MIT-0

```
Public domain image loader - http://nothings.org/stb
No warranty implied; use at your own risk
```

Also available under MIT license for jurisdictions that don't recognize public domain.

---

## curl

**Used in:** HTTP downloads for Module Store
**Location:** `libs/curl/`
**Version:** Binary included in build
**Website:** https://curl.se/

**Copyright:**
```
Copyright (c) 1996 - 2024, Daniel Stenberg, <daniel@haxx.se>, and many
contributors, see the THANKS file.
```

**License:** curl License (BSD-style)

```
Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## stb_truetype.h

**Used in:** TrueType font rendering for display
**Location:** `src/lib/stb_truetype.h`
**Author:** Sean Barrett
**Website:** https://github.com/nothings/stb

**License:** Public Domain / MIT-0

```
Public domain font renderer
No warranty implied; use at your own risk
```

---

## License Compatibility

All third-party components use permissive licenses (MIT, BSD-style, Public Domain) that are compatible with Move Anything's CC BY-NC-SA 4.0 license.

**Key requirements:**
- ✅ Attribution provided (this file)
- ✅ Copyright notices retained in source files
- ✅ No endorsement claims using authors' names
- ✅ Combined work distributed under CC BY-NC-SA 4.0
- ✅ Third-party components remain under their original licenses

**Notes:**
- Flite, QuickJS, curl, and stb libraries retain their permissive licenses
- Move Anything's original code is CC BY-NC-SA 4.0 (non-commercial)
- Combined work follows the most restrictive license (CC BY-NC-SA 4.0)
- All third-party licenses permit this combination

---

## Acknowledgments

- **Carnegie Mellon University** - Flite speech synthesis library
- **Fabrice Bellard & Charlie Gordon** - QuickJS JavaScript engine
- **Sean Barrett** - stb single-file libraries
- **Daniel Stenberg** - curl HTTP library
- **Ableton** - Move hardware platform
