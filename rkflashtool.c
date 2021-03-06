/* rkflashtool - for RockChip based devices.
 *               (RK2808, RK2818, RK2918, RK2928, RK3066, RK3068, RK3126 and RK3188)
 *
 * Copyright (C) 2010-2014 by Ivo van Poorten, Fukaumi Naoki, Guenter Knauf,
 *                            Ulrich Prinz, Steve Wilson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <libusb.h>

/* hack to set binary mode for stdin / stdout on Windows */
#ifdef _WIN32
#include <fcntl.h>
int _CRT_fmode = _O_BINARY;
#endif

#include "version.h"
#include "rkcrc.h"
#include "rkflashtool.h"

#define EP1_READ 0x81
#define EP1_WRITE 0x1

#define RKFT_BLOCKSIZE      0x4000      /* must be multiple of 512 */
#define RKFT_IDB_DATASIZE   0x200
#define RKFT_IDB_BLOCKSIZE  0x210
#define RKFT_IDB_INCR       0x20
#define RKFT_MEM_INCR       0x80
#define RKFT_OFF_INCR       (RKFT_BLOCKSIZE>>9)
#define MAX_PARAM_LENGTH    (128*512-12) /* cf. MAX_LOADER_PARAM in rkloader */
#define SDRAM_BASE_ADDRESS  0x60000000

/*
 * RKFT_CMD_XXXX format
 * 0xAABBCCDD
 * 0xAA 表示Flags
 * 0xBB 表示Lun
 * 0xCC 表示Length
 * 0xDD 表示CDB[0]
 */
#define RKFT_CMD_TESTUNITREADY      0x80000600
#define RKFT_CMD_READFLASHID        0x80000601
#define RKFT_CMD_READFLASHINFO      0x8000061a
#define RKFT_CMD_READCHIPINFO       0x8000061b
#define RKFT_CMD_READEFUSE          0x80000620

#define RKFT_CMD_SETDEVICEINFO      0x00000602
#define RKFT_CMD_ERASESYSTEMDISK    0x00000616
#define RKFT_CMD_SETRESETFLASG      0x0000061e
#define RKFT_CMD_RESETDEVICE        0x000006ff

#define RKFT_CMD_TESTBADBLOCK       0x80000a03
#define RKFT_CMD_READSECTOR         0x80000a04
#define RKFT_CMD_READLBA            0x80000a14
#define RKFT_CMD_READSDRAM          0x80000a17
#define RKFT_CMD_UNKNOWN1           0x80000a21

#define RKFT_CMD_WRITESECTOR        0x00000a05
#define RKFT_CMD_ERASESECTORS       0x00000a06
#define RKFT_CMD_UNKNOWN2           0x00000a0b
#define RKFT_CMD_WRITELBA           0x00000a15
#define RKFT_CMD_WRITESDRAM         0x00000a18
#define RKFT_CMD_EXECUTESDRAM       0x00000a19
#define RKFT_CMD_WRITEEFUSE         0x00000a1f
#define RKFT_CMD_UNKNOWN3           0x00000a22

#define RKFT_CMD_WRITESPARE         0x80001007
#define RKFT_CMD_READSPARE          0x80001008

#define RKFT_CMD_LOWERFORMAT        0x0000001c
#define RKFT_CMD_WRITENKB           0x00000030

#define SETBE16(a, v) do { \
                        ((uint8_t*)a)[1] =  v      & 0xff; \
                        ((uint8_t*)a)[0] = (v>>8 ) & 0xff; \
                      } while(0)

#define SETBE32(a, v) do { \
                        ((uint8_t*)a)[3] =  v      & 0xff; \
                        ((uint8_t*)a)[2] = (v>>8 ) & 0xff; \
                        ((uint8_t*)a)[1] = (v>>16) & 0xff; \
                        ((uint8_t*)a)[0] = (v>>24) & 0xff; \
                      } while(0)

static const struct t_pid {
    const uint16_t pid;
    const char name[8];
} pidtab[] = {
    { 0x281a, "RK2818" },
    { 0x290a, "RK2918" },
    { 0x292a, "RK2928" },
    { 0x292c, "RK3026" },
    { 0x300a, "RK3066" },
    { 0x300b, "RK3168" },
    { 0x301a, "RK3036" },
    { 0x310a, "RK3066B" },
    { 0x310b, "RK3188" },
    { 0x310c, "RK312X" }, // Both RK3126 and RK3128
    { 0x310d, "RK3126" },
    { 0x320a, "RK3288" },
    { 0x320b, "RK322X" }, // Both RK3228 and RK3229
    { 0x330a, "RK3368" },
    { 0x330c, "RK3399" },
    { 0, "" },
};

typedef struct {
    uint32_t flash_size;
    uint16_t block_size;
    uint8_t page_size;
    uint8_t ecc_bits;
    uint8_t access_time;
    uint8_t manufacturer_id;
    uint8_t chip_select;
} nand_info;

static const char* const manufacturer[] = {   /* NAND Manufacturers */
    "Samsung",
    "Toshiba",
    "Hynix",
    "Infineon",
    "Micron",
    "Renesas",
    "Intel",
    "UNKNOWN", /* Reserved */
    "SanDisk",
};
#define MAX_NAND_ID (sizeof manufacturer / sizeof(char *))

#if 0
/* Command Block Wrapper */
struct fsg_bulk_cb_wrap {
	__le32	Signature;		/* Contains 'USBC' */
	u32	Tag;			/* Unique per command id */
	__le32	DataTransferLength;	/* Size of the data */
	u8	Flags;			/* Direction in bit 7 */
	u8	Lun;			/* LUN (normally 0) */
	u8	Length;			/* Of the CDB, <= MAX_COMMAND_SIZE */
	u8	CDB[16];		/* Command Data Block */
};

/* Command Status Wrapper */
struct bulk_cs_wrap {
	__le32	Signature;		/* Should = 'USBS' */
	u32	Tag;			/* Same as original command */
	__le32	Residue;		/* Amount not transferred */
	u8	Status;			/* See below */
};
#endif

#define USB_BULK_CB_WRAP_LEN	31
#define USB_BULK_CS_WRAP_LEN	13

static uint8_t cbw[USB_BULK_CB_WRAP_LEN], csw[USB_BULK_CS_WRAP_LEN], buf[RKFT_BLOCKSIZE];
static uint8_t ibuf[RKFT_IDB_BLOCKSIZE];
static libusb_context *c;
static libusb_device_handle *h = NULL;
static int tmp;
static const char *const strings[2] = { "info", "fatal" };
static void info_and_fatal(const int s, const int cr, char *f, ...) {
    va_list ap;
    va_start(ap,f);
    fprintf(stderr, "%srkflashtool: %s: ", cr ? "\r" : "", strings[s]);
    vfprintf(stderr, f, ap);
    va_end(ap);
    if (s) exit(s);
}

#define info(...)    info_and_fatal(0, 0, __VA_ARGS__)
#define infocr(...)  info_and_fatal(0, 1, __VA_ARGS__)
#define fatal(...)   info_and_fatal(1, 0, __VA_ARGS__)

static void usage(void) {
    fatal("usage:\n"
          "\trkflashtool b [flag]            \treboot device\n"
          "\trkflashtool l <file             \tload DDR init (MASK ROM MODE)\n"
          "\trkflashtool L <file             \tload USB loader (MASK ROM MODE)\n"
          "\trkflashtool v                   \tread chip version\n"
          "\trkflashtool n                   \tread NAND flash info\n"
          "\trkflashtool i offset nsectors >outfile \tread IDBlocks\n"
          "\trkflashtool j offset nsectors <infile  \twrite IDBlocks\n"
          "\trkflashtool m offset nbytes   >outfile \tread SDRAM\n"
          "\trkflashtool M offset nbytes   <infile  \twrite SDRAM\n"
          "\trkflashtool B krnl_addr parm_addr      \texec SDRAM\n"
          "\trkflashtool r partname >outfile \tread flash partition\n"
          "\trkflashtool w partname <infile  \twrite flash partition\n"
          "\trkflashtool r offset nsectors >outfile \tread flash\n"
          "\trkflashtool w offset nsectors <infile  \twrite flash\n"
          "\trkflashtool p >file             \tfetch parameters\n"
          "\trkflashtool P <file             \twrite parameters\n"
          "\trkflashtool e partname          \terase flash (fill with 0xff)\n"
          "\trkflashtool e offset nsectors   \terase flash (fill with 0xff)\n"
         );
}

static void send_exec(uint32_t krnl_addr, uint32_t parm_addr) {
    long int r = random();

    memset(cbw, 0 , 31);
    memcpy(cbw, "USBC", 4);

    if (r)          SETBE32(cbw+4, r);
    if (krnl_addr)  SETBE32(cbw+17, krnl_addr);
    if (parm_addr)  SETBE32(cbw+22, parm_addr);
                    SETBE32(cbw+12, RKFT_CMD_EXECUTESDRAM);

    libusb_bulk_transfer(h, EP1_WRITE, cbw, sizeof(cbw), &tmp, 0);
}

#if 0
usb协议中cbw格式如下
Signature 地址等于结构体首地址
Tag 地址等于结构体首地址 + 4
Flags 地址等于结构体首地址 + 12 ==> cbw[12]
CDB 地址等于结构体首地址 + 15 ==> cbw[15]

struct fsg_bulk_cb_wrap {
	__le32	Signature;		/* Contains 'USBC' */
	u32	Tag;			/* Unique per command id */
	__le32	DataTransferLength;	/* Size of the data */
	u8	Flags;			/* Direction in bit 7 */
	u8	Lun;			/* LUN (normally 0) */
	u8	Length;			/* Of the CDB, <= MAX_COMMAND_SIZE */
	u8	CDB[16];		/* Command Data Block */
};
#endif

/* 发送命令, 对端根据接收到的command, offset, nsectors进行读写操作 */
static void send_cbw(uint32_t command, uint32_t offset, uint16_t nsectors, uint8_t flag)
{
    long int r = random();

	/* 初始化全局cbw变量 <==> Signature */
    memset(cbw, 0 , 31);
    memcpy(cbw, "USBC", 4);

	/* 任意填充cbw[4]- cbw[7] <==> Tag */
    if (r)
		SETBE32(cbw+4, r);

	/* offset : cbw[17] - cbw[20] */
    if (offset)
		SETBE32(cbw+17, offset);

	/* nsectors : cbw[22] - cbw[23] */
    if (nsectors)
		SETBE16(cbw+22, nsectors);

	/* command : cbw[12] - cbw[15] <==> Flags, Lun, Length, CDB[0] */
    if (command)
		SETBE32(cbw+12, command);

	/* set flag for reboot mode */
	if (flag)
		cbw[16] = flag;

	/* dump cbw */
	printf("\nDidrection = 0x%x\n", cbw[12]);
	printf("Length = 0x%x\n", cbw[14]);
	printf("CDB[0] = 0x%x\n", cbw[15]);
	printf("CDB[1] = 0x%x\n", cbw[16]);

	/* 通过usb传输将cbw发送到对端 */
    libusb_bulk_transfer(h, EP1_WRITE, cbw, sizeof(cbw), &tmp, 0);
}

static void send_buf(int length)
{
    libusb_bulk_transfer(h, EP1_WRITE, buf, length, &tmp, 0);
}

/* 接收USB返回的结果 */
static void recv_csw(void)
{
    libusb_bulk_transfer(h, EP1_READ, csw, sizeof(csw), &tmp, 0);
}

static void recv_buf(int length)
{
    libusb_bulk_transfer(h, EP1_READ, buf, length, &tmp, 0);
}

#define FOCUS_ON_NEXT_ARGV do { argc--;argv++; } while(0)

int main(int argc, char **argv)
{
    struct libusb_device_descriptor desc;
    const struct t_pid *ppid = pidtab;
    ssize_t nr;
    int offset = 0, size = 0;
    uint16_t crc16;
    uint8_t flag = 0;
    char action;
    char *partname = NULL;

    info("rkflashtool v%d.%d\n", RKFLASHTOOL_VERSION_MAJOR, RKFLASHTOOL_VERSION_MINOR);

    FOCUS_ON_NEXT_ARGV;

	if (!argc)
		usage();

    action = **argv;

	FOCUS_ON_NEXT_ARGV;

    switch(action) {
    case 'b':
        if (argc > 1)
			usage();
        else if (argc == 1)
            flag = strtoul(argv[0], NULL, 0);
        break;
    case 'l':
    case 'L':
        if (argc) usage();
        break;
    case 'e':
    case 'r':
    case 'w':
        if (argc < 1 || argc > 2)
			usage();
        if (argc == 1) {
            partname = argv[0];
        } else {
            offset = strtoul(argv[0], NULL, 0);
            size   = strtoul(argv[1], NULL, 0);
        }
        break;
    case 'm':
    case 'M':
    case 'B':
    case 'i':
    case 'j':
        if (argc != 2)
			usage();
        offset = strtoul(argv[0], NULL, 0);
        size   = strtoul(argv[1], NULL, 0);
        break;
    case 'n':
    case 'v':
    case 'p':
    case 'P':
        if (argc)
			usage();
        offset = 0;
        size   = 1024;
        break;
    default:
        usage();
    }

    /* Initialize libusb */
    if (libusb_init(&c))
		fatal("cannot init libusb\n");

    libusb_set_debug(c, 3);

    /* Detect connected RockChip device */
    while ( !h && ppid->pid) {
        h = libusb_open_device_with_vid_pid(c, 0x2207, ppid->pid);
        if (h) {
            info("Detected %s...\n", ppid->name);
            break;
        }
        ppid++;
    }
    if (!h)
		fatal("cannot open device\n");

    /* Connect to device */
    if (libusb_kernel_driver_active(h, 0) == 1) {
        info("kernel driver active\n");
        if (!libusb_detach_kernel_driver(h, 0))
            info("driver detached\n");
    }

	/* claim interface */
    if (libusb_claim_interface(h, 0) < 0)
        fatal("cannot claim interface\n");
    info("interface claimed\n");

	/* get device descriptor */
    if (libusb_get_device_descriptor(libusb_get_device(h), &desc) != 0)
        fatal("cannot get device descriptor\n");

	/* oops, in mask rom mode */
    if (desc.bcdUSB == 0x200)
        info("MASK ROM MODE\n");

    switch(action) {
    case 'l':
        info("load DDR init\n");
        crc16 = 0xffff;
        while ((nr = read(STDIN_FILENO, buf, 4096)) == 4096) {
            crc16 = rkcrc16(crc16, buf, nr);
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1137, buf, nr, 0);
        }
        if (nr != -1) {
            crc16 = rkcrc16(crc16, buf, nr);
            buf[nr++] = crc16 >> 8;
            buf[nr++] = crc16 & 0xff;
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1137, buf, nr, 0);
        }
        goto exit;
    case 'L':
        info("load USB loader\n");
        crc16 = 0xffff;
        while ((nr = read(STDIN_FILENO, buf, 4096)) == 4096) {
            crc16 = rkcrc16(crc16, buf, nr);
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1138, buf, nr, 0);
        }
        if (nr != -1) {
            crc16 = rkcrc16(crc16, buf, nr);
            buf[nr++] = crc16 >> 8;
            buf[nr++] = crc16 & 0xff;
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1138, buf, nr, 0);
        }
        goto exit;
    }

    /* Initialize bootloader interface */
    send_cbw(RKFT_CMD_TESTUNITREADY, 0, 0, flag);
    recv_csw();
    usleep(20*1000);

    /*
	 * 如果是读,写,擦除命令
	 * 命令行中必定会带有分区名
	 */
    if (partname)
	{
        info("working with partition: %s\n", partname);

        /*
		 * 发送读LBA命令后得到返回结果,存在全局的buf变量中
		 * 读lda + offset
		 * 当offset = 0时读的是gpt信息
		 */
        offset = 0;
        send_cbw(RKFT_CMD_READLBA, offset, RKFT_OFF_INCR, flag);
        recv_buf(RKFT_BLOCKSIZE);
        recv_csw();

        /* 检查返回的数据长度,超过设定范围报异常 */
        uint32_t *p = (uint32_t*)buf+1;
        size = *p;
        if (size < 0 || size > MAX_PARAM_LENGTH)
          fatal("Bad data length!\n");

        /* 从返回的数据中读出分区信息内容 */
        const char *param = (const char *)&buf[8];
        const char *mtdparts = strstr(param, "mtdparts=");
        if (!mtdparts) {
            info("Error: 'mtdparts' not found in command line.\n");
            goto exit;
        }
		info("%s\n", mtdparts);

        /* 在分区表中找到和命令行传入的分区一致的分区 */
        char partexp[256];
        snprintf(partexp, 256, "(%s)", partname);
        char *par = strstr(mtdparts, partexp);
        if (!par) {
            info("Error: Partition '%s' not found.\n", partname);
            goto exit;
        }

		info("%s\n", par);
        /* Cut string by NULL-ing just before (partition_name) */
        par[0] = '\0';

        /* Search for '@' sign */
        char *arob = strrchr(mtdparts, '@');
        if (!arob) {
            info("Error: Bad syntax in mtdparts.\n");
            goto exit;
        }

        offset = strtoul(arob+1, NULL, 0);
        info("found offset: %#010x\n", offset);

        /* Cut string by NULL-ing just before '@' sign */
        arob[0] = '\0';

        /* Search for '-' sign (if last partition) */
        char *minus = strrchr(mtdparts, '-');
        if (minus) {
            /* Read size from NAND info */
            send_cbw(RKFT_CMD_READFLASHINFO, 0, 0, flag);
            recv_buf(512);
            recv_csw();

            nand_info *nand = (nand_info *) buf;
            size = nand->flash_size - offset;

            info("partition extends up to the end of NAND (size: 0x%08x).\n", size);
            goto action;
        }

        /* Search for ',' sign */
        char *comma = strrchr(mtdparts, ',');
        if (comma) {
            size = strtoul(comma+1, NULL, 0);
            info("found size: %#010x\n", size);
            goto action;
        }

        /* Search for ':' sign (if first partition) */
        char *colon = strrchr(mtdparts, ':');
        if (colon) {
            size = strtoul(colon+1, NULL, 0);
            info("found size: %#010x\n", size);
            goto action;
        }

        /* Error: size not found! */
        info("Error: Bad syntax for partition size.\n");
        goto exit;
    }

action:
    /* Check and execute command */
    switch(action) {
    case 'b':   /* Reboot device */
        info("rebooting device...\n");
        send_cbw(RKFT_CMD_RESETDEVICE, 0, 0, flag);
        recv_csw();
        break;
    case 'r':   /* Read FLASH */
        while (size > 0) {
            infocr("reading mmc at offset 0x%08x", offset);

			/* 读lba + offset, 每次传输RKFT_OFF_INCR */
            send_cbw(RKFT_CMD_READLBA, offset, RKFT_OFF_INCR, flag);
            recv_buf(RKFT_BLOCKSIZE);
            recv_csw();

			/*
			 * 将读到的内容写道标准输出里
			 * 如果在命令行中将标准输出重定向到文件的话
			 * 就相当与将读到的内容写入文件
			 */
            if (write(STDOUT_FILENO, buf, RKFT_BLOCKSIZE) <= 0)
                fatal("Write error! Disk full?\n");

            offset += RKFT_OFF_INCR;
            size   -= RKFT_OFF_INCR;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'w':   /* Write FLASH */
        while (size > 0) {
            infocr("writing flash memory at offset 0x%08x", offset);

			/*
			 * 从标注输入读出内容
			 * 如果在命令行中将标准输入重定向为文件的话
			 * 即相当于将文件内容作为要传输的数据
			 */
            if (read(STDIN_FILENO, buf, RKFT_BLOCKSIZE) <= 0) {
                fprintf(stderr, "... Done!\n");
                info("premature end-of-file reached.\n");
                goto exit;
            }

			/* 写lba + offset, 每次传输RKFT_OFF_INCR */
            send_cbw(RKFT_CMD_WRITELBA, offset, RKFT_OFF_INCR, flag);
            send_buf(RKFT_BLOCKSIZE);
            recv_csw();

            offset += RKFT_OFF_INCR;
            size   -= RKFT_OFF_INCR;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'p':   /* Retreive parameters */
        {
            uint32_t *p = (uint32_t*)buf+1;

            info("reading parameters at offset 0x%08x\n", offset);

            send_cbw(RKFT_CMD_READLBA, offset, RKFT_OFF_INCR, flag);
            recv_buf(RKFT_BLOCKSIZE);
            recv_csw();

            /* Check size */
            size = *p;
            info("size:  0x%08x\n", size);
            if (size < 0 || size > MAX_PARAM_LENGTH)
                fatal("Bad parameter length!\n");

            /* Check CRC */
            uint32_t crc_buf = *(uint32_t *)(buf + 8 + size),
                     crc = 0;
            crc = rkcrc32(crc, buf + 8, size);
            if (crc_buf != crc)
              fatal("bad CRC! (%#x, should be %#x)\n", crc_buf, crc);

			if (write(STDOUT_FILENO, &buf[8], size) <= 0)
                fatal("Write error! Disk full?\n");
        }
        break;
    case 'P':   /* Write parameters */
        {
            /* Header */
            strncpy((char *)buf, "PARM", 4);

            /* Content */
            int sizeRead;
            if ((sizeRead = read(STDIN_FILENO, buf + 8, RKFT_BLOCKSIZE - 8)) < 0) {
                info("read error: %s\n", strerror(errno));
                goto exit;
            }

            /* Length */
            *(uint32_t *)(buf + 4) = sizeRead;

            /* CRC */
            uint32_t crc = 0;
            crc = rkcrc32(crc, buf + 8, sizeRead);
            PUT32LE(buf + 8 + sizeRead, crc);

            /*
             * The parameter file is written at 8 different offsets:
             * 0x0000, 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400, 0x1800, 0x1C00
             */

            for(offset = 0; offset < 0x2000; offset += 0x400) {
                infocr("writing flash memory at offset 0x%08x", offset);
                send_cbw(RKFT_CMD_WRITELBA, offset, RKFT_OFF_INCR, flag);
                send_buf(RKFT_BLOCKSIZE);
                recv_csw();
            }
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'm':   /* Read RAM */
        while (size > 0) {
            int sizeRead = size > RKFT_BLOCKSIZE ? RKFT_BLOCKSIZE : size;
            infocr("reading memory at offset 0x%08x size %x", offset, sizeRead);

            send_cbw(RKFT_CMD_READSDRAM, offset - SDRAM_BASE_ADDRESS, sizeRead, flag);
            recv_buf(sizeRead);
            recv_csw();

            if (write(STDOUT_FILENO, buf, sizeRead) <= 0)
                fatal("Write error! Disk full?\n");

            offset += sizeRead;
            size -= sizeRead;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'M':   /* Write RAM */
        while (size > 0) {
            int sizeRead;
            if ((sizeRead = read(STDIN_FILENO, buf, RKFT_BLOCKSIZE)) <= 0) {
                info("premature end-of-file reached.\n");
                goto exit;
            }
            infocr("writing memory at offset 0x%08x size %x", offset, sizeRead);

            send_cbw(RKFT_CMD_WRITESDRAM, offset - SDRAM_BASE_ADDRESS, sizeRead, flag);
            send_buf(sizeRead);
            recv_csw();

            offset += sizeRead;
            size -= sizeRead;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'B':   /* Exec RAM */
        info("booting kernel...\n");
        send_exec(offset - SDRAM_BASE_ADDRESS, size - SDRAM_BASE_ADDRESS);
        recv_csw();
        break;
    case 'i':   /* Read IDB */
        while (size > 0) {
            int sizeRead = size > RKFT_IDB_INCR ? RKFT_IDB_INCR : size;
            infocr("reading IDB flash memory at offset 0x%08x", offset);

            send_cbw(RKFT_CMD_READSECTOR, offset, sizeRead, flag);
            recv_buf(RKFT_IDB_BLOCKSIZE * sizeRead);
            recv_csw();

            if (write(STDOUT_FILENO, buf, RKFT_IDB_BLOCKSIZE * sizeRead) <= 0)
                fatal("Write error! Disk full?\n");

            offset += sizeRead;
            size -= sizeRead;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'j':   /* write IDB */
        while (size > 0) {
            infocr("writing IDB flash memory at offset 0x%08x", offset);

            memset(ibuf, RKFT_IDB_BLOCKSIZE, 0xff);
            if (read(STDIN_FILENO, ibuf, RKFT_IDB_DATASIZE) <= 0) {
                fprintf(stderr, "... Done!\n");
                info("premature end-of-file reached.\n");
                goto exit;
            }

            send_cbw(RKFT_CMD_WRITESECTOR, offset, 1, flag);
            libusb_bulk_transfer(h, EP1_WRITE, ibuf, RKFT_IDB_BLOCKSIZE, &tmp, 0);
            recv_csw();
            offset += 1;
            size -= 1;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'e':   /* Erase flash */
        memset(buf, 0xff, RKFT_BLOCKSIZE);
        while (size > 0) {
            infocr("erasing flash memory at offset 0x%08x", offset);

            send_cbw(RKFT_CMD_WRITELBA, offset, RKFT_OFF_INCR, flag);
            send_buf(RKFT_BLOCKSIZE);
            recv_csw();

            offset += RKFT_OFF_INCR;
            size   -= RKFT_OFF_INCR;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'v':   /* Read Chip Version */
        send_cbw(RKFT_CMD_READCHIPINFO, 0, 0, flag);
        recv_buf(16);
        recv_csw();

        info("chip version: %c%c%c%c-%c%c%c%c.%c%c.%c%c-%c%c%c%c\n",
            buf[ 3], buf[ 2], buf[ 1], buf[ 0],
            buf[ 7], buf[ 6], buf[ 5], buf[ 4],
            buf[11], buf[10], buf[ 9], buf[ 8],
            buf[15], buf[14], buf[13], buf[12]);
        break;
    case 'n':   /* Read NAND Flash Info */
    {
        send_cbw(RKFT_CMD_READFLASHID, 0, 0, flag);
        recv_buf(5);
        recv_csw();

        info("Flash ID: %02x %02x %02x %02x %02x\n",
            buf[0], buf[1], buf[2], buf[3], buf[4]);

        send_cbw(RKFT_CMD_READFLASHINFO, 0, 0, flag);
        recv_buf(512);
        recv_csw();

        nand_info *nand = (nand_info *) buf;
        uint8_t id = nand->manufacturer_id,
                cs = nand->chip_select;

        info("Flash Info:\n"
             "\tManufacturer: %s (%d)\n"
             "\tFlash Size: %dMB\n"
             "\tBlock Size: %dKB\n"
             "\tPage Size: %dKB\n"
             "\tECC Bits: %d\n"
             "\tAccess Time: %d\n"
             "\tFlash CS:%s%s%s%s\n",

             /* Manufacturer */
             id < MAX_NAND_ID ? manufacturer[id] : "Unknown",
             id,

             nand->flash_size >> 11, /* Flash Size */
             nand->block_size >> 1,  /* Block Size */
             nand->page_size  >> 1,  /* Page Size */
             nand->ecc_bits,         /* ECC Bits */
             nand->access_time,      /* Access Time */

             /* Flash CS */
             cs & 1 ? " <0>" : "",
             cs & 2 ? " <1>" : "",
             cs & 4 ? " <2>" : "",
             cs & 8 ? " <3>" : "");
    }
    default:
        break;
    }

exit:
    /* Disconnect and close all interfaces */

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(c);
    return 0;
}
