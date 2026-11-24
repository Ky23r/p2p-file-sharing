#ifndef P2P_TRANSFER_H
#define P2P_TRANSFER_H

#include "file_protocol.h"

enum download_result {
    DOWNLOAD_OK, DOWNLOAD_LOCAL_ERROR, DOWNLOAD_CANCELLED, DOWNLOAD_CONNECT_FAILED,
    DOWNLOAD_TIMEOUT, DOWNLOAD_TRANSFER_FAILED, DOWNLOAD_UNAVAILABLE, DOWNLOAD_NOT_FOUND,
    DOWNLOAD_SIZE_MISMATCH, DOWNLOAD_SIZE_CHANGED
};

/* Borrow a validated search result and existing trusted destination directory;
 * neither is retained. With signals, call only from the main thread and keep
 * that controller alive throughout. NULL signals disables cancellation.
 * Failure preserves errno after cleanup.
 * Never overwrite a destination; remove only this call's temporary file. An
 * already published complete file is retained if a signal arrives afterward.
 */
enum download_result download_file(const FileSearchResult *result, const char *directory,
                                   ClientSignals *signals);

#endif
