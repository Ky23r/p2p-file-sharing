#ifndef P2P_TRANSFER_H
#define P2P_TRANSFER_H

#include "file_protocol.h"

/* NULL on success, a REPORT_FAIL reason for remote failure, or LOCAL_ERROR.
 * Never overwrite a destination; remove only this call's temporary file.
 */
const char *download_file(const file_result *result, const char *directory);

#endif
