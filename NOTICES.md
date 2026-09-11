# Third-party notices

Synth is licensed under the GNU AGPL v3 (see `LICENSE`). It is built with, and its
distributable binaries statically incorporate, the following third-party components.

## JUCE

Portions of this software use the JUCE framework (<https://juce.com>).
Copyright © Raw Material Software Limited. JUCE is distributed under the terms of the
GNU General Public License / GNU Affero General Public License (or a commercial JUCE
licence). The full JUCE licence text ships in the JUCE source tree
(`modules/` and the top-level `LICENSE.md` of the JUCE distribution).

## Steinberg VST 3 SDK

VST 3 plug-in interface support is provided via the Steinberg VST 3 SDK, as bundled with
JUCE. The VST 3 SDK is distributed by Steinberg under the GNU General Public License v3
(dual-licensed with a proprietary Steinberg VST 3 licence). **VST** is a registered
trademark of Steinberg Media Technologies GmbH.

## LAME (libmp3lame)

MP3 export is provided by **libmp3lame 3.100** (<https://lame.sourceforge.io/>), the LAME
project's MPEG Audio Layer III encoder, Copyright © the LAME project contributors.
libmp3lame is distributed under the **GNU Lesser General Public License, version 2.1**; the
full licence text ships in the LAME source tree (`COPYING`). Its source is fetched at build
time, pinned by URL and SHA-256, by `cmake/lame.cmake`.

Only the encoder is incorporated (the 19 C sources directly under `libmp3lame/`); the
mpglib decoder, the SSE-intrinsics `vector/` path and the `i386/` assembly are not built.
The sources are used **unmodified** — the only addition is `cmake/lame-config.h`, which
replaces the `config.h` LAME's autotools would normally generate.

LGPL-2.1 §3 permits distribution under the terms of the GNU GPL v2 or later, which makes
libmp3lame compatible with this project's AGPL v3 licence. As required, the complete
corresponding source for the combined work — including the exact LAME version and the
build files that incorporate it — is available at the repository linked below.

**MP3 patents.** The MP3 format's patents expired worldwide in 2017, so distributing an
MP3 encoder no longer requires a patent licence.

## Source availability (AGPL)

The complete corresponding source for this release is the public repository
<https://github.com/Sanglock81/synth>.
