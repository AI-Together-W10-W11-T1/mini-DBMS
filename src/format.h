#ifndef MD_FORMAT_H
#define MD_FORMAT_H
#define WIN32_LEAN_AND_MEAN
#include "minidb.h"
#include <windows.h>
typedef struct {
    char small[4096];
    char *p;
    size_t n, cap;
    bool failed;
} Buffer;
void mf_begin(Buffer *);
void mf_end(Buffer *);
bool mf_render(const MdResult *, bool, Buffer *);
bool mf_stream_write(DWORD, const char *, size_t);
void mf_diagnostic_render(const MdDiagnostic *, const char *, bool, Buffer *);
void mf_diagnostic(const MdDiagnostic *, const char *, bool);
int mf_diagnostic_exit(const MdDiagnostic *, bool);
int mf_combine(int, int);
#endif
