#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Restricted entry point; never starts the GUI or preset backend. */
int cab_privileged_main(int argc, wchar_t **argv);
#ifdef CAB_WINDOWS_STATIC_AUTH
void *cab_windows_authorize(const wchar_t *helper, const wchar_t *const *paths, unsigned long count, unsigned long *status);
uintptr_t cab_windows_authorized_open(void *batch, const wchar_t *path);
void cab_windows_authorization_close(void *batch);
#endif
#ifdef __cplusplus
}
#endif
