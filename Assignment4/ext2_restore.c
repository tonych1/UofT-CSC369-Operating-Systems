#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>
#include <string.h>

#include "ext2_helper.h"

/**
 * Restores file.
 *
 * @arg1: An ext2 formatted virtual disk
 * @arg2: Absolute path to link of file
 *
 * @return:	Success:				0
 * 			Not fully restored:		ENOENT
 */

extern unsigned char *disk;				// Global pointer to mmap the disk to

extern struct ext2_super_block *sb;		// Pointer to super block
extern struct ext2_group_desc *gt;		// Pointer to group table
extern struct ext2_inode *ind_tbl;		// Pointer to inode table
extern char *blk_bmp;					// Pointer to block bitmap
extern char *ind_bmp;					// Pointer to inode bitmap

// Forward declarations
int restore_dir_entry(struct ext2_dir_entry *entry, bool recursive);
int restore_block(int block);

int main(int argc, char **argv) {
	if(argc != 3) {
		fprintf(stderr, "Usage: %s <image file name> <file path>\n", argv[0]);
		exit(1);
	}
	int fd = open(argv[1], O_RDWR);		// Allow r & w

	// Map disk to memory, allow r & w
	// Share this mapping between processes
	disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(disk == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	// Pointer to super block (number 2)
	sb = (struct ext2_super_block *)(disk + EXT2_BLOCK_SIZE);
	// Pointer to group table (number 3). Only 1 group in assignment, so take first one
	gt = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
	// Pointer to inode table (number 6+)
	ind_tbl = (struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * gt[0].bg_inode_table);
	// Pointer to block bitmap (number 4)
	blk_bmp = disk + EXT2_BLOCK_SIZE * gt[0].bg_block_bitmap;
	// Pointer to inode bitmap (number 5)
	ind_bmp = disk + EXT2_BLOCK_SIZE * gt[0].bg_inode_bitmap;

	// Partition new dir and sub-directories
	int i = strlen(argv[2]) - 1;
	if(argv[2][i] == '/') argv[2][i--] = '\0';		// Eliminate trailing slashes
	while(i >= 0) {									// Move i to last '/'
		if(argv[2][i] != '/') --i;
		else break;
	}
	if(argv[2][0] == '.' && argv[2][1] == '/') {	// Eliminate './' at beginning
		argv[2] += 2;
		i -= 2;
	}

	int parent_dir_inode = EXT2_ROOT_INO - 1;			// Init with root inode
	// New sub-dir not in root, first cd to working directory
	if(i != -1) {
		parent_dir_inode = cd(argv[2], i);

		// Cd fails because path not exist
		if(parent_dir_inode == -ENOENT) {
			fprintf(stderr, "Path not exists\n");
			exit(ENOENT);
		}
	}
	++i;
	
	// Traverse parent dir's dir entry
	// Search for gaps and check if name matches, and try to restore content to this dir entry
	int block = ((struct ext2_inode *)(ind_tbl + parent_dir_inode)) -> i_block[0];
	unsigned char *block_p = disk + block * EXT2_BLOCK_SIZE;		// Start of block
	unsigned char *cur = block_p;								// Search pos
	int cur_dir_size;
	while(1) {
		cur_dir_size = 8 + ((struct ext2_dir_entry *)(cur)) -> name_len;
		cur_dir_size += 3; cur_dir_size >>= 2; cur_dir_size <<= 2;

		// Skip over '.', but not '..'
		if(strncmp(((struct ext2_dir_entry *)cur) -> name, ".", 1) == 0
			&& strncmp(((struct ext2_dir_entry *)cur) -> name, "..", 2) != 0) {}
		else{ 
			// If namelen and reclen doesn't match
			// Then a gap (or last file) is found, and check if name matches
			if(cur_dir_size != ((struct ext2_dir_entry *)(cur)) -> rec_len) {
				struct ext2_dir_entry *gap = (struct ext2_dir_entry *)(cur + cur_dir_size);
				// Check if name matches
				if(strncmp(argv[2] + i, gap -> name, strlen(argv[2]) - i) == 0
					&& strlen(argv[2]) - i == gap -> name_len) {
					// Have found the file to restore
					if(restore_dir_entry(gap, false) == 0) {
						fprintf(stderr, "Failed to recover file\n");
						return ENOENT;
					}
					else {
						// Successfully restored file, restore dir entry context
						struct ext2_dir_entry *prev_ent = (struct ext2_dir_entry *)(cur);
						prev_ent -> rec_len = cur_dir_size;

						// fprintf(stderr, "Successfully recovered file\n");
						return 0;
					}
				}
			}
		}

		cur += cur_dir_size;
		if(cur - block_p >= EXT2_BLOCK_SIZE) {
			// Reached the end of file	
			fprintf(stderr, "Didn't find file\n");
			return ENOENT;
		} 
	}
}

/**
 * Restore the entry
 * 
 * @arg1: A pointer to this entry
 * @arg2: Whether to restore recursively
 * @return: 	Success:	1
 *				Fail:		0
 */
int restore_dir_entry(struct ext2_dir_entry *entry, bool recursive) {
	assert(recursive == 0);

	// If this entry is the first in block, i.e. inode set to 0 during deletion
	// This entry cannot be restored
	if(((unsigned char *)entry - disk) % 1024 == 0) {
		// fprintf(stderr, "Cannot restore first block in entry\n");
		return 0;
	}

	// If the inode bitmap is set, cannot restore block
	int inode = entry -> inode; assert(inode >= 12);
	if(is_available_inode(inode) == 0) {
		// fprintf(stderr, "Inode assigned to other files, cannot restore\n");
		return 0;
	}
	
	// Possible to restore inode, but content of inode might have been overwritten
	// If non-recursive, can't restore folder
	struct ext2_inode *inode_p = ind_tbl + inode - 1;
	if(recursive == 0) {
		if(get_inode_mode(inode) & EXT2_S_IFDIR) {
			// fprintf(stderr, "Can't restore directory\n");
			return 0;
		}
	}

	// Restore every block
	int i;
	for(i = 0; i < 12; ++i) {	// Direct blocks

		int block = (inode_p -> i_block)[i];
		// Value 0 suggests no further block defined, restore ends
		if(block == 0) {
			break;
		}

		// If this block was taken, can't restore
		if(is_available_block(block) == 0) {
			fprintf(stderr, "Block %d was overwritten, can't restore\n", (inode_p -> i_block)[i]);
			return 0;
		}

		if(recursive) {
			// Restore fails as long as one block can't recover
			if(restore_block(block) == 0) {
				fprintf(stderr, "Failed to restore this directory block\n");
				return 0;
			}
		}

		else {
			// Set block bitmap
			--block;
			blk_bmp[block >> 3] |= (1 << (block % 8));
			++block;
			sb -> s_free_blocks_count -= 1;
			gt -> bg_free_blocks_count -= 1;
		}
	}

	// Set inode bitmap
	--inode;
	ind_bmp[inode >> 3] |= (1 << (inode % 8));
	return 1;
}

/**
 * Recursively restore a dir block, called by restore_dir_entry()
 * @arg1: Block number, starting from 0
 * @return:	Success:	1
 * @		Fail:		0
 */
int restore_block(int block) {
	unsigned char *block_p = disk + block * EXT2_BLOCK_SIZE;		// Start of block
	unsigned char *cur = block_p;									// Search pos

	int cur_dir_size, prev_dir_size;
	
	while(1) {
		prev_dir_size = cur_dir_size;
		cur_dir_size = 8 + ((struct ext2_dir_entry *)(cur)) -> name_len;
		cur_dir_size += 3; cur_dir_size >>= 2; cur_dir_size <<= 2;

		// Don't have to restore '.' and '..'
		if(strncmp(((struct ext2_dir_entry *)(cur)) -> name, ".", 1) == 0);
		else if(strncmp(((struct ext2_dir_entry *)(cur)) -> name, ".", 1) == 0);

		else {
			if(restore_dir_entry((struct ext2_dir_entry *)cur, true) == 0) {
				fprintf(stderr, "Failed to restore directory block\n");
				return 0;
			}
		}

		// Update cur to position of next file in this block
		// cur += ((struct ext2_dir_entry *)cur) -> rec_len;
		cur += ((struct ext2_dir_entry *)(cur)) -> rec_len;
        
        // Reached end of block without error, successfully restored this dir block
        if(cur - block_p >= EXT2_BLOCK_SIZE) {
			return 1;
		}
	}
}