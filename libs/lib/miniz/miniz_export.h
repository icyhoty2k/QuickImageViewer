#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Hand-written stand-in for the file miniz's own CMake generates.
 *
 * Upstream produces miniz_export.h with generate_export_header(), which fills
 * MINIZ_EXPORT with __declspec(dllexport/dllimport) so the library can be built
 * as a DLL. qIV compiles miniz's sources straight into the executable — there is
 * no DLL boundary and nothing to export — so the macro expands to nothing.
 *
 * Vendored rather than generated on purpose: pulling in miniz's CMakeLists to
 * obtain one empty #define would make this project's build depend on another
 * project's build system, and that is a far larger thing to own than nine lines.
 *
 * If miniz is ever updated, check whether upstream added anything else to this
 * header — as of 11.3.2 MINIZ_EXPORT and MINIZ_NO_EXPORT are all it carries.
 */

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT

#endif /* MINIZ_EXPORT_H */
