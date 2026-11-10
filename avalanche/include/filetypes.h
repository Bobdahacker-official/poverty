#ifndef AV_FILETYPES_H
#define AV_FILETYPES_H
#include <stddef.h>
enum file_type { FILE_TEXT, FILE_HTML, FILE_PNG, FILE_JPEG, FILE_BMP,
                 FILE_WAV, FILE_EXECUTABLE, FILE_BINARY, FILE_TYPE_COUNT };
enum file_type file_type_detect(const char *, const unsigned char *, size_t);
const char *file_type_mime(enum file_type);
#endif
