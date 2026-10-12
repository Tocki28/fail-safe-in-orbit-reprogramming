/*
 * image_format.h — the firmware image header (Interfaces Sec. 5.5, Figure 6).
 *
 *   offset  bytes  field
 *        0      4  proprietary string "FSIR" (0x46 0x53 0x49 0x52)
 *        4      1  format = 1
 *        5      1  kind: 0x01 bootloader, 0x02 main firmware, 0x03 last-resort firmware
 *        6      2  target = controller identity
 *        8      4  version
 *       12      4  length of the body in bytes
 *       16      4  body check value = CRC-32 of the body (Interfaces Sec. 4.0)
 *       20     32  build identifier
 *       52     12  reserved, 0x00
 * Multi-byte fields are little-endian (Interfaces Sec. 4.0). After the header: the body,
 * then Signature A and Signature B, both Ed25519 over header and body (Interfaces Sec. 5.5).
 * tools/make_image.py takes these values from this file (tools/firmware_layout.py).
 */
#ifndef IMAGE_FORMAT_H
#define IMAGE_FORMAT_H

#define IMAGE_MAGIC_0           0x46U   /* 'F' */
#define IMAGE_MAGIC_1           0x53U   /* 'S' */
#define IMAGE_MAGIC_2           0x49U   /* 'I' */
#define IMAGE_MAGIC_3           0x52U   /* 'R' */
#define IMAGE_FORMAT_VERSION    1U

#define IMAGE_KIND_BOOTLOADER   0x01U
#define IMAGE_KIND_MAIN         0x02U
#define IMAGE_KIND_LAST_RESORT  0x03U

/* Controller identity: 0x0431, after the chip, chosen for the bench; no flight value is set
 * yet. REQ-046 asks the onboard computer to refuse firmware for another controller; the
 * bootloader repeats the check. */
#define IMAGE_TARGET_CONTROLLER 0x0431U

#define IMAGE_OFFSET_MAGIC      0U
#define IMAGE_OFFSET_FORMAT     4U
#define IMAGE_OFFSET_KIND       5U
#define IMAGE_OFFSET_TARGET     6U
#define IMAGE_OFFSET_VERSION    8U
#define IMAGE_OFFSET_LENGTH     12U
#define IMAGE_OFFSET_BODY_CHECK 16U
#define IMAGE_OFFSET_BUILD_ID   20U
#define IMAGE_BUILD_ID_BYTES    32U

#endif /* IMAGE_FORMAT_H */
