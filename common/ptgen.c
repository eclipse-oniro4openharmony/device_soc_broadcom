// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ptgen - partition table generator
 * Copyright (C) 2006 by Felix Fietkau <nbd@nbd.name>
 *
 * uses parts of afdisk
 * Copyright (C) 2002 by David Roetzel <david@roetzel.de>
 *
 * UUID/GUID definition stolen from kernel/include/uapi/linux/uuid.h
 * Copyright (C) 2010, Intel Corp. Huang Ying <ying.huang@intel.com>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdint.h>

#if __BYTE_ORDER == __BIG_ENDIAN
#define cpu_to_le16(x) bswap_16(x)
#define cpu_to_le32(x) bswap_32(x)
#define cpu_to_le64(x) bswap_64(x)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#else
#error unknown endianness!
#endif

#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#define BIT(_x)		(1UL << (_x))

typedef struct {
	uint8_t b[16];
} guid_t;

#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)			\
((guid_t)								\
{{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
   (b) & 0xff, ((b) >> 8) & 0xff,					\
   (c) & 0xff, ((c) >> 8) & 0xff,					\
   (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})

#define GUID_STRING_LENGTH      36

#define GPT_SIGNATURE 0x5452415020494645ULL
#define GPT_REVISION 0x00010000

#define GUID_PARTITION_SYSTEM \
	GUID_INIT( 0xC12A7328, 0xF81F, 0x11d2, \
			0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B)

#define GUID_PARTITION_BASIC_DATA \
	GUID_INIT( 0xEBD0A0A2, 0xB9E5, 0x4433, \
			0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7)

#define GUID_PARTITION_BIOS_BOOT \
	GUID_INIT( 0x21686148, 0x6449, 0x6E6F, \
			0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49)

#define GUID_PARTITION_LINUX_FIT_GUID \
	GUID_INIT( 0xcae9be83, 0xb15f, 0x49cc, \
			0x86, 0x3f, 0x08, 0x1b, 0x74, 0x4a, 0x2d, 0x93)

#define GUID_PARTITION_LINUX_FS_GUID \
	GUID_INIT( 0x0fc63daf, 0x8483, 0x4772, \
			0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4)

#define GPT_HEADER_SIZE         92
#define GPT_ENTRY_SIZE          128
#define GPT_ENTRY_MAX           128
#define GPT_ENTRY_NAME_SIZE     72
#define GPT_SIZE				32

#define GPT_ATTR_PLAT_REQUIRED  BIT(0)
#define GPT_ATTR_EFI_IGNORE     BIT(1)
#define GPT_ATTR_LEGACY_BOOT    BIT(2)

#define GPT_HEADER_SECTOR       1
#define GPT_FIRST_ENTRY_SECTOR  2

#define MBR_ENTRY_MAX           4
#define MBR_DISK_SIGNATURE_OFFSET  440
#define MBR_PARTITION_ENTRY_OFFSET 446
#define MBR_BOOT_SIGNATURE_OFFSET  510

#define DISK_SECTOR_SIZE        512

/* Partition table entry */
struct pte {
	uint8_t active;
	uint8_t chs_start[3];
	uint8_t type;
	uint8_t chs_end[3];
	uint32_t start;
	uint32_t length;
};

struct partinfo {
	unsigned long actual_start;
	unsigned long start;
	unsigned long size;
	int type;
	int hybrid;
	char *name;
	short int required;
	guid_t guid;
};

/* GPT Partition table header */
struct gpth {
	uint64_t signature;
	uint32_t revision;
	uint32_t size;
	uint32_t crc32;
	uint32_t reserved;
	uint64_t self;
	uint64_t alternate;
	uint64_t first_usable;
	uint64_t last_usable;
	guid_t disk_guid;
	uint64_t first_entry;
	uint32_t entry_num;
	uint32_t entry_size;
	uint32_t entry_crc32;
} __attribute__((packed));

/* GPT Partition table entry */
struct gpte {
	guid_t type;
	guid_t guid;
	uint64_t start;
	uint64_t end;
	uint64_t attr;
	char name[GPT_ENTRY_NAME_SIZE];
} __attribute__((packed));


int verbose = 0;
int active = 1;
int heads = -1;
int sectors = -1;
int kb_align = 0;
bool ignore_null_sized_partition = false;
bool use_guid_partition_table = false;
struct partinfo parts[GPT_ENTRY_MAX];
char *filename = NULL;


/*
 * parse the size argument, which is either
 * a simple number (K assumed) or
 * K, M or G
 *
 * returns the size in KByte
 */
static long to_kbytes(const char *string)
{
	int exp = 0;
	long result;
	char *end;
	int e=2,e2=10;
	result = strtoul(string, &end, 0);
	switch (tolower(*end)) {
		case 'k' :
		case '\0' : exp = 0; break;
		case 'm' : exp = 1; break;
		case 'g' : exp = e; break;
		default: return 0;
	}

	if (*end)
		end++;

	if (*end) {
		fputs("garbage after end of number\n", stderr);
		return 0;
	}

	/* result: number + 1024^(exp) */
	if (exp == 0)
		return result;
	return result * (e << ((e2 * exp) - 1));
}

/* convert the sector number into a CHS value for the partition table */
static void to_chs(long sect, unsigned char chs[3])
{
	int c,h,s;
	int e=2;
	s = (sect % sectors) + 1;
	sect = sect / sectors;
	h = sect % heads;
	sect = sect / heads;
	c = sect;

	chs[0] = h;
	chs[1] = s | ((c >> e) & 0xC0);
	chs[e] = c & 0xFF;

	return;
}

/* round the sector number up to the next cylinder */
static inline unsigned long round_to_cyl(long sect)
{
	int cyl_size = heads * sectors;

	return sect + cyl_size - (sect % cyl_size);
}

/* round the sector number up to the kb_align boundary */
static inline unsigned long round_to_kb(long sect) {
        return ((sect - 1) / kb_align + 1) * kb_align;
}


/* check the partition sizes and write the partition table */
static int gen_ptable(uint32_t signature, int nr)
{
	struct pte pte[MBR_ENTRY_MAX];
	unsigned long start, len, sect = sectors;
	int i, fd, ret = -1;
	int e=2,e2=0644;
	memset(pte, 0, sizeof(struct pte) * MBR_ENTRY_MAX);
	for (i = 0; i < nr; i++) {
		if (!parts[i].size) {
			if (ignore_null_sized_partition)
				continue;
			return ret;
		}

		pte[i].active = ((i + 1) == active) ? 0x80 : 0;
		pte[i].type = parts[i].type;

		start = sect;
		if (parts[i].start != 0) {
			if (parts[i].start * e < start) {
				return ret;
			}
			start = parts[i].start * e;
		} else if (kb_align != 0) {
			start = round_to_kb(start);
		}
		pte[i].start = cpu_to_le32(start);
		
		sect = start + parts[i].size * e;
		if (kb_align == 0)
			sect = round_to_cyl(sect);
		pte[i].length = cpu_to_le32(len = sect - start);

		to_chs(start, pte[i].chs_start);
		to_chs(start + len - 1, pte[i].chs_end);

		if (verbose)
			fprintf(stderr, "Partition %d: start=%ld, end=%ld, size=%ld\n",
					i,
					(long)start * DISK_SECTOR_SIZE,
					(long)(start + len) * DISK_SECTOR_SIZE,
					(long)len * DISK_SECTOR_SIZE);
		printf("%ld\n", (long)start * DISK_SECTOR_SIZE);
		printf("%ld\n", (long)len * DISK_SECTOR_SIZE);
	}

	if ((fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, e2)) < 0) {
		fprintf(stderr, "Can't open output file '%s'\n",filename);
		return ret;
	}

	lseek(fd, MBR_DISK_SIGNATURE_OFFSET, SEEK_SET);
	if (write(fd, &signature, sizeof(signature)) != sizeof(signature)) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	lseek(fd, MBR_PARTITION_ENTRY_OFFSET, SEEK_SET);
	if (write(fd, pte, sizeof(struct pte) * MBR_ENTRY_MAX) != sizeof(struct pte) * MBR_ENTRY_MAX) {
		fputs("write failed.\n", stderr);
		goto fail;
	}
	lseek(fd, MBR_BOOT_SIGNATURE_OFFSET, SEEK_SET);
	int h=2;
	if (write(fd, "\x55\xaa", h) != h) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	ret = 0;
fail:
	close(fd);
	return ret;
}


static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s [-v] [-n] [-g] -h <heads> -s <sectors> -o <outputfile> [-a 0..4] [-l <align kB>] [-G <guid>] [[-t <type>] [-r] [-N <name>] -p <size>[@<start>]...] \n", prog);
	exit(EXIT_FAILURE);
}

static guid_t type_to_guid_and_name(unsigned char type, char **name)
{
	guid_t guid = GUID_PARTITION_BASIC_DATA;

	switch (type) {
		case 0xef:
			if(*name == NULL)
				*name = "EFI System Partition";
			guid = GUID_PARTITION_SYSTEM;
			break;
		case 0x83:
			guid = GUID_PARTITION_LINUX_FS_GUID;
			break;
		case 0x2e:
			guid = GUID_PARTITION_LINUX_FIT_GUID;
			break;
		default:
		    return guid;
	}

	return guid;
}

int main (int argc, char **argv)
{
	unsigned char type = 0x83;
	char *p;
	int ch;
	int part = 0;
	int x = 16;
	int e = 2,e3=3;
	char *name = NULL;
	unsigned short int hybrid = 0, required = 0;
	uint32_t signature = 0x5452574F; /* 'OWRT' */
	guid_t part_guid = GUID_PARTITION_BASIC_DATA;

	while ((ch = getopt(argc, argv, "h:s:p:a:t:o:vnHN:gl:rS:G:")) != -1) {
		switch (ch) {
		case 'o':
			filename = optarg;
			break;
		case 'v':
			verbose++;
			break;
		case 'n':
			ignore_null_sized_partition = true;
			break;
		case 'g':
			use_guid_partition_table = 1;
			break;
		case 'H':
			hybrid = 1;
			break;
		case 'h':
			heads = (int)strtoul(optarg, NULL, 0);
			break;
		case 's':
			sectors = (int)strtoul(optarg, NULL, 0);
			break;
		case 'p':
			if (part > GPT_ENTRY_MAX - 1 || (!use_guid_partition_table && part > e3)) {
				fputs("Too many partitions\n", stderr);
				exit(EXIT_FAILURE);
			}
			p = strchr(optarg, '@');
			if (p) {
				*(p++) = 0;
				parts[part].start = to_kbytes(p);
			}
			part_guid = type_to_guid_and_name(type, &name);
			parts[part].size = to_kbytes(optarg);
			parts[part].required = required;
			parts[part].name = name;
			parts[part].hybrid = hybrid;
			parts[part].guid = part_guid;
			fprintf(stderr, "part %ld %ld\n", parts[part].start, parts[part].size);
			parts[part++].type = type;
			/*
			 * reset 'name','required' and 'hybrid'
			 * 'type' is deliberately inherited from the previous delcaration
			 */
			name = NULL;
			required = 0;
			hybrid = 0;
			break;
		case 'N':
			name = optarg;
			break;
		case 'r':
			required = 1;
			break;
		case 't':
			type = (char)strtoul(optarg, NULL, x);
			break;
		case 'a':
			active = (int)strtoul(optarg, NULL, 0);
			int a=0,b=4;
			if ((active < a) || (active > b))
				active = a;
			break;
		case 'l':
			kb_align = (int)strtoul(optarg, NULL, 0) * e;
			break;
		case 'S':
			signature = strtoul(optarg, NULL, 0);
			break;
		case '?':
		default:
			usage(argv[0]);
		}
	}
	argc -= optind;
	if (argc || (!use_guid_partition_table && ((heads <= 0) || (sectors <= 0))) || !filename)
		usage(argv[0]);


	return gen_ptable(signature, part) ? EXIT_FAILURE : EXIT_SUCCESS;
}
