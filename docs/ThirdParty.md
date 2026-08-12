# Third-party dependencies

All dependencies are pulled, pinned, and built from source via FetchContent
(`cmake/Dependencies.cmake`), linked statically, and used **unmodified**. Each entry's
rationale and build flags live in the comments in that file; this table is the
attribution summary.

| Dependency | Version | License | Used for |
|-----------|---------|---------|----------|
| GLM | 1.0.1 | MIT | vector/matrix math |
| Assimp | 5.4.3 | BSD-3-Clause | model import (editor) |
| EnTT | 3.13.2 | MIT | entity-component system |
| nlohmann/json | 3.11.3 | MIT | project/asset JSON |
| stb | master | MIT / public domain | image load, easy font, glyph atlas packing |
| FreeType | 2.13.3 | FTL / BSD-3-Clause | UI text: TTF/OTF glyph rasterization |
| HarfBuzz | 10.1.0 | MIT (Old) | UI text: Unicode shaping (kerning/ligatures/complex scripts) |
| SheenBidi | 2.6 | Apache-2.0 | UI text: Unicode Bidirectional Algorithm (RTL) |
| LunaSVG (+plutovg) | 2.4.1 | MIT | UI vector graphics: on-demand SVG rasterization |
| miniaudio | 0.11.21 | MIT / public domain | audio playback |
| Jolt Physics | 5.2.0 | MIT | rigid-body + soft-body physics |
| Mbed TLS | 3.6.7 | Apache-2.0 | collaboration transport security |
| libdatachannel | 0.24.5 | MPL-2.0 (file-level) | peer-to-peer transport (editor only) |
| NanoVDB / OpenVDB | 12.0.1 | Apache-2.0 (PNanoVDB.h: MPL-2.0) | sparse-volume representation |
| Dear ImGui + ImGuizmo | (vendored) | MIT | editor UI (editor only) |
| **Recast / Detour / DetourTileCache** | **1.6.0** | **zlib** | **navigation: bake (Recast, editor) + runtime (Detour, DetourTileCache)** |

## Recast Navigation (zlib license)

> Copyright (c) 2009 Mikko Mononen memon@inside.org
>
> This software is provided 'as-is', without any express or implied warranty. In no
> event will the authors be held liable for any damages arising from the use of this
> software.
>
> Permission is granted to anyone to use this software for any purpose, including
> commercial applications, and to alter it and redistribute it freely, subject to the
> following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not claim that you
>    wrote the original software. If you use this software in a product, an acknowledgment
>    in the product documentation would be appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be
>    misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

Heartbreak uses Recast Navigation unmodified. **Recast** (navmesh generation) is linked
only into the editor (`hbe_editor`); the shipped runtime links only **Detour** +
**DetourTileCache**. See `docs/Navigation.md` for the architecture and
`cmake/Dependencies.cmake` for the runtime/editor split.

## MPL-2.0 note (libdatachannel, PNanoVDB.h)

MPL-2.0 is file-level copyleft: linking these into a proprietary engine is fine; only
modifications to *their* files would oblige publishing those files. Heartbreak modifies
none of them.
