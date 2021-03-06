/*
 * Many, many hands.
 * Specific DOS label file  - Davidlohr Bueso <dave@gnu.org>
 */

#include <unistd.h>
#include <ctype.h>

#include "nls.h"
#include "xalloc.h"
#include "randutils.h"
#include "common.h"
#include "fdisk.h"
#include "fdiskdoslabel.h"

#define set_hsc(h,s,c,sector) { \
		s = sector % cxt->geom.sectors + 1;			\
		sector /= cxt->geom.sectors;				\
		h = sector % cxt->geom.heads;				\
		sector /= cxt->geom.heads;				\
		c = sector & 0xff;					\
		s |= (sector >> 2) & 0xc0;				\
	}

#define alignment_required	(grain != cxt->sector_size)

struct pte ptes[MAXIMUM_PARTS];
sector_t extended_offset;
int ext_index;

static int get_nonexisting_partition(struct fdisk_context *cxt, int warn, int max)
{
	int pno = -1;
	int i;
	int dflt = 0;

	for (i = 0; i < max; i++) {
		struct pte *pe = &ptes[i];
		struct partition *p = pe->part_table;

		if (p && is_cleared_partition(p)) {
			if (pno >= 0) {
				dflt = pno + 1;
				goto not_unique;
			}
			pno = i;
		}
	}
	if (pno >= 0) {
		printf(_("Selected partition %d\n"), pno+1);
		return pno;
	}
	printf(_("All primary partitions have been defined already!\n"));
	return -1;

 not_unique:
	return get_partition_dflt(cxt, warn, max, dflt);
}


/* Allocate a buffer and read a partition table sector */
static void read_pte(struct fdisk_context *cxt, int pno, sector_t offset)
{
	struct pte *pe = &ptes[pno];

	pe->offset = offset;
	pe->sectorbuffer = xmalloc(cxt->sector_size);
	read_sector(cxt, offset, pe->sectorbuffer);
	pe->changed = 0;
	pe->part_table = pe->ext_pointer = NULL;
}

static void dos_write_mbr_id(unsigned char *b, unsigned int id)
{
	store4_little_endian(&b[440], id);
}

static unsigned int dos_read_mbr_id(const unsigned char *b)
{
	return read4_little_endian(&b[440]);
}

static void clear_partition(struct partition *p)
{
	if (!p)
		return;
	p->boot_ind = 0;
	p->head = 0;
	p->sector = 0;
	p->cyl = 0;
	p->sys_ind = 0;
	p->end_head = 0;
	p->end_sector = 0;
	p->end_cyl = 0;
	set_start_sect(p,0);
	set_nr_sects(p,0);
}

void dos_init(struct fdisk_context *cxt)
{
	int i;

	disklabel = DOS_LABEL;
	partitions = 4;
	ext_index = 0;
	extended_offset = 0;

	for (i = 0; i < 4; i++) {
		struct pte *pe = &ptes[i];

		pe->part_table = pt_offset(cxt->mbr, i);
		pe->ext_pointer = NULL;
		pe->offset = 0;
		pe->sectorbuffer = cxt->mbr;
		pe->changed = 0;
	}

	warn_geometry(cxt);
	warn_limits(cxt);
	warn_alignment(cxt);
}

static void read_extended(struct fdisk_context *cxt, int ext)
{
	int i;
	struct pte *pex;
	struct partition *p, *q;

	ext_index = ext;
	pex = &ptes[ext];
	pex->ext_pointer = pex->part_table;

	p = pex->part_table;
	if (!get_start_sect(p)) {
		fprintf(stderr,
			_("Bad offset in primary extended partition\n"));
		return;
	}

	while (IS_EXTENDED (p->sys_ind)) {
		struct pte *pe = &ptes[partitions];

		if (partitions >= MAXIMUM_PARTS) {
			/* This is not a Linux restriction, but
			   this program uses arrays of size MAXIMUM_PARTS.
			   Do not try to `improve' this test. */
			struct pte *pre = &ptes[partitions-1];

			fprintf(stderr,
				_("Warning: omitting partitions after #%d.\n"
				  "They will be deleted "
				  "if you save this partition table.\n"),
				partitions);
			clear_partition(pre->ext_pointer);
			pre->changed = 1;
			return;
		}

		read_pte(cxt, partitions, extended_offset + get_start_sect(p));

		if (!extended_offset)
			extended_offset = get_start_sect(p);

		q = p = pt_offset(pe->sectorbuffer, 0);
		for (i = 0; i < 4; i++, p++) if (get_nr_sects(p)) {
			if (IS_EXTENDED (p->sys_ind)) {
				if (pe->ext_pointer)
					fprintf(stderr,
						_("Warning: extra link "
						  "pointer in partition table"
						  " %d\n"), partitions + 1);
				else
					pe->ext_pointer = p;
			} else if (p->sys_ind) {
				if (pe->part_table)
					fprintf(stderr,
						_("Warning: ignoring extra "
						  "data in partition table"
						  " %d\n"), partitions + 1);
				else
					pe->part_table = p;
			}
		}

		/* very strange code here... */
		if (!pe->part_table) {
			if (q != pe->ext_pointer)
				pe->part_table = q;
			else
				pe->part_table = q + 1;
		}
		if (!pe->ext_pointer) {
			if (q != pe->part_table)
				pe->ext_pointer = q;
			else
				pe->ext_pointer = q + 1;
		}

		p = pe->ext_pointer;
		partitions++;
	}

	/* remove empty links */
 remove:
	for (i = 4; i < partitions; i++) {
		struct pte *pe = &ptes[i];

		if (!get_nr_sects(pe->part_table) &&
		    (partitions > 5 || ptes[4].part_table->sys_ind)) {
			printf(_("omitting empty partition (%d)\n"), i+1);
			dos_delete_partition(i);
			goto remove; 	/* numbering changed */
		}
	}
}

void dos_print_mbr_id(struct fdisk_context *cxt)
{
	printf(_("Disk identifier: 0x%08x\n"), dos_read_mbr_id(cxt->mbr));
}

void create_doslabel(struct fdisk_context *cxt)
{
	unsigned int id;

	/* random disk signature */
	random_get_bytes(&id, sizeof(id));

	fprintf(stderr, _("Building a new DOS disklabel with disk identifier 0x%08x.\n"), id);

	dos_init(cxt);
	fdisk_mbr_zeroize(cxt);
	set_all_unchanged();
	set_changed(0);

	/* Generate an MBR ID for this disk */
	dos_write_mbr_id(cxt->mbr, id);

	/* Put MBR signature */
	write_part_table_flag(cxt->mbr);
}

void dos_set_mbr_id(struct fdisk_context *cxt)
{
	unsigned long new_id;
	char *ep;
	char ps[64];

	snprintf(ps, sizeof ps, _("New disk identifier (current 0x%08x): "),
		 dos_read_mbr_id(cxt->mbr));

	if (read_chars(ps) == '\n')
		return;

	new_id = strtoul(line_ptr, &ep, 0);
	if (*ep != '\n')
		return;

	dos_write_mbr_id(cxt->mbr, new_id);
	MBRbuffer_changed = 1;
	dos_print_mbr_id(cxt);
}

void dos_delete_partition(int i)
{
	struct pte *pe = &ptes[i];
	struct partition *p = pe->part_table;
	struct partition *q = pe->ext_pointer;

	/* Note that for the fifth partition (i == 4) we don't actually
	   decrement partitions. */

	if (i < 4) {
		if (IS_EXTENDED (p->sys_ind) && i == ext_index) {
			partitions = 4;
			ptes[ext_index].ext_pointer = NULL;
			extended_offset = 0;
		}
		clear_partition(p);
	} else if (!q->sys_ind && i > 4) {
		/* the last one in the chain - just delete */
		--partitions;
		--i;
		clear_partition(ptes[i].ext_pointer);
		ptes[i].changed = 1;
	} else {
		/* not the last one - further ones will be moved down */
		if (i > 4) {
			/* delete this link in the chain */
			p = ptes[i-1].ext_pointer;
			*p = *q;
			set_start_sect(p, get_start_sect(q));
			set_nr_sects(p, get_nr_sects(q));
			ptes[i-1].changed = 1;
		} else if (partitions > 5) {    /* 5 will be moved to 4 */
			/* the first logical in a longer chain */
			struct pte *pe = &ptes[5];

			if (pe->part_table) /* prevent SEGFAULT */
				set_start_sect(pe->part_table,
					       get_partition_start(pe) -
					       extended_offset);
			pe->offset = extended_offset;
			pe->changed = 1;
		}

		if (partitions > 5) {
			partitions--;
			while (i < partitions) {
				ptes[i] = ptes[i+1];
				i++;
			}
		} else
			/* the only logical: clear only */
			clear_partition(ptes[i].part_table);
	}
}

static int dos_probe_label(struct fdisk_context *cxt)
{
	int i;

	if (!valid_part_table_flag(cxt->mbr))
		return 0;

	dos_init(cxt);

	for (i = 0; i < 4; i++) {
		struct pte *pe = &ptes[i];

		if (IS_EXTENDED (pe->part_table->sys_ind)) {
			if (partitions != 4)
				fprintf(stderr, _("Ignoring extra extended "
					"partition %d\n"), i + 1);
			else
				read_extended(cxt, i);
		}
	}

	for (i = 3; i < partitions; i++) {
		struct pte *pe = &ptes[i];

		if (!valid_part_table_flag(pe->sectorbuffer)) {
			fprintf(stderr,
				_("Warning: invalid flag 0x%04x of partition "
				"table %d will be corrected by w(rite)\n"),
				part_table_flag(pe->sectorbuffer), i + 1);
			pe->changed = 1;
		}
	}

	return 1;
}

/*
 * Avoid warning about DOS partitions when no DOS partition was changed.
 * Here a heuristic "is probably dos partition".
 * We might also do the opposite and warn in all cases except
 * for "is probably nondos partition".
 */
int is_dos_partition(int t)
{
	return (t == 1 || t == 4 || t == 6 ||
		t == 0x0b || t == 0x0c || t == 0x0e ||
		t == 0x11 || t == 0x12 || t == 0x14 || t == 0x16 ||
		t == 0x1b || t == 0x1c || t == 0x1e || t == 0x24 ||
		t == 0xc1 || t == 0xc4 || t == 0xc6);
}

static void set_partition(struct fdisk_context *cxt,
			  int i, int doext, sector_t start,
			  sector_t stop, int sysid)
{
	struct partition *p;
	sector_t offset;

	if (doext) {
		p = ptes[i].ext_pointer;
		offset = extended_offset;
	} else {
		p = ptes[i].part_table;
		offset = ptes[i].offset;
	}
	p->boot_ind = 0;
	p->sys_ind = sysid;
	set_start_sect(p, start - offset);
	set_nr_sects(p, stop - start + 1);

	if (!doext)
		print_partition_size(cxt, i + 1, start, stop, sysid);

	if (dos_compatible_flag && (start/(cxt->geom.sectors*cxt->geom.heads) > 1023))
		start = cxt->geom.heads*cxt->geom.sectors*1024 - 1;
	set_hsc(p->head, p->sector, p->cyl, start);
	if (dos_compatible_flag && (stop/(cxt->geom.sectors*cxt->geom.heads) > 1023))
		stop = cxt->geom.heads*cxt->geom.sectors*1024 - 1;
	set_hsc(p->end_head, p->end_sector, p->end_cyl, stop);
	ptes[i].changed = 1;
}

static sector_t get_unused_start(int part_n, sector_t start,
				 sector_t first[], sector_t last[])
{
	int i;

	for (i = 0; i < partitions; i++) {
		sector_t lastplusoff;

		if (start == ptes[i].offset)
			start += sector_offset;
		lastplusoff = last[i] + ((part_n < 4) ? 0 : sector_offset);
		if (start >= first[i] && start <= lastplusoff)
			start = lastplusoff + 1;
	}

	return start;
}

static sector_t align_lba_in_range(struct fdisk_context *cxt,
				   sector_t lba, sector_t start, sector_t stop)
{
	start = align_lba(cxt, start, ALIGN_UP);
	stop = align_lba(cxt, stop, ALIGN_DOWN);

	lba = align_lba(cxt, lba, ALIGN_NEAREST);

	if (lba < start)
		return start;
	else if (lba > stop)
		return stop;
	return lba;
}

void dos_add_partition(struct fdisk_context *cxt, int n, int sys)
{
	char mesg[256];		/* 48 does not suffice in Japanese */
	int i, read = 0;
	struct partition *p = ptes[n].part_table;
	struct partition *q = ptes[ext_index].part_table;
	sector_t start, stop = 0, limit, temp,
		first[partitions], last[partitions];

	if (p && p->sys_ind) {
		printf(_("Partition %d is already defined.  Delete "
			 "it before re-adding it.\n"), n + 1);
		return;
	}
	fill_bounds(first, last);
	if (n < 4) {
		start = sector_offset;
		if (display_in_cyl_units || !cxt->total_sectors)
			limit = cxt->geom.heads * cxt->geom.sectors * cxt->geom.cylinders - 1;
		else
			limit = cxt->total_sectors - 1;

		if (limit > UINT_MAX)
			limit = UINT_MAX;

		if (extended_offset) {
			first[ext_index] = extended_offset;
			last[ext_index] = get_start_sect(q) +
				get_nr_sects(q) - 1;
		}
	} else {
		start = extended_offset + sector_offset;
		limit = get_start_sect(q) + get_nr_sects(q) - 1;
	}
	if (display_in_cyl_units)
		for (i = 0; i < partitions; i++)
			first[i] = (cround(first[i]) - 1) * units_per_sector;

	snprintf(mesg, sizeof(mesg), _("First %s"), str_units(SINGULAR));
	do {
		sector_t dflt, aligned;

		temp = start;
		dflt = start = get_unused_start(n, start, first, last);

		/* the default sector should be aligned and unused */
		do {
			aligned = align_lba_in_range(cxt, dflt, dflt, limit);
			dflt = get_unused_start(n, aligned, first, last);
		} while (dflt != aligned && dflt > aligned && dflt < limit);

		if (dflt >= limit)
			dflt = start;
		if (start > limit)
			break;
		if (start >= temp+units_per_sector && read) {
			printf(_("Sector %llu is already allocated\n"), temp);
			temp = start;
			read = 0;
		}
		if (!read && start == temp) {
			sector_t i = start;

			start = read_int(cxt, cround(i), cround(dflt), cround(limit),
					 0, mesg);
			if (display_in_cyl_units) {
				start = (start - 1) * units_per_sector;
				if (start < i) start = i;
			}
			read = 1;
		}
	} while (start != temp || !read);
	if (n > 4) {			/* NOT for fifth partition */
		struct pte *pe = &ptes[n];

		pe->offset = start - sector_offset;
		if (pe->offset == extended_offset) { /* must be corrected */
			pe->offset++;
			if (sector_offset == 1)
				start++;
		}
	}

	for (i = 0; i < partitions; i++) {
		struct pte *pe = &ptes[i];

		if (start < pe->offset && limit >= pe->offset)
			limit = pe->offset - 1;
		if (start < first[i] && limit >= first[i])
			limit = first[i] - 1;
	}
	if (start > limit) {
		printf(_("No free sectors available\n"));
		if (n > 4)
			partitions--;
		return;
	}
	if (cround(start) == cround(limit)) {
		stop = limit;
	} else {
		int is_suffix_used = 0;

		snprintf(mesg, sizeof(mesg),
			_("Last %1$s, +%2$s or +size{K,M,G}"),
			 str_units(SINGULAR), str_units(PLURAL));

		stop = read_int_with_suffix(cxt,
					    cround(start), cround(limit), cround(limit),
					    cround(start), mesg, &is_suffix_used);
		if (display_in_cyl_units) {
			stop = stop * units_per_sector - 1;
			if (stop >limit)
				stop = limit;
		}

		if (is_suffix_used && alignment_required) {
			/* the last sector has not been exactly requested (but
			 * defined by +size{K,M,G} convention), so be smart
			 * and align the end of the partition. The next
			 * partition will start at phy.block boundary.
			 */
			stop = align_lba_in_range(cxt, stop, start, limit) - 1;
			if (stop > limit)
				stop = limit;
		}
	}

	set_partition(cxt, n, 0, start, stop, sys);
	if (n > 4)
		set_partition(cxt, n - 1, 1, ptes[n].offset, stop, EXTENDED);

	if (IS_EXTENDED (sys)) {
		struct pte *pe4 = &ptes[4];
		struct pte *pen = &ptes[n];

		ext_index = n;
		pen->ext_pointer = p;
		pe4->offset = extended_offset = start;
		pe4->sectorbuffer = xcalloc(1, cxt->sector_size);
		pe4->part_table = pt_offset(pe4->sectorbuffer, 0);
		pe4->ext_pointer = pe4->part_table + 1;
		pe4->changed = 1;
		partitions = 5;
	}
}

static void add_logical(struct fdisk_context *cxt)
{
	if (partitions > 5 || ptes[4].part_table->sys_ind) {
		struct pte *pe = &ptes[partitions];

		pe->sectorbuffer = xcalloc(1, cxt->sector_size);
		pe->part_table = pt_offset(pe->sectorbuffer, 0);
		pe->ext_pointer = pe->part_table + 1;
		pe->offset = 0;
		pe->changed = 1;
		partitions++;
	}
	printf(_("Adding logical partition %d\n"), partitions);
	dos_add_partition(cxt, partitions - 1, LINUX_NATIVE);
}

/*
 * Ask the user for new partition type information (logical, extended).
 * This function calls the actual partition adding logic - dos_add_partition.
 */
void dos_new_partition(struct fdisk_context *cxt)
{
	int i, free_primary = 0;

	for (i = 0; i < 4; i++)
		free_primary += !ptes[i].part_table->sys_ind;

	if (!free_primary && partitions >= MAXIMUM_PARTS) {
		printf(_("The maximum number of partitions has been created\n"));
		return;
	}

	if (!free_primary) {
		if (extended_offset) {
			printf(_("All primary partitions are in use\n"));
			add_logical(cxt);
		} else
			printf(_("If you want to create more than four partitions, you must replace a\n"
				 "primary partition with an extended partition first.\n"));
	} else if (partitions >= MAXIMUM_PARTS) {
		printf(_("All logical partitions are in use\n"));
		printf(_("Adding a primary partition\n"));
		dos_add_partition(cxt, get_partition(cxt, 0, 4), LINUX_NATIVE);
	} else {
		char c, dflt, line[LINE_LENGTH];

		dflt = (free_primary == 1 && !extended_offset) ? 'e' : 'p';
		snprintf(line, sizeof(line),
			 _("Partition type:\n"
			   "   p   primary (%d primary, %d extended, %d free)\n"
			   "%s\n"
			   "Select (default %c): "),
			 4 - (extended_offset ? 1 : 0) - free_primary, extended_offset ? 1 : 0, free_primary,
			 extended_offset ? _("   l   logical (numbered from 5)") : _("   e   extended"),
			 dflt);

		c = tolower(read_chars(line));
		if (c == '\n') {
			c = dflt;
			printf(_("Using default response %c\n"), c);
		}
		if (c == 'p') {
			int i = get_nonexisting_partition(cxt, 0, 4);
			if (i >= 0)
				dos_add_partition(cxt, i, LINUX_NATIVE);
			return;
		} else if (c == 'l' && extended_offset) {
			add_logical(cxt);
			return;
		} else if (c == 'e' && !extended_offset) {
			int i = get_nonexisting_partition(cxt, 0, 4);
			if (i >= 0)
				dos_add_partition(cxt, i, EXTENDED);
			return;
		} else
			printf(_("Invalid partition type `%c'\n"), c);
	}
}

void dos_write_table(struct fdisk_context *cxt)
{
	int i;

	/* MBR (primary partitions) */
	if (!MBRbuffer_changed) {
		for (i = 0; i < 4; i++)
			if (ptes[i].changed)
				MBRbuffer_changed = 1;
	}
	if (MBRbuffer_changed) {
		write_part_table_flag(cxt->mbr);
		write_sector(cxt, 0, cxt->mbr);
	}
	/* EBR (logical partitions) */
	for (i = 4; i < partitions; i++) {
		struct pte *pe = &ptes[i];

		if (pe->changed) {
			write_part_table_flag(pe->sectorbuffer);
			write_sector(cxt, pe->offset, pe->sectorbuffer);
		}
	}
}

const struct fdisk_label dos_label =
{
	.name = "dos",
	.probe = dos_probe_label,
};
