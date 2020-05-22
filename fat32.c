//------------------------------------------------------------------------------
// Copyright (c) 2020 Bj�rn Brodtkorb
//
// This software is provided without warranty of any kind. Permission is
// granted, free of charge, to copy and modify this software, if this copyright
// notice is included in all copies of the software.
//------------------------------------------------------------------------------

#include "fat32.h"
#include "disk_interface.h"
#include "board_serial.h"
#include "board_sd_card.h"
#include "dynamic_memory.h"
#include "syscall.h"
#include "cache.h"

#include <stddef.h>


/// Buffer and bitmask used for volume mounting. When a partition on the MSD 
/// contains a valid FAT32 file system, a FAT32 volume is dynamically allocated
/// and added to the linked list with base `volume_base`. The bitmask ensures 
/// a unique volume letter for each volume. 
static struct volume_s* volume_base;
static u32 volume_bitmask;

/// Temporary buffer used in the mounting process, specifically for retrieving
/// the MBR boot sector and BPB sector for FAT32 recognition
static u8 mount_buffer[512];

/// UCS-2 offsets used in long file name (LFN) entries
static const u8 lfn_offsets[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
	
/// Remove
static const char file_size_ext[] = {'k', 'M', 'G'};

/// Microsoft uses lookup tables for an uninitialized volume. This is because
/// the FAT12/16/32 type is dependent on the number of cluster, and the sectors
/// are dependent on the volume size.
///
/// This look-up table does only apply when:
///		- sector size equals 512
///		- the reserved sector count equals 32
///		- number of FAT's equals 2
struct clust_size_s { u32 sector_cnt; u32 clust_size; };
static const struct clust_size_s cluster_size_lut[] = {
	{      66600,	0  },			// Disks up to 32.5 MB	
	{     532480,	1  },			// Disks up to 260 MB	, 0.5k clusters
	{   16777216,	8  },			// Disks up to 8 GB		, 4k clusters
	{   33554432,	16 },			// Disks up to 16 GB	, 8k clusters
	{   67108864,	32 },			// Disks up to 32 GB	, 16k clusters
	{ 0xFFFFFFFF,	64 }			// Disks > 32 GB		, 32k clusters<
};


static void fat_memcpy(const void* src, void* dest, u32 count);
static u8 fat_memcmp(const void* src_1, const void* src_2, u32 count);
static void fat_store32(void* dest, u32 value);
static void fat_store16(void* dest, u16 value);
static u32 fat_load32(const void* src);
static u16 fat_load16(const void* src);
static u8 fat_volume_add(struct volume_s* vol);
static u8 fat_volume_remove(char letter);
static u8 fat_search(const u8* bpb);
static void fat_print_sector(const u8* sector);
static u8 fat_dir_lfn_cmp(const u8* lfn, const char* name, u32 size);
static u8 fat_dir_sfn_cmp(const char* sfn, const char* name, u8 size);
static u8 fat_dir_sfn_crc(const u8* sfn);
static u8 fat_dir_set_index(struct dir_s* dir, u32 index);
static u8 fat_dir_get_next(struct dir_s* dir);
static u8 fat_dir_search(struct dir_s* dir, const char* name, u32 size);
static u8 fat_table_get(struct volume_s* vol, u32 cluster, u32* fat);
static u8 fat_table_set(struct volume_s* vol, u32 cluster, u32 fat_entry);
static u8 fat_read(struct volume_s* vol, u32 lba);
static u8 fat_flush(struct volume_s* vol);
static inline u32 fat_sect_to_clust(struct volume_s* vol, u32 sect);
static inline u32 fat_clust_to_sect(struct volume_s* vol, u32 clust);
static fstatus fat_follow_path(struct dir_s* dir, const char* path, u32 length);
static fstatus fat_get_vol_label(struct volume_s* vol, char* label);
static void fat_print_info(struct info_s* info);
static u8 fat_file_addr_resolve(struct file_s* file);
static fstatus fat_make_entry_chain(struct dir_s* dir, u8 entry_cnt);

//Remove
static void fat_print_table(struct volume_s* vol, u32 sector) {
	fat_read(vol, vol->fat_lba + sector);
	print("\n" ANSI_YELLOW);
	print("FAT: %d\t", sector * 128);
	for (u8 i = 0; i < 128;) {
		u32 curr = fat_load32(vol->buffer + i * 4);
		
		print("%h", (u8)(curr >> 24));
		print("%h", (u8)(curr >> 16));
		print("%h", (u8)(curr >> 8));
		print("%h", (u8)(curr));
		print("   ");
		if ((i++ % 4) == 0) {
			print("\n");
			print("FAT: %d\t", sector * 128 + i);
			
		}
	}
	print("\n" BLUE);
}

/// Copies `count` number of bytes using byte access
static void fat_memcpy(const void* src, void* dest, u32 count) {
	const u8* src_ptr = (const u8 *)src;
	u8* dest_ptr = (u8 *)dest;
	do {
		*dest_ptr++ = *src_ptr++;
	} while (--count);
}

/// Compares two memory blocks with size `count`
static u8 fat_memcmp(const void* src_1, const void* src_2, u32 count) {
	if (count == 0) {
		return 1;
	}
	const u8* src_1_ptr = (const u8 *)src_1;
	const u8* src_2_ptr = (const u8 *)src_2;
	
	do {
		if (*src_1_ptr != *src_2_ptr) {
			return 0;
		}
		src_1_ptr++;
		src_2_ptr++;
	} while (--count);
	
	return 1;
}

/// Store a 32-bit value in LE format
static void fat_store32(void* dest, u32 value) {
	u8* dest_ptr = (u8 *)dest;
	*dest_ptr++ = (u8)value;
	value >>= 8;
	*dest_ptr++ = (u8)value;
	value >>= 8;
	*dest_ptr++ = (u8)value;
	value >>= 8;
	*dest_ptr++ = (u8)value;
}

/// Store a 16-bit value in LE format
static void fat_store16(void* dest, u16 value) {
	u8* dest_ptr = (u8 *)dest;
	*dest_ptr++ = (u8)value;
	value >>= 8;
	*dest_ptr++ = (u8)value;
}

/// Load a 32-bit value from `src` in LE format
static u32 fat_load32(const void* src) {
	u32 value = 0;
	const u8* src_ptr = (const u8 *)src;
	value |= *src_ptr++;
	value |= (*src_ptr++ << 8);
	value |= (*src_ptr++ << 16);
	value |= (*src_ptr++ << 24);
	return value;
}

/// Load a 16-bit value from `src` in LE format
static u16 fat_load16(const void* src) {
	u16 value = 0;
	const u8* src_ptr = (const u8 *)src;
	value |= *src_ptr++;
	value |= (*src_ptr++ << 8);
	return value;
}

/// Remove
static void fat_print_sector(const u8* sector) {
	for (u32 i = 0; i < 512;) {
		print("%c", sector[i]);
		
		if ((i++ % 32) == 0) {
			print("\n");
		}
	}
	print("\n");
}

/// Add a volume to the system volumes and assign a letter to it
static u8 fat_volume_add(struct volume_s* vol) {
	if (volume_base == NULL) {
		volume_base = vol; 
	} else {
		struct volume_s* vol_it = volume_base;
		while (vol_it->next != NULL) {
			vol_it = vol_it->next;
		}
		vol_it->next = vol;
	}
	vol->next = NULL;
	
	// Assign a letter to the volume based on the bitmask
	for (u8 i = 0; i < 32; i++) {
		if ((volume_bitmask & (1 << i)) == 0) {
			volume_bitmask |= (1 << i);
			vol->letter = 'C' + i;
			break;
		}
	}
	return 1;
}


/// Remove a volume from the system volumes. This functions does NOT delete the 
/// memory
static u8 fat_volume_remove(char letter) {
	struct volume_s* curr;
	if (volume_base == NULL) {
		return 0;
	} else if (volume_base->letter == letter) {
		curr = volume_base;
		volume_base = volume_base->next;
	} else {
		struct volume_s* prev = volume_base;
		curr = volume_base->next;
		
		while (curr != NULL) {
			if (curr->letter == letter) {
				break;
			}
			prev = curr;
			curr = curr->next;
		}
		
		if (curr == NULL) return 0;
		prev->next = curr->next;
	}
	
	// Clear the bit set in the bitmask
	u8 bit_pos = curr->letter - 'C';
	volume_bitmask &= ~(1 << bit_pos);
	
	return 1;
}

/// Checks for a valid FAT32 file system on the given partition. The `bpb`should
/// point to a buffer containing the first sector in this parition. 
static u8 fat_search(const u8* bpb) {
	// Check the BPB boot signature
	if (fat_load16(bpb + 510) != 0xAA55) return 0;
	
	// A valid FAT file system will have the "FAT" string in either the FAT16
	// boot sector, or in the FAT32 boot sector. This does NOT indicate the
	// FAT file system fype
	if (!fat_memcmp(bpb + BPB_32_FSTYPE, "FAT", 3)) {
		if (!fat_memcmp(bpb + BPB_16_FSTYPE, "FAT", 3)) {
			return 0;
		}
	}
	
	// A FAT12, FAT16 or FAT32 file system is present. The type is determined 
	// by the count of data cluster.
	u32 root_sectors = ((fat_load16(bpb + BPB_ROOT_ENT_CNT) * 32) + 
		(fat_load16(bpb + BPB_SECTOR_SIZE) - 1)) / 
		(fat_load16(bpb + BPB_SECTOR_SIZE) - 1);
	
	u32 fat_size = (fat_load16(bpb + BPB_FAT_SIZE_16)) ? 
		(u32)fat_load16(bpb + BPB_FAT_SIZE_16) : 
		fat_load32(bpb + BPB_32_FAT_SIZE);
		
	u32 tot_sect = (fat_load16(bpb + BPB_TOT_SECT_16)) ? 
		(u32)fat_load16(bpb + BPB_TOT_SECT_16) :
		fat_load32(bpb + BPB_TOT_SECT_32);
		
	u32 data_sectors = tot_sect - (fat_load16(bpb + BPB_RSVD_CNT) + 
		(bpb[BPB_NUM_FATS] * fat_size) + root_sectors);
		
	u32 data_clusters = data_sectors / bpb[BPB_CLUSTER_SIZE];
	
	// Only FAT32 is supported
	if (data_clusters < 65525) {
		return 0;
	}
	return 1;
}

/// Calculates the SFN checksum based on the 8.3 short file name
static u8 fat_dir_sfn_crc(const u8* sfn) {
	u8 crc = 0;
	u8 count = 11;
	do {
		crc = ((crc & 1) << 7) + (crc >> 1) + *sfn++;
	} while (--count);
	
	return crc;
}

/// Move the directory pointer to entry `index` relative to the directory base. 
static u8 fat_dir_set_index(struct dir_s* dir, u32 index) {
	return 1;
}

/// Move the directory pointer to the next entry
static u8 fat_dir_get_next(struct dir_s* dir) {
	// Update the rw offset to point to the next 32-byte entry
	dir->rw_offset += 32;
	
	// Check for sector overflow
	if (dir->rw_offset >= dir->vol->sector_size) {
		dir->rw_offset -= dir->vol->sector_size;
		dir->sector++;
		
		// Check for cluster overflow
		if (dir->sector >= (fat_clust_to_sect(dir->vol, dir->cluster) +
				dir->vol->cluster_size)) {
			
			// Get the next cluster from the FAT table	
			u32 new_cluster;
			if (!fat_table_get(dir->vol, dir->cluster, &new_cluster)) {
				return 0;
			}
			
			// Check if the FAT table entry is the EOC. The FAT table entry
			// will in these cases be either EOC or date clusters. No need 
			// to check for bad clusters. 
			u32 eoc_value = new_cluster & 0xFFFFFFF;
			if ((eoc_value >= 0xFFFFFF8) && (eoc_value <= 0xFFFFFFF)) {
				return 0;
			}
			
			// Update the sector LBA from the new cluster number
			dir->cluster = new_cluster;
			dir->sector = fat_clust_to_sect(dir->vol, dir->cluster);
		}
	}
	return 1;
}

/// Resolves any overflow on rw_offset, sector and cluster on the given `file`
/// descriptor
static u8 fat_file_addr_resolve(struct file_s* file) {	
	// Check for sector overflow
	if (file->rw_offset >= file->vol->sector_size) {
		file->rw_offset -= file->vol->sector_size;
		file->sector++;
		
		// Check for cluster overflow
		if (file->sector >= (fat_clust_to_sect(file->vol, file->cluster) +
			file->vol->cluster_size)) {
			
			// Get the next cluster from the FAT table
			u32 new_cluster;
			if (!fat_table_get(file->vol, file->cluster, &new_cluster)) {
				return 0;
			}
			
			// Check if the FAT table entry is the EOC
			u32 eoc_value = new_cluster & 0xFFFFFFF;
			if ((eoc_value >= 0xFFFFFF8) && (eoc_value <= 0xFFFFFFF)) {
				print("EOC value: %d\n", eoc_value);
				return 0;
			}
			
			// Update the sector LBA from the cluster number
			file->cluster = new_cluster;
			file->sector = fat_clust_to_sect(file->vol, file->cluster);	 
		}
	}
	return 1;
}

/// Returns the 32-bit FAT entry corresponding with the cluster number
static u8 fat_table_get(struct volume_s* vol, u32 cluster, u32* fat_entry) {
	u32 start_sect = vol->fat_lba + cluster / 128;
	u32 start_off = cluster % 128;
	
	if (!fat_read(vol, start_sect)) {
		return 0;
	}
	*fat_entry = fat_load32(vol->buffer + start_off * 4);
	return 1;
}

// Takes in a cluster and a FAT table entry and updates the cluster FAT table
// entry with this value.
static u8 fat_table_set(struct volume_s* vol, u32 cluster, u32 fat_entry) {
	u32 start_sect = vol->fat_lba + cluster / 128;
	u32 start_offset = cluster % 128;
	
	if (!fat_read(vol, start_sect)) {
		return 0;
	}
	fat_store32(vol->buffer + 4 * start_offset, fat_entry);
	vol->buffer_dirty = 1;
	fat_flush(vol);
	return 1;
}

static u8 fat_get_cluster(struct volume_s* vol, u32* cluster) {
	if (!fat_read(vol, vol->info_lba)) {
		return 0;
	}
	u32 next_free = fat_load32(vol->buffer + INFO_CLUST_NEXT_FREE);
	u32 tot_free = fat_load32(vol->buffer + INFO_CLUST_CNT);
	
	u32 sector = vol->fat_lba + (next_free / 128);
	u32 rw = (next_free % 128) * 4;
	u32 entry;
	u8 match_found = 0;
	while (1) {
		// Load the current sector
		if (!fat_read(vol, sector)) {
			return 0;
		}
		
		// Check if the entry is available
		entry = fat_load32(vol->buffer + rw);
		if ((entry & 0b1111111) == 0) {
			if (match_found) {
				break;
			} else {
				match_found = 1;
				*cluster = (128 * (sector - vol->fat_lba)) + (rw / 4);
				fat_store32(vol->buffer + rw, 0xFFFFFFF);
				vol->buffer_dirty = 1;
			}
		}
		// Resolve the address
		rw += 4;
		if (rw >= vol->sector_size) {
			sector++;
			rw = 0;
		}
	}
	// Now the sector argument is containing the first free cluster and
	// the entry is containing the next free clusters
	if (!fat_read(vol, vol->info_lba)) {
		return 0;
	}	
	fat_store32(vol->buffer + INFO_CLUST_NEXT_FREE, 
		(128 * (sector - vol->fat_lba)) + (rw / 4));
	fat_store32(vol->buffer + INFO_CLUST_CNT, tot_free - 1);
	
	// Write back the buffer to the MSD
	vol->buffer_dirty = 1;
	fat_flush(vol);
}

// Takes in a LBA and update the volumes local buffer if needed. After this 
// function returns, the LBA sector is clean and valid
static u8 fat_read(struct volume_s* vol, u32 lba) {
	
	// Check if the current buffer contains any dirty data, and if this is the
	// case, write it back to the MSD before reading
	if (vol->buffer_lba == lba) {
		// Not nessecary to update buffer
		return 1;
	}
	if (!fat_flush(vol)) {
		return 0;
	}
	if (!disk_read(vol->disk, vol->buffer, lba, 1)) {
		return 0;
	}
	vol->buffer_lba = lba;
	return 1;
}

// Check if the buffer is dirty and cleans it
static u8 fat_flush(struct volume_s* vol) {
	if (vol->buffer_dirty) {
		if (!disk_write(vol->disk, vol->buffer, vol->buffer_lba, 1)) {
			return 0;
		}
		vol->buffer_dirty = 0;
	}
	return 1;
}

// Maps a sector LBA to the relative cluster number and vica verca
static inline u32 fat_sect_to_clust(struct volume_s* vol, u32 sect) {
	return ((sect - vol->data_lba) / vol->cluster_size) + 2;
}

static inline u32 fat_clust_to_sect(struct volume_s* vol, u32 clust) {
	return ((clust - 2) * vol->cluster_size) + vol->data_lba;
}

// Universal compare without caring for capitals vs non-capitals
static u8 fat_dir_sfn_cmp(const char* sfn, const char* name, u8 size) {
	if (size > 8) {
		size = 8;
	}
	do {
		char tmp_char = *name;
		if (tmp_char >= 'a' && tmp_char <= 'z') {
			tmp_char -= 32;
		}
		
		if (tmp_char != *sfn) {
			return 0;
		}
		sfn++;
		name++;
	} while (--size);
	return 1;
}

// Takes in a pointer to a LFN entry and check if it matches the given name
static u8 fat_dir_lfn_cmp(const u8* lfn, const char* name, u32 size) {
	
	// Calculate the name offset
	u8 name_off = 13 * ((lfn[LFN_SEQ] & LFN_SEQ_MSK) - 1);
	
	for (u8 i = 0; i < 13; i++) {
		if (lfn[lfn_offsets[i]] == 0x00 || lfn[lfn_offsets[i]] == 0xff) {
			break;
		}

		if (lfn[lfn_offsets[i]] != name[name_off + i]) {
			return 0;
		}
	}
	return 1;
}

// Takes in a pointer to a directory and tries to find a name match
static u8 fat_dir_search(struct dir_s* dir, const char* name, u32 size) {
	
	// If the directory object is not pointing to the directory start, fix it.
	if (dir->start_sect != dir->sector) {
		dir->sector = dir->start_sect;
		dir->cluster = fat_sect_to_clust(dir->vol, dir->sector);
		dir->rw_offset = 0;
	}
	
	u8 lfn_crc = 0;
	u8 lfn_match;
	u8 match = 0;
	
	while (1) {
		// Update the buffer if needed
		if (!fat_read(dir->vol, dir->sector)) {
			
			return 0;
		}
		u8* curr_buff = dir->vol->buffer;
		
		// Check if the directory is valid
		u32 rw_tmp = dir->rw_offset;
		u8 sfn_tmp = curr_buff[rw_tmp];
		
		if (sfn_tmp == 0x00) {
			break;
		}
		
		// Only allow to compare used folders
		if (!((sfn_tmp == 0x00) || (sfn_tmp == 0x05) || (sfn_tmp == 0xE5))) {
			
			// Check if the entry pointed to by dir is a LFN or a SFN
			if ((curr_buff[rw_tmp + SFN_ATTR] & ATTR_LFN) == ATTR_LFN) {
				
				// If the LFN name does not match the input
				if (!fat_dir_lfn_cmp(curr_buff + rw_tmp, name, size)) {
					lfn_match = 0;
				}
				lfn_crc = curr_buff[rw_tmp + LFN_CRC];
			} else {
				if (lfn_crc && lfn_match) {
					// Update the directory
					if (lfn_crc == fat_dir_sfn_crc(curr_buff + rw_tmp)) {
						match = 1;
					}
				} else {
					// Compare name with 8.3 file name
					if (fat_dir_sfn_cmp((char *)curr_buff + rw_tmp, name, size)) {
						match = 1;
					}
				}
				if (match) {
					// The directory is found, so update the dir pointer
					dir->cluster = (fat_load16(curr_buff + rw_tmp +
						SFN_CLUSTH) << 16) | fat_load16(curr_buff +
						rw_tmp + SFN_CLUSTL);
					dir->sector = fat_clust_to_sect(dir->vol, dir->cluster);
					dir->start_sect = dir->sector;
					dir->size = fat_load32(curr_buff + rw_tmp + SFN_FILE_SIZE);
					dir->rw_offset = 0;

					return 1;
				}
				lfn_match = 1;
				lfn_crc = 0;
			}
		}
		if (!fat_dir_get_next(dir)) {
			return 0;
		}
	}
	return 0;
}

// This function will take in a path and a blank directory object
// The function will follow the path and return the directory object
// to the last found folder. 
static fstatus fat_follow_path(struct dir_s* dir, const char* path, u32 length) {
	
	// Get the right volume from the path
	struct volume_s* vol = volume_get(*path++);
	if (vol == NULL) {
		return FSTATUS_NO_VOLUME;
	}
	// Update the dir info with the root cluster data
	dir->vol = vol;
	dir->sector = vol->root_lba;
	dir->start_sect = vol->root_lba;
	dir->cluster = fat_clust_to_sect(vol, vol->root_lba);
	dir->rw_offset = 0;
	
	// Check for the colon
	if (*path != ':') {
		return FSTATUS_PATH_ERR;
	}
	path++;
	if (*path != '/') {
		return FSTATUS_PATH_ERR;
	}
	
	// These variables with hold the temporarily fragmented name
	const char* frag_ptr;
	u8 frag_size;

	while (1) {
		
		// Interate the path until a backslash is found
		while (*path && (*path != '/')) {
			path++;
		}
		if (*path++ == '\0') break;
		if (*path == '\0') break;
		
		// Path points to the first character in the name fragment
		const char* tmp_ptr = path;
		frag_ptr = path;
		frag_size = 0;
		
		while ((*tmp_ptr != '\0') && (*tmp_ptr != '/')) {
						
			// The current name fragment is a file, However, the name fragment
			// before it has been found. The dir is pointing to that entry. 
			if (*tmp_ptr == '.') {
				return FSTATUS_OK;
			}
			frag_size++;
			tmp_ptr++;
		}
		
		// After this point frag_ptr will point to the first character in
		// the name fragment, and the frag_size will contain the name fragment
		// size.
		
		// Print the directory name we are searching for
		print("Searching for directory: " ANSI_NORMAL);
		print_count(frag_ptr, frag_size);
		print("\n");

		// Start from the first entry and search for the name fragment
		if (!fat_dir_search(dir, frag_ptr, frag_size)) {
			print(ANSI_RED "Directory not fount\n" ANSI_NORMAL);
			return 0;
		} else {
			// Directory found
		}
	}	
	//print(ANSI_RED "Path found\n" ANSI_NORMAL);
	return FSTATUS_OK;
}

// Gets the volume label
// The volume label can be found two different places, in the BPB section or in
// the root directory. For some reason Microsoft tend to store it in the root
// directory. This functions will do the same. 
static fstatus fat_get_vol_label(struct volume_s* vol, char* label) {	
	// Make a dir object pointing to the root directory
	struct dir_s dir;
	dir.sector = vol->root_lba;
	dir.rw_offset = 0;
	dir.cluster = fat_sect_to_clust(vol, dir.sector);
	
	while (1) {
		if (!fat_read(vol, dir.sector)) {
			return FSTATUS_ERROR;
		}
		
		// Check if the attribute is volume label
		u8 attribute = vol->buffer[dir.rw_offset + SFN_ATTR];
		if (attribute & ATTR_VOL_LABEL) {
			
			// Check that it is not a LFN entry
			if ((attribute & ATTR_LFN) != ATTR_LFN) {
				const char* src = (const char *)(vol->buffer + dir.rw_offset);
				for (u8 i = 0; i < 11; i++) {
					// The volume label is padded with spaces
					*label++ = *src++;
				}
				return 1;
			}
		}
		
		// Get the next directory
		if (!fat_dir_get_next(&dir)) {
			return 0;
		}
	}
}

// Remove
static void fat_print_info(struct info_s* info) {
	print(BLUE);
	u32 size = info->size;
	char ext = 0;	
	u8 ext_cnt = 0;
	while (size >= 1000) {
		size /= 1000;
		ext = file_size_ext[ext_cnt++];
	}
	print("%d", size);
	if (ext) {
		print("%c", ext);
	}
	print("B\t");
	u16 time = info->w_time;
	u16 date = info->w_date;
	
	print("%d/%d/%d %d:%d\t", 
		date & 0b11111,
		(date >> 5) & 0b1111,
		((date >> 9) & 0b1111111) + 1980,
		(time >> 11) & 0b11111,
		(time >> 5) & 0b111111);
		
	if (info->attribute & ATTR_DIR) {
		print("DIR\t");
	} else {
		print("\t");
	}
	
	print_count(info->name, info->name_length);
	print("\n");
}

//------------------------------------------------------------------------------
// FAT23 file system API
// This section will implement the file system API exposed to the user
//------------------------------------------------------------------------------

void fat32_thread(void* arg) {
	
	// Configure the hardware
	board_sd_card_config();
	
	// Wait for the SD card to be insterted
	while (!board_sd_card_get_status());
	
	// Try to mount the disk. If this is not working the disk initialize 
	// functions may be ehh...
	disk_mount(DISK_SD_CARD);
	
	for (u8 i = 0; i < 6; i++) {
		print("S: %d c: %d\n", cluster_size_lut[i].clust_size, cluster_size_lut[i].sector_cnt);
	}
	
	struct volume_s* tmp = volume_get('C');
	u32 cluster;
	
	fat_get_cluster(tmp, &cluster);
	fat_table_set(tmp, 33, 0);
	fat_print_table(tmp, 0);

	// Print all the volumes on the system
	print(BLUE "Displaying system volumes:\n");
	struct volume_s* vol = volume_get_first();
	while (vol) {
		for (u8 i = 0; i < 11; i++) {
			if (vol->label[i]) {
				print("%c", vol->label[i]);
			}
		}
		print(" (%c:)\n", vol->letter);
		vol = vol->next;
	}
	print("\n");
	
	
	// List all directories
	struct dir_s dir;
	fat_dir_open(&dir, "C:/alpha/", 0);
	
	struct info_s* info = (struct info_s *)dynamic_memory_new(DRAM_BANK_0,
		sizeof(struct info_s));
	fstatus status;
	print("\nListing directories in: C:/alpha\n");
	do {
		status = fat_dir_read(&dir, info);
		
		if (fat_memcmp(info->name, "uuuuuughh.txt", info->name_length)) {
			//fat_dir_rename(&dir, "dude", 4);
		}
		
		// Print the information
		if (status == FSTATUS_OK) {
			fat_print_info(info);
		}
	} while (status != FSTATUS_EOF);
	print(BLUE "- EOD -\n");

	while (1) {
		
	}
}

// Mounts a physical disk. This will try to mount all the filesystem available. 
// With the standard MBR one physical disk can contain 4 native file system.
u8 disk_mount(disk_e disk) {
	
	// Verify that the MSD are inserted
	if (!disk_get_status(disk)) {
		return 0;
	}
	
	// Initialize the hardware
	if (!disk_initialize(disk)) {
		return 0;
	}
	// Read the MBR sector at LBA address zero
	disk_read(disk, mount_buffer, 0, 1);

	// Check the boot signature in the MBR
	if (fat_load16(mount_buffer + MBR_BOOT_SIG) != MBR_BOOT_SIG_VALUE) {
		return 0;
	}
	
	// The first part will only retrive the partition info. This is to aviod
	// multiple MBR sector memory loads. 
	struct partition_s partitions[4];
	for (u8 i = 0; i < 4; i++) {
		u32 offset = MBR_PARTITION + i * MBR_PARTITION_SIZE;
		partitions[i].lba = fat_load32(mount_buffer + offset + PAR_LBA);
		partitions[i].size = fat_load32(mount_buffer + offset + PAR_SIZE);
		partitions[i].type = mount_buffer[offset + PAR_TYPE];
		partitions[i].status = mount_buffer[offset + PAR_STATUS];
	}
	
	// Check all the valid partitions for a valid file system
	for (u8 i = 0; i < 4; i++) {
		if (partitions[i].lba) {
			disk_read(disk, mount_buffer, partitions[i].lba, 1);
			
			// The BPB structure is now in the mounting buffer
			if (fat_search(mount_buffer)) {
				
				// We have found a valid file system at partition number i.
				// Allocate memory for the volume (more than 1 KiB)
				struct volume_s* vol = (struct volume_s *)
					dynamic_memory_new(DRAM_BANK_0, sizeof(struct volume_s));
				
				// Update FAT32 information
				vol->sector_size = fat_load16(mount_buffer + BPB_SECTOR_SIZE);
				vol->cluster_size = mount_buffer[BPB_CLUSTER_SIZE];
				vol->total_size = fat_load32(mount_buffer + BPB_TOT_SECT_32);
				
				// Update FAT32 offsets that will be used by the driver
				vol->info_lba = partitions[i].lba + fat_load16(mount_buffer +
					BPB_32_FSINFO);
				vol->fat_lba = partitions[i].lba + fat_load16(mount_buffer + 
					BPB_RSVD_CNT);
				vol->data_lba = vol->fat_lba + (fat_load32(mount_buffer + 
					BPB_32_FAT_SIZE) * mount_buffer[BPB_NUM_FATS]);
				
				vol->root_lba = fat_clust_to_sect(vol, fat_load32(mount_buffer +
					BPB_32_ROOT_CLUST));
				vol->disk = disk;
				
				vol->buffer_lba = 0;

				// Add the newly made volume to the list of system volumes
				fat_volume_add(vol);
				
				// Get the volume label
				fat_get_vol_label(vol, vol->label);
			}
		}
	}
	return 1;
}

// This function should be called after a MSD has been unplugged. It will remove
// all the corresponding volumes and delete the memory. 
u8 disk_eject(disk_e disk) {
	struct volume_s* vol = volume_get_first();
	
	while (vol != NULL) {
		if (vol->disk == disk) {
			if (!fat_volume_remove(vol->letter)) {
				return 0;
			}
			dynamic_memory_free(vol);
		}
		vol = vol->next;
	}
	return 1;
}

// Gets the first volume available. Typically used by the kernel or other 
// software to list all the available volumes. 
struct volume_s* volume_get_first(void) {
	return volume_base;
}

// Gets a volume from the linked list base on the volume letter
struct volume_s* volume_get(char letter) {
	
	struct volume_s* vol = volume_base;
	while (vol != NULL) {
		if (vol->letter == letter) {
			return vol;
		}
		vol = vol->next;
	}
	
	return NULL;
}

fstatus volume_set_label(struct volume_s* vol, const char* name, u8 length) {
	// Make a dir object pointing to the root directory
	struct dir_s dir;
	dir.sector = vol->root_lba;
	dir.rw_offset = 0;
	dir.cluster = fat_sect_to_clust(vol, dir.sector);
	
	while (1) {
		if (!fat_read(vol, dir.sector)) {
			return FSTATUS_ERROR;
		}
		
		// Check if the attribute is volume label
		u8 attribute = vol->buffer[dir.rw_offset + SFN_ATTR];
		if (attribute & ATTR_VOL_LABEL) {
			
			// Check that it is not a LFN entry
			if ((attribute & ATTR_LFN) != ATTR_LFN) {
				char* src = (char *)(vol->buffer + dir.rw_offset);
				for (u8 i = 0; i < 11; i++) {
					// The volume label is padded with spaces
					if (i >= length) {
						*src++ = ' ';
					} else {
						*src++ = *name++;
					}
					vol->buffer_dirty = 1;
				}
				// Writes the buffer back to the MSD
				fat_flush(vol);
				return 1;
			}
		}
		
		// Get the next directory
		if (!fat_dir_get_next(&dir)) {
			return 0;
		}
	}
}

// Get the volue label
fstatus volume_get_label(struct volume_s* vol, char* name) {
	return fat_get_vol_label(vol, name);
}

// Formats the volume
fstatus volume_format(struct volume_s* vol) {
	return FSTATUS_OK;
}

// Opens a directory
fstatus fat_dir_open(struct dir_s* dir, const char* path, u16 length) {
	return fat_follow_path(dir, path, length);
}

fstatus fat_dir_close(struct dir_s* dir) {
	// Check if the volume is clean
	if (!fat_flush(dir->vol)) {
		return FSTATUS_ERROR;
	}
	return FSTATUS_OK;
}

fstatus fat_dir_read(struct dir_s* dir, struct info_s* info) {
	u8 lfn_crc = 0;
	u8 name_length = 0;
	while (1) {
		if (!fat_read(dir->vol, dir->sector)) {
			return FSTATUS_ERROR;
		}
		u8* entry_ptr = dir->vol->buffer + dir->rw_offset;
		
		// Check if the entry is in use
		u8 sfn_check = entry_ptr[0];
		
		// Check for the end marker
		if (sfn_check == 0x00) {
			return FSTATUS_EOF;
		}
		if ((sfn_check != 0xE5) && (sfn_check != 0x05)) {
			u8 sfn_attr = entry_ptr[SFN_ATTR];
			
			// We have a used directory, so read the attribute field to check
			// if it belongs to a SFN or a LFN
			if ((sfn_attr & ATTR_LFN) == ATTR_LFN) {
				
				// LFN case
				u8 name_offset = 13 * ((entry_ptr[0] & LFN_SEQ_MSK) - 1);
				for (u8 i = 0; i < 13; i++) {
					u8 tmp_char = entry_ptr[lfn_offsets[i]]; 
					if (!((tmp_char == 0x00) || (tmp_char == 0xFF))) {
						info->name[name_offset + i] = tmp_char;
						name_length++;
					}
				}
				lfn_crc = entry_ptr[LFN_CRC];
			} else {
				if (lfn_crc) {
					// This SFN entry is the last entry in a chain of
					// LFN entries. Return CRC error if the checksum is wrong
					if (lfn_crc != fat_dir_sfn_crc(entry_ptr)) {
						return FSTATUS_ERROR;
					}
					
				} else {
					// Just one SFN entry needs to be read
					for (u8 i = 0; i < 11; i++) {
						info->name[i] = entry_ptr[i];
						name_length++;
					}
				}
				
				// The code should return
				// Here we update the SFN field that is not the name
				info->attribute = entry_ptr[SFN_ATTR];
				info->c_time_tenth = entry_ptr[SFN_CTIME_TH];
				info->c_time = fat_load16(entry_ptr + SFN_CTIME);
				info->c_date = fat_load16(entry_ptr + SFN_CDATE);
				info->w_time = fat_load16(entry_ptr + SFN_WTIME);
				info->w_date = fat_load16(entry_ptr + SFN_WDATE);
				info->a_date = fat_load16(entry_ptr + SFN_ADATE);
				info->size = fat_load32(entry_ptr + SFN_FILE_SIZE);
				info->name_length = name_length;
				
				// Get the next entry
				fat_dir_get_next(dir);
				
				return FSTATUS_OK;
			}
			}
		// Get the next entry
		fat_dir_get_next(dir);
	}
}

// Makes a directory in the specified path
fstatus fat_dir_make(const char* path) {
	return FSTATUS_OK;
}

// Renames a directory
fstatus fat_dir_rename(struct dir_s* dir, const char* name, u8 length) {
	
	// Get the lenght of the name
	u8 name_length = 0;
	for (u8 i = 0; i < length; i++) {
		if ((name[i] == '.') && (i != 0)) {
			break;
		}
		name_length++;
	}
	
	print("Name length: %d\n", name_length);
	u8 entries_req = 1;
	if (name_length > 8) {
		entries_req = (length / 13) - 1;
	}
	
	// Check the number of entries pointed to by dir
	if (!fat_read(dir->vol, dir->sector)) {
		return FSTATUS_ERROR;
	}
	u8 entries_pres = 1;
	if ((dir->vol->buffer[dir->rw_offset + SFN_ATTR] & ATTR_LFN) == ATTR_LFN) {
		entries_pres = (dir->vol->buffer[dir->rw_offset] & LFN_SEQ_MSK) + 1;
	}
	
	if (entries_req > entries_pres) {
		// Allocate a chain of entries
		
	} else {
		// Enough entry are present
	}
	print("Ent req: %d\nEnt pres: %d\n", entries_req, entries_pres);
}

// Opens a file and returns the file object. It takes in a global path.
fstatus fat_file_open(struct file_s* file, const char* path, u16 length) {
	struct dir_s dir;

	fstatus status = fat_follow_path(&dir, path, length);
	if (status != FSTATUS_OK) {
		return status;
	}
	const char* char_ptr = path + length - 1;
	if (*char_ptr == 0x00) return FSTATUS_PATH_ERR;
	if (*char_ptr == '/') {
		char_ptr--;
	}
	u8 frag_size;
	
	while ((*char_ptr != '/') && (*char_ptr != 0x00)) {
		frag_size++;
		char_ptr--;
	}
	if (*char_ptr == 0x00) {
		return FSTATUS_PATH_ERR;
	}
	
	// Try to find the file
	if (!fat_dir_search(&dir, char_ptr + 1, frag_size)) {
		return FSTATUS_PATH_ERR;
	}
	
	// Update whe address of the file
	file->sector = dir.sector;
	file->start_sect = dir.sector;
	file->cluster = dir.cluster;
	file->rw_offset = 0;
	file->vol = dir.vol;
	file->size = dir.size;
	
	print(ANSI_RED "File size: %d\n" ANSI_NORMAL, file->size);
	
	return FSTATUS_OK;
}

// Cleans up a file
fstatus fat_file_close(struct file_s* file) {
	if(!fat_flush(file->vol)) {
		return FSTATUS_ERROR;
	}
	return FSTATUS_OK;
}

// This functions reads a number of bytes from the file pointer. It will return 
// the number of bytes written. If the requested number and returned number 
// differ, the EOF marked has been hit. 
fstatus fat_file_read(struct file_s* file, u8* buffer, u32 count, u32* status) {
	*status = 0;
	u16 sector_size = file->vol->sector_size;
	u32 file_size = file->size;
	
	if (!fat_read(file->vol, file->sector)) {
		return FSTATUS_ERROR;
	}
	while (count--) {
		
		// If the rw offset is too big, call the functions that resolves the
		// address. This means update the sector and the cluster
		if (file->rw_offset >= sector_size) {
			fat_file_addr_resolve(file);
			
			// Update the file system buffer
			if (!fat_read(file->vol, file->sector)) {
				return FSTATUS_ERROR;
			}
		}
		*buffer++ = file->vol->buffer[file->rw_offset++];
		// Updat the offsets
		file->glob_offset++;
		(*status)++;
		
		if (file->glob_offset >= file_size) {
			break;
		}
	}
	return FSTATUS_OK;
}

// Write a number of characters to the location pointed to be file
fstatus fat_file_write(struct file_s* file, const u8* buffer, u32 count) {
	return FSTATUS_OK;
}

// Move the read/write file pointer. The offset specified is from the start
// of the file. 
fstatus fat_file_jump(struct file_s* file, u32 offset) {
	
	// Make the file pointer point to the beginning of the file
	file->cluster = fat_sect_to_clust(file->vol, file->start_sect);
	
	// Get the relative offsets
	u32 sector_offset = offset / file->vol->sector_size;
	u32 cluster_offset = sector_offset / file->vol->cluster_size;
	sector_offset = sector_offset % file->vol->cluster_size;
	
	while (cluster_offset) {
		u32 new_cluster;
		if (!fat_table_get(file->vol, file->cluster, &new_cluster)) {
			return FSTATUS_ERROR;
		}
		// Check if the FAT table entry is the EOC
		u32 eoc_value = new_cluster & 0xFFFFFFF;
		if ((eoc_value >= 0xFFFFFF8) && (eoc_value <= 0xFFFFFFF)) {
			return 0;
		}
		 
		file->cluster = new_cluster;		
		cluster_offset--;
	}
	
	// The base cluster address is determined. Update the sector and rw offset
	// from the relative offsets calulated above. 
	file->sector = fat_clust_to_sect(file->vol, file->cluster) + sector_offset;
	file->rw_offset = offset % file->vol->sector_size;
	file->glob_offset = offset;
	
	return FSTATUS_OK;
}