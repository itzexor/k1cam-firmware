/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * uvcd_firmware.h -- the firmware protocol on k1cam's Extension Unit, version 1.
 *
 * Three fixed 60-byte controls on XU entity 4. 60 bytes is the most a UVC
 * control carries through the kernel gadget's event interface.
 *
 *   26 COMMAND  SET_CUR    one command frame
 *   27 DATA     SET_CUR    one chunk of an update bundle
 *               GET_CUR    the window the last READ selected
 *   28 STATUS   GET_CUR    where things stand
 *
 * All integers are little-endian; bytes not named here are reserved and
 * must be zero (a frame with a nonzero reserved byte is rejected).
 *
 * USB acknowledging a SET_CUR means only that the frame arrived: the daemon
 * reads it afterwards. Every COMMAND and DATA frame therefore carries a
 * host-chosen nonzero sequence number, and its outcome is the STATUS whose
 * SEQUENCE equals it: RESULT 0 accepted, anything else the reason it was
 * rejected. A frame sent again with the same sequence and the same bytes
 * is not executed again and keeps its first result; the same sequence with
 * other bytes is rejected as STALE_SEQUENCE. One frame at a time: wait for
 * its STATUS before sending the next.
 *
 * Operations that take time (taking a dump snapshot, installing an update,
 * booting into it) run in the background. PHASE, OPERATION, DONE/TOTAL and
 * ERROR describe the current one; poll STATUS, waiting POLL_MS between
 * reads, until it settles.
 */
#ifndef UVCD_FIRMWARE_H
#define UVCD_FIRMWARE_H

#include <stdint.h>

#define UVCD_FW_COMMAND_SELECTOR 26
#define UVCD_FW_DATA_SELECTOR 27
#define UVCD_FW_STATUS_SELECTOR 28
#define UVCD_FW_FRAME_SIZE 60
#define UVCD_FW_PROTOCOL_VERSION 1

#define UVCD_FW_MAGIC_0 'K'
#define UVCD_FW_MAGIC_1 '1'
#define UVCD_FW_MAGIC_2 'F'
#define UVCD_FW_MAGIC_3 'W'

/* Commands. SESSION is 0 unless a command says otherwise. */
enum uvcd_fw_opcode {
	UVCD_FW_OP_NONE = 0,
	/* Refresh the device information record (TOTAL = its length), read
	 * with READ target INFO. */
	UVCD_FW_OP_INFO = 1,
	/* Select the DATA window: TARGET (UVCD_FW_TARGET_*) at OFFSET. The
	 * dump targets need the dump SESSION. */
	UVCD_FW_OP_READ = 2,
	/* Start an update upload: LENGTH bundle bytes whose SHA-256 is
	 * DIGEST. STATUS then carries the new SESSION, PHASE RECEIVING. */
	UVCD_FW_OP_BEGIN_UPGRADE = 3,
	/* Cancel SESSION's operation where that is still possible. With
	 * UVCD_FW_FLAG_UNSCHEDULE, also withdraw a scheduled trial (SESSION
	 * may be 0 for one scheduled earlier). */
	UVCD_FW_OP_ABORT = 4,
	/* Start a dump of the whole flash (TARGET 0). The daemon copies it to
	 * RAM, with the data partition's writers paused, unless memory is
	 * short; then, only with UVCD_FW_FLAG_ALLOW_LIVE, it reads the flash
	 * as the host asks (a diagnostic dump, not a consistent one). */
	UVCD_FW_OP_BEGIN_DUMP = 5,
	/* All LENGTH bytes of SESSION's bundle are sent: check them, write
	 * the inactive slot, verify it from flash and schedule a trial. */
	UVCD_FW_OP_FINISH_UPGRADE = 6,
	/* Reboot into the scheduled trial (SESSION of a STAGED update, or 0
	 * for one scheduled earlier). */
	UVCD_FW_OP_REBOOT = 7,
	/* Release SESSION's dump. */
	UVCD_FW_OP_END_DUMP = 8,
	/* Not a command: STATUS OPCODE for a DATA frame. */
	UVCD_FW_OP_DATA = 0x80,
};

enum uvcd_fw_target {
	UVCD_FW_TARGET_IMAGE = 0,    /* the dump itself */
	UVCD_FW_TARGET_INFO = 1,     /* the device information record */
	UVCD_FW_TARGET_MANIFEST = 2, /* the dump's size, digest and consistency */
	UVCD_FW_TARGET_NONE = 0xff,  /* DATA GET_CUR before anything was selected */
};

#define UVCD_FW_FLAG_ALLOW_LIVE 0x01 /* BEGIN_DUMP */
#define UVCD_FW_FLAG_UNSCHEDULE 0x01 /* ABORT */

enum uvcd_fw_phase {
	UVCD_FW_PHASE_IDLE = 0,
	UVCD_FW_PHASE_COMPLETE = 1,
	UVCD_FW_PHASE_ERROR = 2,       /* the operation failed: ERROR says why */
	UVCD_FW_PHASE_ACCEPTED = 3,    /* queued for the background worker */
	UVCD_FW_PHASE_RECEIVING = 4,   /* update upload in progress */
	UVCD_FW_PHASE_VALIDATING = 5,
	UVCD_FW_PHASE_ERASING = 6,
	UVCD_FW_PHASE_PROGRAMMING = 7,
	UVCD_FW_PHASE_VERIFYING = 8,
	UVCD_FW_PHASE_STAGED = 9,      /* trial scheduled: reboot to run it */
	UVCD_FW_PHASE_REBOOTING = 10,
	UVCD_FW_PHASE_ABORTED = 11,
	UVCD_FW_PHASE_READING = 12,    /* dump snapshot being taken */
	UVCD_FW_PHASE_READY = 13,      /* dump readable */
};

enum uvcd_fw_error {
	UVCD_FW_ERROR_NONE = 0,
	UVCD_FW_ERROR_BAD_FRAME = 1,       /* wrong length, magic or reserved bytes */
	UVCD_FW_ERROR_BAD_VERSION = 2,
	UVCD_FW_ERROR_UNSUPPORTED = 3,     /* unknown opcode or target */
	UVCD_FW_ERROR_STALE_SEQUENCE = 4,
	UVCD_FW_ERROR_SESSION = 5,         /* no such session */
	UVCD_FW_ERROR_BUSY = 6,            /* another operation is running */
	UVCD_FW_ERROR_STATE = 7,           /* not possible in the current phase */
	UVCD_FW_ERROR_RANGE = 8,           /* offset or length out of range */
	UVCD_FW_ERROR_GAP = 9,             /* DATA not where the upload stands */
	UVCD_FW_ERROR_CONFLICT = 10,       /* DATA resent with other bytes */
	UVCD_FW_ERROR_CHECKSUM = 11,       /* DATA frame CRC mismatch */
	UVCD_FW_ERROR_NO_MEMORY = 12,
	UVCD_FW_ERROR_TOO_LARGE = 13,
	UVCD_FW_ERROR_LAYOUT = 14,         /* flash layout or geometry not supported */
	UVCD_FW_ERROR_SLOT_STATE = 15,     /* boot state does not allow an update */
	UVCD_FW_ERROR_BUNDLE = 16,         /* malformed bundle */
	UVCD_FW_ERROR_INCOMPATIBLE = 17,   /* other board/layout, or newer boot compat */
	UVCD_FW_ERROR_HASH = 18,           /* bundle or component digest mismatch */
	UVCD_FW_ERROR_IMAGE = 19,          /* uImage or SquashFS not intact */
	UVCD_FW_ERROR_MTD = 20,            /* opening or reading flash failed */
	UVCD_FW_ERROR_ERASE = 21,
	UVCD_FW_ERROR_WRITE = 22,
	UVCD_FW_ERROR_VERIFY = 23,         /* flash read back differs */
	UVCD_FW_ERROR_ENV = 24,            /* boot environment unreadable or unwritable */
	UVCD_FW_ERROR_OVERLAY = 25,        /* preparing the new slot's settings failed */
	UVCD_FW_ERROR_NOT_CANCELLABLE = 26,
	UVCD_FW_ERROR_TIMEOUT = 27,        /* session abandoned by the host */
	UVCD_FW_ERROR_DISCONNECTED = 28,   /* USB went away mid-session */
	UVCD_FW_ERROR_REBOOT = 29,
};

enum uvcd_fw_feature {
	UVCD_FW_FEATURE_INFO = 1u << 0,
	UVCD_FW_FEATURE_DUMP = 1u << 1,
	UVCD_FW_FEATURE_UPGRADE = 1u << 2,  /* the A/B layout, and its boot state allows it */
	UVCD_FW_FEATURE_AB_SLOTS = 1u << 3, /* the A/B layout */
};

/* STATUS BOOT: how the running system was started. */
enum uvcd_fw_boot {
	UVCD_FW_BOOT_UNKNOWN = 0,
	UVCD_FW_BOOT_SINGLE = 1,    /* single-slot layout */
	UVCD_FW_BOOT_CONFIRMED = 2, /* the confirmed slot */
	UVCD_FW_BOOT_TRIAL = 3,     /* a trial of a new slot, not yet confirmed */
	UVCD_FW_BOOT_FALLBACK = 4,  /* the other slot, the confirmed one wouldn't boot */
};

/* Slots in STATUS: 0 a, 1 b, 0xff none or unknown (fw_layout.h). */

/* COMMAND frame */
#define UVCD_FW_COMMAND_MAGIC 0
#define UVCD_FW_COMMAND_VERSION 4
#define UVCD_FW_COMMAND_OPCODE 5
#define UVCD_FW_COMMAND_TARGET 6
#define UVCD_FW_COMMAND_FLAGS 7
#define UVCD_FW_COMMAND_SEQUENCE 8
#define UVCD_FW_COMMAND_SESSION 12
#define UVCD_FW_COMMAND_OFFSET 16
#define UVCD_FW_COMMAND_LENGTH 20
#define UVCD_FW_COMMAND_DIGEST 24 /* 32 bytes: BEGIN_UPGRADE only */
#define UVCD_FW_COMMAND_RESERVED 56

/* STATUS frame */
#define UVCD_FW_STATUS_MAGIC 0
#define UVCD_FW_STATUS_VERSION 4
#define UVCD_FW_STATUS_PHASE 5
#define UVCD_FW_STATUS_OPCODE 6    /* the last frame's opcode (UVCD_FW_OP_DATA for DATA) */
#define UVCD_FW_STATUS_RESULT 7    /* its result: 0 accepted, else UVCD_FW_ERROR_* */
#define UVCD_FW_STATUS_SEQUENCE 8  /* its sequence number */
#define UVCD_FW_STATUS_SESSION 12  /* the current session, 0 if none */
#define UVCD_FW_STATUS_DONE 16     /* progress: bytes received, written or copied */
#define UVCD_FW_STATUS_TOTAL 20
#define UVCD_FW_STATUS_FEATURES 24
#define UVCD_FW_STATUS_POLL_MS 28  /* u16: how long to wait between polls */
#define UVCD_FW_STATUS_ACTIVE_SLOT 30
#define UVCD_FW_STATUS_TRIAL_SLOT 31
#define UVCD_FW_STATUS_RUNNING_SLOT 32
#define UVCD_FW_STATUS_BOOT 33     /* UVCD_FW_BOOT_* */
#define UVCD_FW_STATUS_OPERATION 34 /* BEGIN_DUMP or BEGIN_UPGRADE, 0 if none */
#define UVCD_FW_STATUS_ERROR 35    /* the operation's error, in PHASE ERROR */
#define UVCD_FW_STATUS_DETAIL 36   /* u32: errno or other detail for RESULT/ERROR */
#define UVCD_FW_STATUS_RESERVED 40

/* DATA frame. SET carries a bundle chunk at OFFSET in SESSION's upload;
 * GET_CUR returns the window the last accepted READ selected, with that
 * READ's SESSION and SEQUENCE, KIND its target, LENGTH bytes of it (0 at
 * the end). CRC is CRC-16/CCITT-FALSE over bytes 0-13 and the LENGTH
 * payload bytes; payload bytes past LENGTH are zero. */
#define UVCD_FW_DATA_SESSION 0
#define UVCD_FW_DATA_OFFSET 4
#define UVCD_FW_DATA_SEQUENCE 8
#define UVCD_FW_DATA_LENGTH 12
#define UVCD_FW_DATA_KIND 13
#define UVCD_FW_DATA_CRC 14
#define UVCD_FW_DATA_PAYLOAD 16
#define UVCD_FW_DATA_MAX (UVCD_FW_FRAME_SIZE - UVCD_FW_DATA_PAYLOAD) /* 44 */

static inline uint16_t uvcd_fw_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t uvcd_fw_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline void uvcd_fw_put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void uvcd_fw_put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static inline int uvcd_fw_has_magic(const uint8_t *p)
{
	return p[0] == UVCD_FW_MAGIC_0 && p[1] == UVCD_FW_MAGIC_1 &&
	       p[2] == UVCD_FW_MAGIC_2 && p[3] == UVCD_FW_MAGIC_3;
}

static inline void uvcd_fw_put_magic(uint8_t *p)
{
	p[0] = UVCD_FW_MAGIC_0;
	p[1] = UVCD_FW_MAGIC_1;
	p[2] = UVCD_FW_MAGIC_2;
	p[3] = UVCD_FW_MAGIC_3;
}

#endif /* UVCD_FIRMWARE_H */
