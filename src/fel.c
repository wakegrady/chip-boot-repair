/*
 * Copyright (C) 2012  Henrik Nordstrom <henrik@henriknordstrom.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Needs _BSD_SOURCE for htole and letoh  */
/* glibc 2.20+ also requires _DEFAULT_SOURCE */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _NETBSD_SOURCE

#include <libusb.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#include "portable_endian.h"

/* These ifdefs make it so instead of assert and exit, a throw happens */
#ifdef LIBSUNXI
#include "libsunxi.h"
#undef assert
#define assert(expr) throw_assert(expr)
#define exit(expr) throw_exit(expr)
#endif

struct  aw_usb_request {
	char signature[8];
	uint32_t length;
	uint32_t unknown1;	/* 0x0c000000 */
	uint16_t request;
	uint32_t length2;	/* Same as length */
	char	pad[10];
}  __attribute__((packed));

struct aw_fel_version {
	char signature[8];
	uint32_t soc_id;	/* 0x00162300 */
	uint32_t unknown_0a;	/* 1 */
	uint16_t protocol;	/* 1 */
	uint8_t  unknown_12;	/* 0x44 */
	uint8_t  unknown_13;	/* 0x08 */
	uint32_t scratchpad;	/* 0x7e00 */
	uint32_t pad[2];	/* unused */
} __attribute__((packed));

static const int AW_USB_READ = 0x11;
static const int AW_USB_WRITE = 0x12;

static int AW_USB_FEL_BULK_EP_OUT;
static int AW_USB_FEL_BULK_EP_IN;
static int timeout = 60000;
static int verbose = 0; /* Makes the 'fel' tool more talkative if non-zero */
static int progress = 0; /* Makes the 'fel' tool show a progress bar when transferring large files */
static uint32_t uboot_entry = 0; /* entry point (address) of U-Boot */
static uint32_t uboot_size  = 0; /* size of U-Boot binary */

static void pr_info(const char *fmt, ...)
{
	va_list arglist;
	if (verbose) {
		va_start(arglist, fmt);
		vprintf(fmt, arglist);
		va_end(arglist);
	}
}

static const int AW_USB_MAX_BULK_SEND = 4 * 1024 * 1024; // 4 MiB per bulk request

typedef void (*progress_cb_t)(int total,int sent,int len);

void progress_bar(int total,int sent,int len)
{
	if (progress && (len<total)) {
		int   w = 60;
		float r = ((float)sent)/total;
		int   x = w * r;
		int   i;

		fprintf(stderr,"\r%3d%% [", (int)(r*100) );

		for (i=0;i<x;i++) {
			fprintf(stderr,"=");
		}
		for (i=x;i<w;i++) {
			fprintf(stderr," ");
		}

		fprintf(stderr,"] ");
	}
}

void usb_bulk_send(libusb_device_handle *usb, int ep, const void *data, int length, progress_cb_t progress_cb)
{
	int rc, sent, total=length, len;
	while (length > 0) {
		len = length < AW_USB_MAX_BULK_SEND ? length : AW_USB_MAX_BULK_SEND;
		rc = libusb_bulk_transfer(usb, ep, (void *)data, len, &sent, timeout);
		if (rc != 0) {
			fprintf(stderr, "libusb usb_bulk_send error %d\n", rc);
			exit(2);
		}
		length -= sent;
		data += sent;

		if (progress_cb) {
			progress_cb(total, total-length, len);
		}
	}
}

void usb_bulk_recv(libusb_device_handle *usb, int ep, void *data, int length)
{
	int rc, recv;
	while (length > 0) {
		rc = libusb_bulk_transfer(usb, ep, data, length, &recv, timeout);
		if (rc != 0) {
			fprintf(stderr, "usb_bulk_recv error %d\n", rc);
			exit(2);
		}
		length -= recv;
		data += recv;
	}
}

/* Constants taken from ${U-BOOT}/include/image.h */
#define IH_MAGIC	0x27051956	/* Image Magic Number	*/
#define IH_ARCH_ARM		2	/* ARM			*/
#define IH_TYPE_INVALID		0	/* Invalid Image	*/
#define IH_TYPE_FIRMWARE	5	/* Firmware Image	*/
#define IH_TYPE_SCRIPT		6	/* Script file		*/
#define IH_NMLEN		32	/* Image Name Length	*/

/* Additional error codes, newly introduced for get_image_type() */
#define IH_TYPE_ARCH_MISMATCH	-1

#define HEADER_NAME_OFFSET	32	/* offset of name field	*/
#define HEADER_SIZE		(HEADER_NAME_OFFSET + IH_NMLEN)

/*
 * Utility function to determine the image type from a mkimage-compatible
 * header at given buffer (address).
 *
 * For invalid headers (insufficient size or 'magic' mismatch) the function
 * will return IH_TYPE_INVALID. Negative return values might indicate
 * special error conditions, e.g. IH_TYPE_ARCH_MISMATCH signals that the
 * image doesn't match the expected (ARM) architecture.
 * Otherwise the function will return the "ih_type" field for valid headers.
 */
int get_image_type(const uint8_t *buf, size_t len)
{
	uint32_t *buf32 = (uint32_t *)buf;

	if (len <= HEADER_SIZE) /* insufficient length/size */
		return IH_TYPE_INVALID;
	if (be32toh(buf32[0]) != IH_MAGIC) /* signature mismatch */
		return IH_TYPE_INVALID;
	/* For sunxi, we always expect ARM architecture here */
	if (buf[29] != IH_ARCH_ARM)
		return IH_TYPE_ARCH_MISMATCH;

	/* assume a valid header, and return ih_type */
	return buf[30];
}

void aw_send_usb_request(libusb_device_handle *usb, int type, int length)
{
	struct aw_usb_request req;
	memset(&req, 0, sizeof(req));
	strcpy(req.signature, "AWUC");
	req.length = req.length2 = htole32(length);
	req.request = htole16(type);
	req.unknown1 = htole32(0x0c000000);
	usb_bulk_send(usb, AW_USB_FEL_BULK_EP_OUT, &req, sizeof(req), NULL);
}

void aw_read_usb_response(libusb_device_handle *usb)
{
	char buf[13];
	usb_bulk_recv(usb, AW_USB_FEL_BULK_EP_IN, &buf, sizeof(buf));
	assert(strcmp(buf, "AWUS") == 0);
}

void aw_usb_write(libusb_device_handle *usb, const void *data, size_t len, progress_cb_t progress_cb)
{
	aw_send_usb_request(usb, AW_USB_WRITE, len);
	usb_bulk_send(usb, AW_USB_FEL_BULK_EP_OUT, data, len, progress_cb);
	aw_read_usb_response(usb);
}

void aw_usb_read(libusb_device_handle *usb, const void *data, size_t len, progress_cb_t progress_cb)
{
	aw_send_usb_request(usb, AW_USB_READ, len);
	usb_bulk_send(usb, AW_USB_FEL_BULK_EP_IN, data, len, progress_cb);
	aw_read_usb_response(usb);
}

struct aw_fel_request {
	uint32_t request;
	uint32_t address;
	uint32_t length;
	uint32_t pad;
};

static const int AW_FEL_VERSION = 0x001;
static const int AW_FEL_1_WRITE = 0x101;
static const int AW_FEL_1_EXEC  = 0x102;
static const int AW_FEL_1_READ  = 0x103;

void aw_send_fel_request(libusb_device_handle *usb, int type, uint32_t addr, uint32_t length)
{
	struct aw_fel_request req;
	memset(&req, 0, sizeof(req));
	req.request = htole32(type);
	req.address = htole32(addr);
	req.length = htole32(length);
	aw_usb_write(usb, &req, sizeof(req), NULL);
}

void aw_read_fel_status(libusb_device_handle *usb)
{
	char buf[8];
	aw_usb_read(usb, &buf, sizeof(buf), NULL);
}

void aw_fel_get_version(libusb_device_handle *usb, struct aw_fel_version *buf)
{
	aw_send_fel_request(usb, AW_FEL_VERSION, 0, 0);
	aw_usb_read(usb, buf, sizeof(*buf), NULL);
	aw_read_fel_status(usb);

	buf->soc_id = (le32toh(buf->soc_id) >> 8) & 0xFFFF;
	buf->unknown_0a = le32toh(buf->unknown_0a);
	buf->protocol = le32toh(buf->protocol);
	buf->scratchpad = le16toh(buf->scratchpad);
	buf->pad[0] = le32toh(buf->pad[0]);
	buf->pad[1] = le32toh(buf->pad[1]);
}

void aw_fel_print_version(libusb_device_handle *usb)
{
	struct aw_fel_version buf;
	aw_fel_get_version(usb, &buf);

	const char *soc_name="unknown";
	switch (buf.soc_id) {
	case 0x1623: soc_name="A10";break;
	case 0x1625: soc_name="A13";break;
	case 0x1633: soc_name="A31";break;
	case 0x1651: soc_name="A20";break;
	case 0x1650: soc_name="A23";break;
	case 0x1639: soc_name="A80";break;
	case 0x1667: soc_name="A33";break;
	case 0x1673: soc_name="A83T";break;
	case 0x1680: soc_name="H3";break;
	}

	printf("%.8s soc=%08x(%s) %08x ver=%04x %02x %02x scratchpad=%08x %08x %08x\n",
		buf.signature, buf.soc_id, soc_name, buf.unknown_0a,
		buf.protocol, buf.unknown_12, buf.unknown_13,
		buf.scratchpad, buf.pad[0], buf.pad[1]);
}

void aw_fel_read(libusb_device_handle *usb, uint32_t offset, void *buf, size_t len)
{
	aw_send_fel_request(usb, AW_FEL_1_READ, offset, len);
	aw_usb_read(usb, buf, len, progress ? progress_bar : NULL);
	if (progress) {
		fprintf(stderr,"\n");
	}

	aw_read_fel_status(usb);
}

void aw_fel_write(libusb_device_handle *usb, void *buf, uint32_t offset, size_t len)
{
	/* safeguard against overwriting an already loaded U-Boot binary */
	if (uboot_size > 0 && offset <= uboot_entry + uboot_size && offset + len >= uboot_entry) {
		fprintf(stderr, "ERROR: Attempt to overwrite U-Boot! "
			"Request 0x%08X-0x%08X overlaps 0x%08X-0x%08X.\n",
			offset, offset + (int)len,
			uboot_entry, uboot_entry + uboot_size);
		exit(1);
	}
	aw_send_fel_request(usb, AW_FEL_1_WRITE, offset, len);
	aw_usb_write(usb, buf, len, progress ? progress_bar : NULL);
	if (progress) {
		fprintf(stderr,"\n");
	}
	aw_read_fel_status(usb);
}

void aw_fel_execute(libusb_device_handle *usb, uint32_t offset)
{
	aw_send_fel_request(usb, AW_FEL_1_EXEC, offset, 0);
	aw_read_fel_status(usb);
}

void hexdump(void *data, uint32_t offset, size_t size)
{
	size_t j;
	unsigned char *buf = data;
	for (j = 0; j < size; j+=16) {
		size_t i;
		printf("%08lx: ",(long int)offset + j);
		for (i = 0; i < 16; i++) {
			if ((j+i) < size) {
				printf("%02x ", buf[j+i]);
			} else {
				printf("__ ");
			}
		}
		printf(" ");
		for (i = 0; i < 16; i++) {
			if (j+i >= size) {
				printf(".");
			} else if (isprint(buf[j+i])) {
				printf("%c", buf[j+i]);
			} else {
				printf(".");
			}
		}
		printf("\n");
	}
}

int save_file(const char *name, void *data, size_t size)
{
	FILE *out = fopen(name, "wb");
	int rc;
	if (!out) {
		perror("Failed to open output file: ");
		exit(1);
	}
	rc = fwrite(data, size, 1, out);
	fclose(out);
	return rc;
}

void *load_file(const char *name, size_t *size)
{
	size_t bufsize = 8192;
	size_t offset = 0;
	char *buf = malloc(bufsize);
	FILE *in;
	if (strcmp(name, "-") == 0)
		in = stdin;
	else
		in = fopen(name, "rb");
	if (!in) {
		perror("Failed to open input file: ");
		exit(1);
	}

	while(1) {
		ssize_t len = bufsize - offset;
		ssize_t n = fread(buf+offset, 1, len, in);
		offset += n;
		if (n < len)
			break;
		bufsize <<= 1;
		buf = realloc(buf, bufsize);
	}
	if (size)
		*size = offset;
	if (in != stdin)
		fclose(in);
	return buf;
}

void aw_fel_hexdump(libusb_device_handle *usb, uint32_t offset, size_t size)
{
	unsigned char buf[size];
	aw_fel_read(usb, offset, buf, size);
	hexdump(buf, offset, size);
}

void aw_fel_dump(libusb_device_handle *usb, uint32_t offset, size_t size)
{
	unsigned char buf[size];
	aw_fel_read(usb, offset, buf, size);
	fwrite(buf, size, 1, stdout);
}
void aw_fel_fill(libusb_device_handle *usb, uint32_t offset, size_t size, unsigned char value)
{
	unsigned char buf[size];
	memset(buf, value, size);
	aw_fel_write(usb, buf, offset, size);
}

/*
 * The 'sram_swap_buffers' structure is used to describe information about
 * two buffers in SRAM, the content of which needs to be exchanged before
 * calling the U-Boot SPL code and then exchanged again before returning
 * control back to the FEL code from the BROM.
 */

typedef struct {
	uint32_t buf1; /* BROM buffer */
	uint32_t buf2; /* backup storage location */
	uint32_t size; /* buffer size */
} sram_swap_buffers;

/*
 * Each SoC variant may have its own list of memory buffers to be exchanged
 * and the information about the placement of the thunk code, which handles
 * the transition of execution from the BROM FEL code to the U-Boot SPL and
 * back.
 *
 * Note: the entries in the 'swap_buffers' tables need to be sorted by 'buf1'
 * addresses. And the 'buf1' addresses are the BROM data buffers, while 'buf2'
 * addresses are the intended backup locations.
 */
typedef struct {
	uint32_t           soc_id;       /* ID of the SoC */
	uint32_t           spl_addr;     /* SPL load address */
	uint32_t           scratch_addr; /* A safe place to upload & run code */
	uint32_t           thunk_addr;   /* Address of the thunk code */
	uint32_t           thunk_size;   /* Maximal size of the thunk code */
	uint32_t           needs_l2en;   /* Set the L2EN bit */
	sram_swap_buffers *swap_buffers;
} soc_sram_info;

/*
 * The FEL code from BROM in A10/A13/A20 sets up two stacks for itself. One
 * at 0x2000 (and growing down) for the IRQ handler. And another one at 0x7000
 * (and also growing down) for the regular code. In order to use the whole
 * 32 KiB in the A1/A2 sections of SRAM, we need to temporarily move these
 * stacks elsewhere. And the addresses above 0x7000 are also a bit suspicious,
 * so it might be safer to backup the 0x7000-0x8000 area too. On A10/A13/A20
 * we can use the SRAM section A3 (0x8000) for this purpose.
 */
sram_swap_buffers a10_a13_a20_sram_swap_buffers[] = {
	{ .buf1 = 0x01800, .buf2 = 0x8000, .size = 0x800 },
	{ .buf1 = 0x05C00, .buf2 = 0x8800, .size = 0x8000 - 0x5C00 },
	{ 0 }  /* End of the table */
};

/*
 * A31 is very similar to A10/A13/A20, except that it has no SRAM at 0x8000.
 * So we use the SRAM section at 0x44000 instead. This is the memory, which
 * is normally shared with the OpenRISC core (should we do an extra check to
 * ensure that this core is powered off and can't interfere?).
 */
sram_swap_buffers a31_sram_swap_buffers[] = {
	{ .buf1 = 0x01800, .buf2 = 0x44000, .size = 0x800 },
	{ .buf1 = 0x05C00, .buf2 = 0x44800, .size = 0x8000 - 0x5C00 },
	{ 0 }  /* End of the table */
};

soc_sram_info soc_sram_info_table[] = {
	{
		.soc_id       = 0x1623, /* Allwinner A10 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0xAE00, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
		.needs_l2en   = 1,
	},
	{
		.soc_id       = 0x1625, /* Allwinner A13 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0xAE00, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
		.needs_l2en   = 1,
	},
	{
		.soc_id       = 0x1651, /* Allwinner A20 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0xAE00, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
	},
	{
		.soc_id       = 0x1650, /* Allwinner A23 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = a31_sram_swap_buffers,
	},
	{
		.soc_id       = 0x1633, /* Allwinner A31 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = a31_sram_swap_buffers,
	},
	{
		.soc_id       = 0x1667, /* Allwinner A33 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = a31_sram_swap_buffers,
	},
	{
		.soc_id       = 0x1673, /* Allwinner A83T */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = a31_sram_swap_buffers,
	},
	{
		.soc_id       = 0x1680, /* Allwinner H3 */
		.scratch_addr = 0x2000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = a31_sram_swap_buffers,
	},
	{ 0 } /* End of the table */
};

/*
 * This generic record assumes BROM with similar properties to A10/A13/A20/A31,
 * but no extra SRAM sections beyond 0x8000. It also assumes that the IRQ
 * handler stack usage never exceeds 0x400 bytes.
 *
 * The users may or may not hope that the 0x7000-0x8000 area is also unused
 * by the BROM and re-purpose it for the SPL stack.
 *
 * The size limit for the ".text + .data" sections is ~21 KiB.
 */
sram_swap_buffers generic_sram_swap_buffers[] = {
	{ .buf1 = 0x01C00, .buf2 = 0x5800, .size = 0x400 },
	{ 0 }  /* End of the table */
};

soc_sram_info generic_sram_info = {
	.scratch_addr = 0x2000,
	.thunk_addr   = 0x5680, .thunk_size = 0x180,
	.swap_buffers = generic_sram_swap_buffers,
};

soc_sram_info *aw_fel_get_sram_info(libusb_device_handle *usb)
{
	/* persistent sram_info, retrieves result pointer once and caches it */
	static soc_sram_info *result = NULL;
	if (result == NULL) {
		int i;

		struct aw_fel_version buf;
		aw_fel_get_version(usb, &buf);

		for (i = 0; soc_sram_info_table[i].swap_buffers; i++)
			if (soc_sram_info_table[i].soc_id == buf.soc_id) {
				result = &soc_sram_info_table[i];
				break;
			}

		if (!result) {
			printf("Warning: no 'soc_sram_info' data for your SoC (id=%04X)\n",
			       buf.soc_id);
			result = &generic_sram_info;
		}
	}
	return result;
}

static uint32_t fel_to_spl_thunk[] = {
	#include "fel-to-spl-thunk.h"
};

#define	DRAM_BASE		0x40000000
#define	DRAM_SIZE		0x80000000

void aw_enable_l2_cache(libusb_device_handle *usb, soc_sram_info *sram_info)
{
	uint32_t arm_code[] = {
		htole32(0xee112f30), /* mrc        15, 0, r2, cr1, cr0, {1}  */
		htole32(0xe3822002), /* orr        r2, r2, #2                */
		htole32(0xee012f30), /* mcr        15, 0, r2, cr1, cr0, {1}  */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
}

void aw_get_stackinfo(libusb_device_handle *usb, soc_sram_info *sram_info,
                      uint32_t *sp_irq, uint32_t *sp)
{
	uint32_t results[2] = { 0 };
#if 0
	/* Does not work on Cortex-A8 (needs Virtualization Extensions) */
	uint32_t arm_code[] = {
		htole32(0xe1010300), /* mrs        r0, SP_irq                */
		htole32(0xe58f0004), /* str        r0, [pc, #4]              */
		htole32(0xe58fd004), /* str        sp, [pc, #4]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 0x10, results, 8);
#else
	/* Works everywhere */
	uint32_t arm_code[] = {
		htole32(0xe10f0000), /* mrs        r0, CPSR                  */
		htole32(0xe3c0101f), /* bic        r1, r0, #31               */
		htole32(0xe3811012), /* orr        r1, r1, #18               */
		htole32(0xe121f001), /* msr        CPSR_c, r1                */
		htole32(0xe1a0100d), /* mov        r1, sp                    */
		htole32(0xe121f000), /* msr        CPSR_c, r0                */
		htole32(0xe58f1004), /* str        r1, [pc, #4]              */
		htole32(0xe58fd004), /* str        sp, [pc, #4]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 0x24, results, 8);
#endif
	*sp_irq = le32toh(results[0]);
	*sp     = le32toh(results[1]);
}

uint32_t aw_get_ttbr0(libusb_device_handle *usb, soc_sram_info *sram_info)
{
	uint32_t ttbr0 = 0;
	uint32_t arm_code[] = {
		htole32(0xee122f10), /* mrc        15, 0, r2, cr2, cr0, {0}  */
		htole32(0xe58f2008), /* str        r2, [pc, #8]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 0x14, &ttbr0, sizeof(ttbr0));
	ttbr0 = le32toh(ttbr0);
	return ttbr0;
}

uint32_t aw_get_sctlr(libusb_device_handle *usb, soc_sram_info *sram_info)
{
	uint32_t sctlr = 0;
	uint32_t arm_code[] = {
		htole32(0xee112f10), /* mrc        15, 0, r2, cr1, cr0, {0}  */
		htole32(0xe58f2008), /* str        r2, [pc, #8]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 0x14, &sctlr, sizeof(sctlr));
	sctlr = le32toh(sctlr);
	return sctlr;
}

uint32_t *aw_backup_and_disable_mmu(libusb_device_handle *usb,
                                    soc_sram_info *sram_info)
{
	uint32_t *tt = NULL;
	uint32_t ttbr0 = aw_get_ttbr0(usb, sram_info);
	uint32_t sctlr = aw_get_sctlr(usb, sram_info);
	uint32_t i;

	uint32_t arm_code[] = {
		/* Disable I-cache, MMU and branch prediction */
		htole32(0xee110f10), /* mrc        15, 0, r0, cr1, cr0, {0}  */
		htole32(0xe3c00001), /* bic        r0, r0, #1                */
		htole32(0xe3c00a01), /* bic        r0, r0, #4096             */
		htole32(0xe3c00b02), /* bic        r0, r0, #2048             */
		htole32(0xee010f10), /* mcr        15, 0, r0, cr1, cr0, {0}  */
		/* Return back to FEL */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	if (!(sctlr & 1)) {
		pr_info("MMU is not enabled by BROM\n");
		return NULL;
	}

	if ((sctlr >> 28) & 1) {
		fprintf(stderr, "TEX remap is enabled!\n");
		exit(1);
	}

	if (ttbr0 & 0x3FFF) {
		fprintf(stderr, "Unexpected TTBR0 (%08X)\n", ttbr0);
		exit(1);
	}

	tt = malloc(16 * 1024);
	pr_info("Reading the MMU translation table from 0x%08X\n", ttbr0);
	aw_fel_read(usb, ttbr0, tt, 16 * 1024);
	for (i = 0; i < 4096; i++)
		tt[i] = le32toh(tt[i]);

	/* Basic sanity checks to be sure that this is a valid table */
	for (i = 0; i < 4096; i++) {
		if (((tt[i] >> 1) & 1) != 1 || ((tt[i] >> 18) & 1) != 0) {
			fprintf(stderr, "MMU: not a section descriptor\n");
			exit(1);
		}
		if ((tt[i] >> 20) != i) {
			fprintf(stderr, "MMU: not a direct mapping\n");
			exit(1);
		}
	}

	pr_info("Disabling I-cache, MMU and branch prediction...");
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	pr_info(" done.\n");

	return tt;
}

void aw_restore_and_enable_mmu(libusb_device_handle *usb,
                               soc_sram_info *sram_info,
                               uint32_t *tt)
{
	uint32_t i;
	uint32_t ttbr0 = aw_get_ttbr0(usb, sram_info);

	uint32_t arm_code[] = {
		/* Invalidate I-cache, TLB and BTB */
		htole32(0xe3a00000), /* mov        r0, #0                    */
		htole32(0xee080f17), /* mcr        15, 0, r0, cr8, cr7, {0}  */
		htole32(0xee070f15), /* mcr        15, 0, r0, cr7, cr5, {0}  */
		htole32(0xee070fd5), /* mcr        15, 0, r0, cr7, cr5, {6}  */
		htole32(0xf57ff04f), /* dsb        sy                        */
		htole32(0xf57ff06f), /* isb        sy                        */
		/* Enable I-cache, MMU and branch prediction */
		htole32(0xee110f10), /* mrc        15, 0, r0, cr1, cr0, {0}  */
		htole32(0xe3800001), /* orr        r0, r0, #1                */
		htole32(0xe3800a01), /* orr        r0, r0, #4096             */
		htole32(0xe3800b02), /* orr        r0, r0, #2048             */
		htole32(0xee010f10), /* mcr        15, 0, r0, cr1, cr0, {0}  */
		/* Return back to FEL */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	pr_info("Setting write-combine mapping for DRAM.\n");
	for (i = (DRAM_BASE >> 20); i < ((DRAM_BASE + DRAM_SIZE) >> 20); i++) {
		/* Clear TEXCB bits */
		tt[i] &= ~((7 << 12) | (1 << 3) | (1 << 2));
		/* Set TEXCB to 00100 (Normal uncached mapping) */
		tt[i] |= (1 << 12);
	}

	pr_info("Setting cached mapping for BROM.\n");
	/* Clear TEXCB bits first */
	tt[0xFFF] &= ~((7 << 12) | (1 << 3) | (1 << 2));
	/* Set TEXCB to 00111 (Normal write-back cached mapping) */
	tt[0xFFF] |= (1 << 12) | /* TEX */
		     (1 << 3)  | /* C */
		     (1 << 2);   /* B */

	pr_info("Writing back the MMU translation table.\n");
	for (i = 0; i < 4096; i++)
		tt[i] = htole32(tt[i]);
	aw_fel_write(usb, tt, ttbr0, 16 * 1024);

	pr_info("Enabling I-cache, MMU and branch prediction...");
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	pr_info(" done.\n");

	free(tt);
}

/*
 * Maximum size of SPL, at the same time this is the start offset
 * of the main U-Boot image within u-boot-sunxi-with-spl.bin
 */
#define SPL_LEN_LIMIT 0x8000

void aw_fel_write_and_execute_spl(libusb_device_handle *usb,
				  uint8_t *buf, size_t len)
{
	soc_sram_info *sram_info = aw_fel_get_sram_info(usb);
	sram_swap_buffers *swap_buffers;
	char header_signature[9] = { 0 };
	size_t i, thunk_size;
	uint32_t *thunk_buf;
	uint32_t sp, sp_irq;
	uint32_t spl_checksum, spl_len, spl_len_limit = SPL_LEN_LIMIT;
	uint32_t *buf32 = (uint32_t *)buf;
	uint32_t cur_addr = sram_info->spl_addr;
	uint32_t *tt = NULL;

	if (!sram_info || !sram_info->swap_buffers) {
		fprintf(stderr, "SPL: Unsupported SoC type\n");
		exit(1);
	}

	if (len < 32 || memcmp(buf + 4, "eGON.BT0", 8) != 0) {
		fprintf(stderr, "SPL: eGON header is not found\n");
		exit(1);
	}

	spl_checksum = 2 * le32toh(buf32[3]) - 0x5F0A6C39;
	spl_len = le32toh(buf32[4]);

	if (spl_len > len || (spl_len % 4) != 0) {
		fprintf(stderr, "SPL: bad length in the eGON header\n");
		exit(1);
	}

	len = spl_len;
	for (i = 0; i < len / 4; i++)
		spl_checksum -= le32toh(buf32[i]);

	if (spl_checksum != 0) {
		fprintf(stderr, "SPL: checksum check failed\n");
		exit(1);
	}

	if (sram_info->needs_l2en) {
		pr_info("Enabling the L2 cache\n");
		aw_enable_l2_cache(usb, sram_info);
	}

	aw_get_stackinfo(usb, sram_info, &sp_irq, &sp);
	pr_info("Stack pointers: sp_irq=0x%08X, sp=0x%08X\n", sp_irq, sp);

	tt = aw_backup_and_disable_mmu(usb, sram_info);

	swap_buffers = sram_info->swap_buffers;
	for (i = 0; swap_buffers[i].size; i++) {
		if ((swap_buffers[i].buf2 >= sram_info->spl_addr) &&
		    (swap_buffers[i].buf2 < sram_info->spl_addr + spl_len_limit))
			spl_len_limit = swap_buffers[i].buf2 - sram_info->spl_addr;
		if (len > 0 && cur_addr < swap_buffers[i].buf1) {
			uint32_t tmp = swap_buffers[i].buf1 - cur_addr;
			if (tmp > len)
				tmp = len;
			aw_fel_write(usb, buf, cur_addr, tmp);
			cur_addr += tmp;
			buf += tmp;
			len -= tmp;
		}
		if (len > 0 && cur_addr == swap_buffers[i].buf1) {
			uint32_t tmp = swap_buffers[i].size;
			if (tmp > len)
				tmp = len;
			aw_fel_write(usb, buf, swap_buffers[i].buf2, tmp);
			cur_addr += tmp;
			buf += tmp;
			len -= tmp;
		}
	}

	/* Clarify the SPL size limitations, and bail out if they are not met */
	if (sram_info->thunk_addr < spl_len_limit)
		spl_len_limit = sram_info->thunk_addr;

	if (spl_len > spl_len_limit) {
		fprintf(stderr, "SPL: too large (need %d, have %d)\n",
			(int)spl_len, (int)spl_len_limit);
		exit(1);
	}

	/* Write the remaining part of the SPL */
	if (len > 0)
		aw_fel_write(usb, buf, cur_addr, len);

	thunk_size = sizeof(fel_to_spl_thunk) + sizeof(sram_info->spl_addr) +
		     (i + 1) * sizeof(*swap_buffers);

	if (thunk_size > sram_info->thunk_size) {
		fprintf(stderr, "SPL: bad thunk size (need %d, have %d)\n",
			(int)sizeof(fel_to_spl_thunk), sram_info->thunk_size);
		exit(1);
	}

	thunk_buf = malloc(thunk_size);
	memcpy(thunk_buf, fel_to_spl_thunk, sizeof(fel_to_spl_thunk));
	memcpy(thunk_buf + sizeof(fel_to_spl_thunk) / sizeof(uint32_t),
	       &sram_info->spl_addr, sizeof(sram_info->spl_addr));
	memcpy(thunk_buf + sizeof(fel_to_spl_thunk) / sizeof(uint32_t) + 1,
	       swap_buffers, (i + 1) * sizeof(*swap_buffers));

	for (i = 0; i < thunk_size / sizeof(uint32_t); i++)
		thunk_buf[i] = htole32(thunk_buf[i]);

	pr_info("=> Executing the SPL...");
	aw_fel_write(usb, thunk_buf, sram_info->thunk_addr, thunk_size);
	aw_fel_execute(usb, sram_info->thunk_addr);
	pr_info(" done.\n");

	free(thunk_buf);

	/* TODO: Try to find and fix the bug, which needs this workaround */
	usleep(250000);

	/* Read back the result and check if everything was fine */
	aw_fel_read(usb, sram_info->spl_addr + 4, header_signature, 8);
	if (strcmp(header_signature, "eGON.FEL") != 0) {
		fprintf(stderr, "SPL: failure code '%s'\n",
			header_signature);
		exit(1);
	}

	/* re-enable the MMU if it was enabled by BROM */
	if(tt != NULL)
		aw_restore_and_enable_mmu(usb, sram_info, tt);
}

/*
 * This function tests a given buffer address and length for a valid U-Boot
 * image. Upon success, the image data gets transferred to the default memory
 * address stored within the image header; and the function preserves the
 * U-Boot entry point (offset) and size values.
 */
void aw_fel_write_uboot_image(libusb_device_handle *usb,
		uint8_t *buf, size_t len)
{
	if (len <= HEADER_SIZE)
		return; /* Insufficient size (no actual data), just bail out */

	uint32_t *buf32 = (uint32_t *)buf;

	/* Check for a valid mkimage header */
	int image_type = get_image_type(buf, len);
	if (image_type <= IH_TYPE_INVALID) {
		switch (image_type) {
		case IH_TYPE_INVALID:
			fprintf(stderr, "Invalid U-Boot image: bad size or signature\n");
			break;
		case IH_TYPE_ARCH_MISMATCH:
			fprintf(stderr, "Invalid U-Boot image: wrong architecture\n");
			break;
		default:
			fprintf(stderr, "Invalid U-Boot image: error code %d\n",
				image_type);
		}
		exit(1);
	}
	if (image_type != IH_TYPE_FIRMWARE) {
		fprintf(stderr, "U-Boot image type mismatch: "
			"expected IH_TYPE_FIRMWARE, got %02X\n", image_type);
		exit(1);
	}
	uint32_t data_size = be32toh(buf32[3]); /* Image Data Size */
	uint32_t load_addr = be32toh(buf32[4]); /* Data Load Address */
	if (data_size != len - HEADER_SIZE) {
		fprintf(stderr, "U-Boot image data size mismatch: "
			"expected %zu, got %u\n", len - HEADER_SIZE, data_size);
		exit(1);
	}
	/* TODO: Verify image data integrity using the checksum field ih_dcrc,
	 * available from be32toh(buf32[6])
	 *
	 * However, this requires CRC routines that mimic their U-Boot
	 * counterparts, namely image_check_dcrc() in ${U-BOOT}/common/image.c
	 * and crc_wd() in ${U-BOOT}/lib/crc32.c
	 *
	 * It should be investigated if existing CRC routines in sunxi-tools
	 * could be factored out and reused for this purpose - e.g. calc_crc32()
	 * from nand-part-main.c
	 */

	/* If we get here, we're "good to go" (i.e. actually write the data) */
	pr_info("Writing image \"%.*s\", %u bytes @ 0x%08X.\n",
		IH_NMLEN, buf + HEADER_NAME_OFFSET, data_size, load_addr);

	aw_fel_write(usb, buf + HEADER_SIZE, load_addr, data_size);

	/* keep track of U-Boot memory region in global vars */
	uboot_entry = load_addr;
	uboot_size = data_size;
}

/*
 * This function handles the common part of both "spl" and "uboot" commands.
 */
void aw_fel_process_spl_and_uboot(libusb_device_handle *usb,
		const char *filename)
{
	/* load file into memory buffer */
	size_t size;
	uint8_t *buf = load_file(filename, &size);
	/* write and execute the SPL from the buffer */
	aw_fel_write_and_execute_spl(usb, buf, size);
	/* check for optional main U-Boot binary (and transfer it, if applicable) */
	if (size > SPL_LEN_LIMIT)
		aw_fel_write_uboot_image(usb, buf + SPL_LEN_LIMIT, size - SPL_LEN_LIMIT);
}

/*
 * Test the SPL header for our "sunxi" variant. We want to make sure that
 * we can safely use specific header fields to pass information to U-Boot.
 * In case of a missing signature (e.g. Allwinner boot0) or header version
 * mismatch, this function will return "false". If all seems fine,
 * the result is "true".
 */
#define SPL_SIGNATURE			"SPL" /* marks "sunxi" header */
#define SPL_MIN_VERSION			1 /* minimum required version */
#define SPL_MAX_VERSION			1 /* maximum supported version */
int have_sunxi_spl(libusb_device_handle *usb, uint32_t spl_addr)
{
	uint8_t spl_signature[4];

	aw_fel_read(usb, spl_addr + 0x14,
		&spl_signature, sizeof(spl_signature));

	if (memcmp(spl_signature, SPL_SIGNATURE, 3) != 0)
		return 0; /* signature mismatch, no "sunxi" SPL */

	if (spl_signature[3] < SPL_MIN_VERSION) {
		fprintf(stderr, "sunxi SPL version mismatch: "
			"found 0x%02X < required minimum 0x%02X\n",
			spl_signature[3], SPL_MIN_VERSION);
		fprintf(stderr, "You need to update your U-Boot (mksunxiboot) to a more recent version.\n");
		return 0;
	}
	if (spl_signature[3] > SPL_MAX_VERSION) {
		fprintf(stderr, "sunxi SPL version mismatch: "
			"found 0x%02X > maximum supported 0x%02X\n",
			spl_signature[3], SPL_MAX_VERSION);
		fprintf(stderr, "You need a more recent version of this (sunxi-tools) fel utility.\n");
		return 0;
	}
	return 1; /* sunxi SPL and suitable version */
}

/*
 * Pass information to U-Boot via specialized fields in the SPL header
 * (see "boot_file_head" in ${U-BOOT}/tools/mksunxiboot.c), providing
 * information about the boot script address (DRAM location of boot.scr).
 */
void pass_fel_information(libusb_device_handle *usb, uint32_t script_address)
{
	soc_sram_info *sram_info = aw_fel_get_sram_info(usb);

	/* write something _only_ if we have a suitable SPL header */
	if (have_sunxi_spl(usb, sram_info->spl_addr)) {
		pr_info("Passing boot info via sunxi SPL: script address = 0x%08X\n",
			script_address);
		aw_fel_write(usb, &script_address,
			sram_info->spl_addr + 0x18, sizeof(script_address));
	}
}

static int aw_fel_get_endpoint(libusb_device_handle *usb)
{
	struct libusb_device *dev = libusb_get_device(usb);
	struct libusb_config_descriptor *config;
	int if_idx, set_idx, ep_idx, ret;

	ret = libusb_get_active_config_descriptor(dev, &config);
	if (ret)
		return ret;

	for (if_idx = 0; if_idx < config->bNumInterfaces; if_idx++) {
		const struct libusb_interface *iface = config->interface + if_idx;

		for (set_idx = 0; set_idx < iface->num_altsetting; set_idx++) {
			const struct libusb_interface_descriptor *setting =
				iface->altsetting + set_idx;

			for (ep_idx = 0; ep_idx < setting->bNumEndpoints; ep_idx++) {
				const struct libusb_endpoint_descriptor *ep =
					setting->endpoint + ep_idx;

				// Test for bulk transfer endpoint
				if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
						LIBUSB_TRANSFER_TYPE_BULK)
					continue;

				if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
						LIBUSB_ENDPOINT_IN)
					AW_USB_FEL_BULK_EP_IN = ep->bEndpointAddress;
				else
					AW_USB_FEL_BULK_EP_OUT = ep->bEndpointAddress;
			}
		}
	}

	libusb_free_config_descriptor(config);

	return 0;
}

/* Less reliable than clock_gettime, but does not require linking with -lrt */
static double gettime(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + (double)tv.tv_usec / 1000000.;
}
#ifdef LIBSUNXI
int fel_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	int uboot_autostart = 0; /* flag for "uboot" command = U-Boot autostart */
	int rc;
	libusb_device_handle *handle = NULL;
	libusb_context *ctx = NULL; //a libusb session

	int busnum = -1, devnum = -1;
	int iface_detached = -1;
	rc = libusb_init(&ctx);
	assert(rc == 0);

	if (argc <= 1) {
		printf("Usage: %s [options] command arguments... [command...]\n"
			"	-v, --verbose			Verbose logging\n"
			"	-d, --dev busnum:devnum		Specify the USB device to use\n"
			"	-p, --progress			Show progress bar when transferring large files\n"
			"\n"
			"	spl file			Load and execute U-Boot SPL\n"
			"		If file additionally contains a main U-Boot binary\n"
			"		(u-boot-sunxi-with-spl.bin), this command also transfers that\n"
			"		to memory (default address from image), but won't execute it.\n"
			"\n"
			"	uboot file-with-spl		like \"spl\", but actually starts U-Boot\n"
			"		U-Boot execution will take place when the fel utility exits.\n"
			"		This allows combining \"uboot\" with further \"write\" commands\n"
			"		(to transfer other files needed for the boot).\n"
			"\n"
			"	hex[dump] address length	Dumps memory region in hex\n"
			"	dump address length		Binary memory dump\n"
			"	exe[cute] address		Call function address\n"
			"	read address length file	Write memory contents into file\n"
			"	write address file		Store file contents into memory\n"
			"	ver[sion]			Show BROM version\n"
			"	clear address length		Clear memory\n"
			"	fill address length value	Fill memory\n"
			, argv[0]
		);
	}

	handle = libusb_open_device_with_vid_pid(NULL, 0x1f3a, 0xefe8);
	while (argc > 1) {
		if (argv[1][0] != '-')
			break;

		if (strcmp(argv[1], "--verbose") == 0 ||
		    strcmp(argv[1], "-v") == 0)
			verbose = 1;

		if (strcmp(argv[1], "--progress") == 0 ||
		    strcmp(argv[1], "-p") == 0)
			progress = 1;

		if (strcmp(argv[1], "--dev") == 0 ||
		    strcmp(argv[1], "-d") == 0) {
			char *dev = argv[2];

			busnum = strtoul(dev, &dev, 0);
			devnum = strtoul(dev + 1, NULL, 0);
			argc -= 1;
			argv += 1;
		}

		argc -= 1;
		argv += 1;
	}

	if (busnum >= 0 && devnum >= 0) {
		struct libusb_device_descriptor desc;
		size_t ndevs, i;
		libusb_device **list;
		libusb_device *dev = NULL;

		ndevs = libusb_get_device_list(NULL, &list);
		for (i = 0; i < ndevs; i++) {
			if (libusb_get_bus_number(list[i]) != busnum ||
			    libusb_get_device_address(list[i]) != devnum) {
				if (i == ndevs-1) {
					fprintf(stderr, "ERROR: No USB FEL device at 0x%x:0x%x\n", busnum, devnum);
					exit(1);
				}
				continue;
			}

			libusb_get_device_descriptor(list[i], &desc);
			if (desc.idVendor == 0x1f3a &&
			    desc.idProduct == 0xefe8)
				dev = list[i];
			break;
		}

		if (dev) {
			libusb_ref_device(dev);
			libusb_open(dev, &handle);
		}
		libusb_free_device_list (list, 1);
	} else {
		handle = libusb_open_device_with_vid_pid(NULL, 0x1f3a, 0xefe8);
	}
	if (!handle) {
		switch (errno) {
		case EACCES:
			fprintf(stderr, "ERROR: You don't have permission to access Allwinner USB FEL device\n");
			break;
		default:
			fprintf(stderr, "ERROR: Allwinner USB FEL device not found!\n");
			break;
		}
		exit(1);
	}
	rc = libusb_claim_interface(handle, 0);
#if defined(__linux__)
	if (rc != LIBUSB_SUCCESS) {
		libusb_detach_kernel_driver(handle, 0);
		iface_detached = 0;
		rc = libusb_claim_interface(handle, 0);
	}
#endif
	if (rc != 0)
		fprintf(stderr, "ERROR: lsusb_claim_interface %d\n",rc);
	assert(rc == 0);

	if (aw_fel_get_endpoint(handle)) {
		fprintf(stderr, "ERROR: Failed to get FEL mode endpoint addresses!\n");
		exit(1);
	}

	while (argc > 1 ) {
		int skip = 1;
		if (strncmp(argv[1], "hex", 3) == 0 && argc > 3) {
			aw_fel_hexdump(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strncmp(argv[1], "dump", 4) == 0 && argc > 3) {
			aw_fel_dump(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if ((strncmp(argv[1], "exe", 3) == 0 && argc > 2)
			) {
			aw_fel_execute(handle, strtoul(argv[2], NULL, 0));
			skip=3;
		} else if (strncmp(argv[1], "ver", 3) == 0 && argc > 1) {
			aw_fel_print_version(handle);
			skip=1;
		} else if (strcmp(argv[1], "write") == 0 && argc > 3) {
			double t1, t2;
			size_t size;
			void *buf = load_file(argv[3], &size);
			uint32_t offset = strtoul(argv[2], NULL, 0);
			t1 = gettime();
			aw_fel_write(handle, buf, offset, size);
			t2 = gettime();
			if (t2 > t1)
				pr_info("Written %.1f KB in %.1f sec (speed: %.1f KB/s)\n",
					(double)size / 1000., t2 - t1,
					(double)size / (t2 - t1) / 1000.);
			/*
			 * If we have transferred a script, try to inform U-Boot
			 * about its address.
			 */
			if (get_image_type(buf, size) == IH_TYPE_SCRIPT)
				pass_fel_information(handle, offset);

			free(buf);
			skip=3;
		} else if (strcmp(argv[1], "read") == 0 && argc > 4) {
			size_t size = strtoul(argv[3], NULL, 0);
			void *buf = malloc(size);
			aw_fel_read(handle, strtoul(argv[2], NULL, 0), buf, size);
			save_file(argv[4], buf, size);
			free(buf);
			skip=4;
		} else if (strcmp(argv[1], "clear") == 0 && argc > 2) {
			aw_fel_fill(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), 0);
			skip=3;
		} else if (strcmp(argv[1], "fill") == 0 && argc > 3) {
			aw_fel_fill(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), (unsigned char)strtoul(argv[4], NULL, 0));
			skip=4;
		} else if (strcmp(argv[1], "spl") == 0 && argc > 2) {
			aw_fel_process_spl_and_uboot(handle, argv[2]);
			skip=2;
		} else if (strcmp(argv[1], "uboot") == 0 && argc > 2) {
			aw_fel_process_spl_and_uboot(handle, argv[2]);
			uboot_autostart = (uboot_entry > 0 && uboot_size > 0);
			if (!uboot_autostart)
				printf("Warning: \"uboot\" command failed to detect image! Can't execute U-Boot.\n");
			skip=2;
		} else {
			fprintf(stderr,"Invalid command %s\n", argv[1]);
			exit(1);
		}
		argc-=skip;
		argv+=skip;
	}

	// auto-start U-Boot if requested (by the "uboot" command)
	if (uboot_autostart) {
		pr_info("Starting U-Boot (0x%08X).\n", uboot_entry);
		aw_fel_execute(handle, uboot_entry);
	}

#if defined(__linux__)
	if (iface_detached >= 0)
		libusb_attach_kernel_driver(handle, iface_detached);
#endif

/* Cleanup when finished - added for use in library. See http://www.dreamincode.net/forums/topic/148707-introduction-to-using-libusb-10/ */
	if (handle) {
		rc = libusb_release_interface(handle, 0); //release the claimed interface
		if (rc != 0) {
			fprintf(stderr,"Cannot Release Interface");
			return 1;
		} else {
			libusb_close(handle); //close the device we opened
			libusb_exit(ctx); //needs to be called to end the
		}
	}
	return 0;
}
