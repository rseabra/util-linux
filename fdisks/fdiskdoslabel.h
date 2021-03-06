#ifndef FDISK_DOS_LABEL_H
#define FDISK_DOS_LABEL_H

/*
 * per partition table entry data
 *
 * The four primary partitions have the same sectorbuffer (MBRbuffer)
 * and have NULL ext_pointer.
 * Each logical partition table entry has two pointers, one for the
 * partition and one link to the next one.
 */
struct pte {
	struct partition *part_table;	/* points into sectorbuffer */
	struct partition *ext_pointer;	/* points into sectorbuffer */
	char changed;			/* boolean */
	sector_t offset;	        /* disk sector number */
	unsigned char *sectorbuffer;	/* disk sector contents */
};

extern struct pte ptes[MAXIMUM_PARTS];
extern int dos_compatible_flag;

#define pt_offset(b, n)	((struct partition *)((b) + 0x1be + \
					      (n) * sizeof(struct partition)))

extern int ext_index; /* the prime extended partition */
extern sector_t extended_offset, sector_offset;

static inline void write_part_table_flag(unsigned char *b)
{
	b[510] = 0x55;
	b[511] = 0xaa;
}

/* A valid partition table sector ends in 0x55 0xaa */
static inline unsigned int part_table_flag(unsigned char *b)
{
	return ((unsigned int) b[510]) + (((unsigned int) b[511]) << 8);
}

static inline sector_t get_partition_start(struct pte *pe)
{
	return pe->offset + get_start_sect(pe->part_table);
}

extern void create_doslabel(struct fdisk_context *cxt);
extern void dos_print_mbr_id(struct fdisk_context *cxt);
extern void dos_set_mbr_id(struct fdisk_context *cxt);
extern void dos_delete_partition(int i);
extern int is_dos_partition(int t);
extern void dos_init(struct fdisk_context *cxt);
extern void dos_add_partition(struct fdisk_context *cxt, int n, int sys);
extern void dos_new_partition(struct fdisk_context *cxt);
extern void dos_write_table(struct fdisk_context *cxt);

#endif
