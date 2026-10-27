#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H
#define BYTEORDER 1234
#define HAVE_MEMMOVE 1
/* No namespace expansion, external DTDs, general entity expansion or diagnostic
 * input-context buffering. Built-in XML/numeric references remain supported.
 * The backend independently rejects every DOCTYPE before reading its subset. */
#define XML_GE 0
#define XML_CONTEXT_BYTES 0
/* The backend explicitly supplies a cryptographically random XML hash salt. */
#define XML_POOR_ENTROPY 1
#endif
