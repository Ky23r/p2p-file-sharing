#ifndef P2P_TRANSFER_H
#define P2P_TRANSFER_H

#include "file_protocol.h"

/* Borrow a validated search result and existing trusted destination directory;
 * neither is retained. With signals, call only from the main thread and keep
 * that controller alive throughout. NULL signals disables cancellation.
 * Return NULL on success, a static error name on failure, or CANCELLED for local shutdown.
 * Do not free returned strings. Failure preserves errno after cleanup.
 * Never overwrite a destination; remove only this call's temporary file. An
 * already published complete file is retained if a signal arrives afterward.
 */
const char *download_file(const FileSearchResult *result, const char *directory, ClientSignals *signals);

#endif
