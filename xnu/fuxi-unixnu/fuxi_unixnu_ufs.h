// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ported from UniXNU drivers/NativeUfs/TransferPoll.h (BSD-2-Clause-Patent)
// Ported from UniXNU drivers/NativeUfs/RootCommands.h (BSD-2-Clause-Patent)
// See LICENSE.NativeUfs and LICENSE.UniXNU. C policy for XNU 8796's single-slot BSD transport.
#ifndef FUXI_UNIXNU_UFS_H
#define FUXI_UNIXNU_UFS_H
#include <stdint.h>

#define FUXI_UFS_FATAL_STATUS 0x71880u
enum fuxi_ufs_end { FUXI_UFS_WAIT, FUXI_UFS_COMPLETE, FUXI_UFS_FATAL, FUXI_UFS_TIMEOUT };

static inline enum fuxi_ufs_end
fuxi_unixnu_ufs_end(uint32_t doorbell, uint32_t status, uint32_t ocs, int expired)
{
	if (status & FUXI_UFS_FATAL_STATUS) return FUXI_UFS_FATAL;
	/* A fast completion may never expose DB=1. DB=0 alone is insufficient. */
	if (!(doorbell & 1) && ocs != 15) return FUXI_UFS_COMPLETE;
	return expired ? FUXI_UFS_TIMEOUT : FUXI_UFS_WAIT;
}

static inline int
fuxi_unixnu_ufs_response(const uint8_t *rsp, uint8_t lun, uint8_t tag)
{
	unsigned length = (unsigned)rsp[10] << 8 | rsp[11];
	if (rsp[0] != 0x21 || rsp[2] != lun || rsp[3] != tag || rsp[8] || length > 480)
		return 0;
	if (length) {
		unsigned sense = (unsigned)rsp[32] << 8 | rsp[33];
		if (length < 2 || sense > length - 2) return 0;
	}
	/* A successful short/overrun read must never expose stale bounce bytes. */
	if (!rsp[6] && !rsp[7] && ((rsp[1] & 0x60) || rsp[12] || rsp[13] || rsp[14] || rsp[15]))
		return 0;
	return 1;
}
#endif
