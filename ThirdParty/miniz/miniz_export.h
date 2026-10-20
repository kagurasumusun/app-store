/* Build shim for the vendored miniz sources.
 *
 * Upstream generates this header with CMake's GenerateExportHeader. The
 * library is compiled directly into the application, so no visibility
 * attributes are required.
 */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED
#define MINIZ_DEPRECATED_EXPORT MINIZ_EXPORT
#define MINIZ_DEPRECATED_NO_EXPORT MINIZ_NO_EXPORT

#endif /* MINIZ_EXPORT_H */
