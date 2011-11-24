/*  
 * -------------------------------------------------------------
 *  IFM LKM - iPODS Filesystem Manager - Loadable Kernel Module
 * -------------------------------------------------------------
 *  ifm_lkm.c
 * -------------------------------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This is an experimental code that interferes with the way the Linux
 * kernel handles data on a filesystem. Its usage on real applications
 * is not recommended at this stage. Use it at your own risk.
 *
 *
 * See the README file for usage instructions, notes and ChangeLog.
 *
 * -------------------------------------------------------------
 */

// Header files
#include <asm/uaccess.h>	// put_user() 
#include <linux/module.h>	// Needed by all modules
#include <linux/kernel.h>	// Needed for KERN_ALERT
#include <linux/fs.h>		// /dev/ operations
#include <linux/mount.h>	// struct vfsmount
#include <linux/nsproxy.h>	// nsproxy
#include <linux/mnt_namespace.h>
#include <linux/list.h>
#include <linux/buffer_head.h>
#include <linux/jbd.h>
#include <linux/vmalloc.h>
#include <linux/ext3ipods_fs.h>
#include <linux/ext3ipods_jbd.h>

// Global definitions
#define DEVICE_NAME	"ifm"
#define IFM_MIN_READLEN	128
#define	IFM_COMMANDLEN	256

// Module information
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Felipe Franciosi <felipe@paradoxo.org>");
MODULE_DESCRIPTION("iPODS Filesystem Manager LKM");

// Prototypes
static int	device_open(struct inode *, struct file *);
static int	device_release(struct inode *, struct file *);
static ssize_t	device_read(struct file *, char *, size_t, loff_t *);
static ssize_t	device_write(struct file *, const char *, size_t, loff_t *);

// Global variables
static int		major;
static int		is_open = 0;
static int		read_state = 0;
static struct vfsmount	*mnt_ext3ipods;
char			command[IFM_COMMANDLEN];

// File operations
static struct file_operations fops = {
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release
};

// From namespace.c
// Locate the next mounted filesystem from the vfsmount list
static struct vfsmount *next_mnt(struct vfsmount *p, struct vfsmount *root) {
	struct list_head *next = p->mnt_mounts.next;
	if (next == &p->mnt_mounts) {
		while (1) {
			if (p == root)
				return NULL;
			next = p->mnt_child.next;
			if (next != &p->mnt_parent->mnt_mounts)
				break;
			p = p->mnt_parent;
		}
	}
	return list_entry(next, struct vfsmount, mnt_child);
}

// Module initialisation
int init_module(void) {
	// Attempt to register device with dynamic major number
	major = register_chrdev(0, DEVICE_NAME, &fops);

	// Deal with failures
	if (major < 0) {
		printk("Failed to assign %s: %d.\n", DEVICE_NAME, major);
		return major;
	}

	// FIXME: failed attempt to create a 'dev' entry under /sys
	//printk("Registering region: %d\n", register_chrdev_region(MKDEV(major, 0), 1, DEVICE_NAME));

	// Pass major number to user
	printk("%s registered with major: %d\n", DEVICE_NAME, major);

	// Reset command
	memset(command, 0, sizeof(command));

	// Return success on module initialisation
	return 0;
}

// Module cleanup
void cleanup_module(void) {
	// Unregister device
	unregister_chrdev(major, DEVICE_NAME);
	printk(KERN_ALERT "%s unloaded.\n", DEVICE_NAME);
}

// Open device
static int device_open(struct inode *inode, struct file *file) {
	// Local variables
	struct vfsmount	*root;

	// Return an error if device already opened
	if (is_open) {
		return(-EBUSY);
	}
	is_open++;

	// Reset read_state
	read_state = 0;

	// Locate the first ext3ipods mounted filesystem
	// - ifm_lkm only supports one filesystem mounted at a time
	root = current->nsproxy->mnt_ns->root;
	mnt_ext3ipods = root;
	while ((mnt_ext3ipods = next_mnt(mnt_ext3ipods, root))) {
		if (!strcmp(mnt_ext3ipods->mnt_sb->s_type->name, "ext3ipods")) {
			goto fs_located;
		}
	}
	mnt_ext3ipods = NULL;

fs_located:

	// Increase module counter
	try_module_get(THIS_MODULE);

	// Return success
	return(0);
}

// Release device
static int device_release(struct inode *inode, struct file *file)
{
	// Release device
	is_open--;

	//  Decrement the usage count
	module_put(THIS_MODULE);

	// Return success
	return(0);
}

// Auxiliary function that writes to user space
static ssize_t userspace_write(char *ubuf, size_t ubuflen, char **msg, size_t *msglen) {
	// Local variables
	ssize_t	bytes_written = 0;

	// We should only print if the buffer can take the whole message
	if (*msglen < ubuflen) {
		return(0);
	}

	// Write to device
	while (ubuflen && *ubuf) {
		put_user(*ubuf, *msg);
		ubuf++;
		(*msg)++;
		bytes_written++;
		ubuflen--;
	}

	// Update msglen
	*msglen -= bytes_written;

	// Return the number of bytes written to userspace
	return(bytes_written);
}

// Evaluate block quality
// NOTE:0 This is a function called only by "device_read()" while running the "eval" request.
//       Its sole purpose is to compute the delta for each QoS attribute and to update bgq accordingly.
static unsigned int bgq_eval(long unsigned int *bgq, short unsigned int *bgf, long unsigned int inof, struct ext3ipods_sb_info *sbi, unsigned int block) {
	// Local Variables
	int	delta;

//printk("block=%u. inof>>&=%lu. b/c=%lu\n", block, ((inof>>20)&0x03), block/sbi->s_blocks_per_group);

	// Write Performance delta
	delta = ((inof >> 20) & 0x03) - (bgf[block/sbi->s_blocks_per_group] & 0x03);
	bgq[block/sbi->s_blocks_per_group] += (delta>0)?2*delta:-delta;

	// Read Performance delta
	delta = ((inof >> 22) & 0x03) - ((bgf[block/sbi->s_blocks_per_group] >> 2) & 0x03);
	bgq[block/sbi->s_blocks_per_group] += (delta>0)?2*delta:-delta;

	// Reliability delta
	delta = ((inof >> 24) & 0x03) - ((bgf[block/sbi->s_blocks_per_group] >> 4) & 0x03);
	bgq[block/sbi->s_blocks_per_group] += (delta>0)?2*delta:-delta;

//printk("bgq[%lu] = %lu\n", block/sbi->s_blocks_per_group, bgq[block/sbi->s_blocks_per_group]);

	return(0);
}

// Read from device
static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t *offset) {
	// Local variables
	char				msg[IFM_MIN_READLEN];	// Message buffer
	char				*ptr;			// Message buffer pointer
	struct ext3ipods_group_desc	*egd;			// ext3ipods group descriptor
	struct ext3ipods_super_block	*esb;			// ext3ipods super block
	struct ext3ipods_sb_info	*sbi;			// ext3ipods super block info
//	struct ext3ipods_inode		*ino;			// ext3ipods inode
	struct ext3ipods_inode_info	*ei;			// ext3ipods inode info
	loff_t				i_dscopy;		// ei->disksize copy for 64bit div
	unsigned int			i_blocks;		// number of blocks allocd to inode
	long unsigned int		inode_index;		// index of the current inode
	static long unsigned int	block_index = 0;	// Last block index printed
	struct buffer_head		*bhib;			// Buffer head for the inode bitmap
//	struct buffer_head		*bhit;			// Buffer head for the inode table
	struct buffer_head		*bh1i;			// Buffer head for 1-indirect block
	struct buffer_head		*bh2i;			// Buffer head for 2-indirect block
	struct buffer_head		*bh3i;			// Buffer head for 3-indirect block
	struct buffer_head		*bh = NULL;		// Generic buffer head
	char				bitmask;		// Bitmask to test bitmaps
	int 				bytes_read = 0;		// Actual number of bytes read
	int 				bytes_temp = 0;		// Actual number of bytes read (temp acc)
	long unsigned int		ris;			// Requested inode start (get)
	long unsigned int		rgs;			// Requested group start (info)
	unsigned int			blocksize;		// Filesystem block size
	static long unsigned int	*bgq = NULL;		// Block group quality array
	static short unsigned int	*bgf = NULL;		// Block group flags array
	int				i, j, k, l, m, n;	// Temporary integers

	// Check if user buffer has a minimum acceptable size
	// TODO: Check if this could be an issue with terminals that would read a byte at a time
	if (length < IFM_MIN_READLEN) {
		goto out;
	}

	// Check if we have a mounted ext3ipods fs
	if (!mnt_ext3ipods) {
		goto out;
	}

	// Fetch super block info
	sbi = EXT3IPODS_SB(mnt_ext3ipods->mnt_sb);
	esb = sbi->s_es;
	blocksize = esb->s_blocks_per_group/8;

	// Reset msg
	memset(msg, 0, sizeof(msg));

	// Deal with each command accordingly
	if (!strncmp(command, "info", 4)) {
		// Only print header for first dump ("info", with no rgs associated)
		ptr = command+4;
		if (*ptr == 0) {
			snprintf(msg, sizeof(msg), "%d\n%lu\n", esb->s_blocks_per_group, sbi->s_groups_count);
			bytes_read += userspace_write(msg, strlen(msg), &buffer, &length); // We've already checked length>=IFM_MIN_READLEN
		}

		// Fetch requested block group
		for (rgs=0; (*ptr >= '0' && *ptr <= '9'); ptr++) {
			rgs = (10*rgs)+(*ptr-'0');
		}

		// Dump group flags
		for (i=rgs; i<sbi->s_groups_count; i++) {
			egd = ext3ipods_get_group_desc(mnt_ext3ipods->mnt_sb, i, NULL);
			snprintf(msg, sizeof(msg), "%hu\n", egd->bg_flags);
			if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
				sprintf(command, "info%d", i);
				goto out;
			}
			bytes_read += bytes_temp;
		}
		memset(command, 0, sizeof(command));
		goto out;
	}
	else
	if (!strncmp(command, "eval", 4)) {
		// Fetch requested block group
		ptr = command+4;
		for (rgs=0; (*ptr >= '0' && *ptr <= '9'); ptr++) {
			rgs = (10*rgs)+(*ptr-'0');
		}
		if (rgs > sbi->s_groups_count) {
			// Invalidate request
			memset(command, 0, sizeof(command));
			if (bgq) { vfree(bgq); bgq = NULL; }
			if (bgf) { vfree(bgf); bgf = NULL; }
			goto out;
		}

		// If rgs=0, re-evaluate the entire filesystem
		if (rgs == 0) {
			// Reset vars
//			bhib = bhit = NULL;
			bhib = NULL;
			memset(command, 0, sizeof(command));
			if (bgq) { vfree(bgq); bgq = NULL; }
			if (bgf) { vfree(bgf); bgf = NULL; }

			// Allocate bgq and bgf
			bgq = vmalloc(sizeof(long unsigned int)*sbi->s_groups_count);
			if (!bgq) { goto out; }
			bgf = vmalloc(sizeof(short unsigned int)*sbi->s_groups_count);
			if (!bgf) { vfree(bgq); bgq = NULL; goto out; }

			// Reset bgq and read bgf
			for (i=0; i<sbi->s_groups_count; i++) {
				egd = ext3ipods_get_group_desc(mnt_ext3ipods->mnt_sb, i, NULL);
				bgq[i] = 0;
				bgf[i] = egd->bg_flags;
			}

			// Run every inode and get their flags
			for (i=0; i<sbi->s_groups_count; i++) {
				// Load block group descriptor and inode bitmap
				egd = ext3ipods_get_group_desc(mnt_ext3ipods->mnt_sb, i, NULL);
				bhib = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(egd->bg_inode_bitmap));

				// Parse all inodes in each block group
				for (j=0; j<(sbi->s_inodes_per_group); j++) {
					// Compute a bitmask for every byte of the bitvector
					bitmask = 0x01 << (j%8);

					// Calculate the proper inode index
					inode_index = (i*sbi->s_inodes_per_group) + j + 1;

					// Skip reserved inodes
					if (inode_index < sbi->s_first_ino) {
						continue;
					}

					// Look for "ones" in the bitvector
					if ((bhib->b_data[j/8] & bitmask)) {
						// Fetch the corresponding ext3ipods_inode_info
						ei = EXT3IPODS_I(ext3ipods_iget(mnt_ext3ipods->mnt_sb, inode_index));
						// Load the block where the inode is and point 'ino' to the proper offset in the block
//						bhit = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(egd->bg_inode_table)+(j/sbi->s_inodes_per_block));
//						ino = (struct ext3ipods_inode *)((bhit->b_data)+((j%sbi->s_inodes_per_block)*EXT3IPODS_GOOD_OLD_INODE_SIZE));
//						ei = EXT3IPODS_I(ino);
						i_dscopy = ei->i_disksize-1;
						do_div(i_dscopy, blocksize);
						i_blocks = (ei->i_disksize > 0)?i_dscopy+1:0;

						// Run every block in this inode
						for (k=0; k<i_blocks; k++) {
							if (k<EXT3IPODS_NDIR_BLOCKS) {
								// Direct addressing
								bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(ei->i_data[k]));
							} else {
								// 1-Indirect addressing
								bh1i = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(ei->i_data[EXT3IPODS_IND_BLOCK]));
								bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(ei->i_data[EXT3IPODS_IND_BLOCK]));
								for (l=0, k++; (l<(blocksize/4)) && (k<i_blocks); l++, k++) {
									bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(*(unsigned int *)&(bh1i->b_data[l*4])));
								}
								brelse(bh1i);
								i_dscopy = ei->i_disksize;
								do_div(i_dscopy, blocksize);
								if (k >= i_dscopy+1) { break; }
								// 2-Indirect addressing
								bh1i = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(ei->i_data[EXT3IPODS_DIND_BLOCK]));
								bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(ei->i_data[EXT3IPODS_DIND_BLOCK]));
								for (l=0, k++; (l<(blocksize/4)) && (k<i_blocks); l++) {
									bh2i = sb_bread(mnt_ext3ipods->mnt_sb, *(unsigned int *)&(bh1i->b_data[l*4]));
									bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(*(unsigned int *)&(bh1i->b_data[l*4])));
									for (m=0, k++; (m<(blocksize/4)) && (k<i_blocks) ; m++, k++) {
										bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(*(unsigned int *)&(bh2i->b_data[m*4])));
									}
									brelse(bh2i);
								}
								brelse(bh1i);
								if (k >= i_blocks) { break; }
								// 3-Indirect addressing
								bh1i = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(ei->i_data[EXT3IPODS_TIND_BLOCK]));
								bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(ei->i_data[EXT3IPODS_TIND_BLOCK]));
								for (l=0; (l<(blocksize/4)) && (k<i_blocks); l++) {
									bh2i = sb_bread(mnt_ext3ipods->mnt_sb, *(unsigned int *)&(bh1i->b_data[l*4]));
									bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(ei->i_data[l*4]));
									for (m=0; (m<(blocksize/4)) && (k<i_blocks); m++) {
										bh3i = sb_bread(mnt_ext3ipods->mnt_sb, *(unsigned int *)&(bh2i->b_data[m*4]));
										bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(*(unsigned int *)&(bh2i->b_data[m*4])));
										for (n=0; (n<(blocksize/4)) && (k<i_blocks); n++, k++) {
											bgq_eval(bgq, bgf, ei->i_flags, sbi, le32_to_cpu(*(unsigned int *)&(bh3i->b_data[n*4])));
										}
										brelse(bh3i);
									}
									brelse(bh2i);
								}
								brelse(bh1i);
							}
						}

						// Free the inode block
//						brelse(bhit);
					}
				}

				// Free the inode bitmap
				brelse(bhib);
			}

			// Update block groups' quality
			for (i=0; i<sbi->s_groups_count; i++) {
				// Fetch group descriptor, update qualities and mark things dirty
				egd = ext3ipods_get_group_desc(mnt_ext3ipods->mnt_sb, i, &bh);
				egd->bg_quality = bgq[i];
				mnt_ext3ipods->mnt_sb->s_dirt = 1;
				mark_buffer_dirty(bh);
			}
		}

		// Print block group evaluated quality accordingly
		if (rgs == 0) { rgs++; }
		for (/*nop*/; rgs<=sbi->s_groups_count; rgs++) {
			snprintf(msg, sizeof(msg), "%lu\n", bgq[rgs-1]);
			if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
				sprintf(command, "eval%lu", rgs);
				goto out;
			}
			bytes_read += bytes_temp;
		}

		// Reset command, every bgq was printed
		memset(command, 0, sizeof(command));

		// Free allocated memory and quit
		vfree(bgf);
		vfree(bgq);
		bgq = NULL;
		bgf = NULL;
		goto out;
	}
	else
	if (!strncmp(command, "get", 3)) {
		// Fetch requested inode
		ptr = command+3;
		for (ris=0; (*ptr >= '0' && *ptr <= '9'); ptr++) {
			ris = (10*ris)+(*ptr-'0');
		}

		// Reset ptr (needed in case of block_index != 0)
		ptr = msg;

		// Check for massive reset
		if (ris == 0) {
			block_index = 0;
			ris = 1;
		}

		// Set loop parameters according to ris
		i = (ris-1)/(sbi->s_inodes_per_group);
		j = (ris-1)%(sbi->s_inodes_per_group);

		// Run every block group
		for (/*nop*/; i<sbi->s_groups_count; i++) {
			egd = ext3ipods_get_group_desc(mnt_ext3ipods->mnt_sb, i, NULL);
			bhib = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(egd->bg_inode_bitmap));

			// Run the inode bitmap and, for every valid entry, fetch and print that inode info
			for (/*nop*/; j<(sbi->s_inodes_per_group); j++) {
				bitmask = 0x01 << (j%8);
				inode_index = (i*sbi->s_inodes_per_group) + j + 1;
				if (inode_index < sbi->s_first_ino) {
					continue;
				}
				if ((bhib->b_data[j/8] & bitmask)) {
					// Fetch the corresponding ext3ipods_inode_info
					ei = EXT3IPODS_I(ext3ipods_iget(mnt_ext3ipods->mnt_sb, inode_index));
//					bhit = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(egd->bg_inode_table)+(j/sbi->s_inodes_per_block));
//					ino = (struct ext3ipods_inode *)((bhit->b_data)+((j%sbi->s_inodes_per_block)*EXT3IPODS_GOOD_OLD_INODE_SIZE));
//					ei = EXT3IPODS_I(ino);
					i_dscopy = ei->i_disksize-1;
					do_div(i_dscopy, blocksize);
					i_blocks = (ei->i_disksize > 0)?i_dscopy+1:0;

					// Print inode header
					if (block_index == 0) {
						snprintf(msg, sizeof(msg), "%lu ", inode_index);
						ptr = msg + strlen(msg);
						sprintf(ptr, "%d%d%d",
							(ei->i_flags>>20)&0x03,
							(ei->i_flags>>22)&0x03,
							(ei->i_flags>>24)&0x03);
						ptr += 3;
						sprintf(ptr, ":");
						if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
							sprintf(command, "get%lu", inode_index);
//							brelse(bhit);
							brelse(bhib);
							goto out;
						}
						bytes_read += bytes_temp;
					}
	
					// Run the datablocks of this inode
					for (k=0; k<i_blocks; k++) {
						if (k<EXT3IPODS_NDIR_BLOCKS) {
							// Direct addressing
							if ((block_index < k) || ((block_index==0) && (k==0))) {
								snprintf(msg, sizeof(msg), " %u", le32_to_cpu(ei->i_data[k]));
								if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
									sprintf(command, "get%lu", inode_index);
//									brelse(bhit);
									brelse(bhib);
									goto out;
								}
								bytes_read += bytes_temp;
								block_index = k;
							}
						} else {
							// 1-Indirect addressing
							bh1i = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(ei->i_data[EXT3IPODS_IND_BLOCK]));
							if (block_index < k) {
								snprintf(msg, sizeof(msg), " %u", le32_to_cpu(ei->i_data[EXT3IPODS_IND_BLOCK]));
								if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
									sprintf(command, "get%lu", inode_index);
									brelse(bh1i);
//									brelse(bhit);
									brelse(bhib);
									goto out;
								}
								bytes_read += bytes_temp;
								block_index = k;
							}
							for (l=0, k++; (l<(blocksize/4)) && (k<i_blocks); l++, k++) {
								if (block_index < k) {
									snprintf(msg, sizeof(msg), " %u", le32_to_cpu(*(unsigned int *)&(bh1i->b_data[l*4])));
									if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
										sprintf(command, "get%lu", inode_index);
										brelse(bh1i);
//										brelse(bhit);
										brelse(bhib);
										goto out;
									}
									bytes_read += bytes_temp;
									block_index = k;
								}
							}
							brelse(bh1i);
							if (k >= i_blocks) { break; }
							// 2-Indirect addressing
							bh1i = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(ei->i_data[EXT3IPODS_DIND_BLOCK]));
							if (block_index < k) {
								snprintf(msg, sizeof(msg), " %u", le32_to_cpu(ei->i_data[EXT3IPODS_DIND_BLOCK]));
								if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
									sprintf(command, "get%lu", inode_index);
									brelse(bh1i);
//									brelse(bhit);
									brelse(bhib);
									goto out;
								}
								bytes_read += bytes_temp;
								block_index = k;
							}
							for (l=0, k++; (l<(blocksize/4)) && (k<i_blocks); l++) {
								bh2i = sb_bread(mnt_ext3ipods->mnt_sb, *(unsigned int *)&(bh1i->b_data[l*4]));
								if (block_index < k) {
									snprintf(msg, sizeof(msg), " %u", *(unsigned int *)&(bh1i->b_data[l*4]));
									if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
										sprintf(command, "get%lu", inode_index);
										brelse(bh2i);
										brelse(bh1i);
//										brelse(bhit);
										brelse(bhib);
										goto out;
									}
									bytes_read += bytes_temp;
									block_index = k;
								}
								for (m=0, k++; (m<(blocksize/4)) && (k<i_blocks) ; m++, k++) {
									if (block_index < k) {
										sprintf(msg, " %u", le32_to_cpu(*(unsigned int *)&(bh2i->b_data[m*4])));
										if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
											sprintf(command, "get%lu", inode_index);
											brelse(bh2i);
											brelse(bh1i);
//											brelse(bhit);
											brelse(bhib);
											goto out;
										}
										bytes_read += bytes_temp;
										block_index = k;
									}
								}
								brelse(bh2i);
							}
							brelse(bh1i);
							if (k >= i_blocks) { break; }
							// 3-Indirect addressing
							bh1i = sb_bread(mnt_ext3ipods->mnt_sb, le32_to_cpu(ei->i_data[EXT3IPODS_TIND_BLOCK]));
							if (block_index < k) {
								snprintf(msg, sizeof(msg), " %u", le32_to_cpu(ei->i_data[EXT3IPODS_TIND_BLOCK]));
								if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
									sprintf(command, "get%lu", inode_index);
									brelse(bh1i);
//									brelse(bhit);
									brelse(bhib);
									goto out;
								}
								bytes_read += bytes_temp;
								block_index = k;
							}
							for (l=0; (l<(blocksize/4)) && (k<i_blocks); l++) {
								bh2i = sb_bread(mnt_ext3ipods->mnt_sb, *(unsigned int *)&(bh1i->b_data[l*4]));
								if (block_index < k) {
									snprintf(msg, sizeof(msg), " %u", *(unsigned int *)&(bh1i->b_data[l*4]));
									if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
										sprintf(command, "get%lu", inode_index);
										brelse(bh2i);
										brelse(bh1i);
//										brelse(bhit);
										brelse(bhib);
										goto out;
									}
									bytes_read += bytes_temp;
									block_index = k;
								}
								for (m=0; (m<(blocksize/4)) && (k<i_blocks); m++) {
									bh3i = sb_bread(mnt_ext3ipods->mnt_sb, *(unsigned int *)&(bh2i->b_data[m*4]));
									if (block_index < k) {
										snprintf(msg, sizeof(msg), " %u", *(unsigned int *)&(bh2i->b_data[m*4]));
										if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
											sprintf(command, "get%lu", inode_index);
											brelse(bh3i);
											brelse(bh2i);
											brelse(bh1i);
//											brelse(bhit);
											brelse(bhib);
											goto out;
										}
										bytes_read += bytes_temp;
										block_index = k;
									}
									for (n=0; (n<(blocksize/4)) && (k<i_blocks); n++, k++) {
										if (block_index < k) {
											snprintf(msg, sizeof(msg), " %u", le32_to_cpu(*(unsigned int *)&(bh3i->b_data[n*4])));
											if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
												sprintf(command, "get%lu", inode_index);
												brelse(bh3i);
												brelse(bh2i);
												brelse(bh1i);
//												brelse(bhit);
												brelse(bhib);
												goto out;
											}
											bytes_read += bytes_temp;
											block_index = k;
										}
									}
									brelse(bh3i);
								}
								brelse(bh2i);
							}
							brelse(bh1i);
						}
					}
					sprintf(msg, "\n");
					if ((bytes_temp = userspace_write(msg, strlen(msg), &buffer, &length)) == 0) {
						// If I keep get<same_inode> and block_index=<I've_printed_the_last_block>,
						// this function will skip to this point and print the line break properly!
						sprintf(command, "get%lu", inode_index);
//						brelse(bhit);
						brelse(bhib);
						goto out;
					}
					bytes_read += bytes_temp;

					// Adjust command for next iteration
					sprintf(command, "get%lu", inode_index+1);

					// Reset block count
					block_index = 0;

					// Release buffer heads and quit
//					brelse(bhit);
					brelse(bhib);
					goto out;
				}
			}
			// Reset j for next iteration
			j = 0;

			// Release buffer head
			brelse(bhib);
		}

		if (i == sbi->s_groups_count) {
			// Every inode bitmap has been checked, reset command
			memset(command, 0, sizeof(command));
		}
	}

out:
	// Return the number of bytes read
	return(bytes_read);
}

// Read commands from the device (user writing to device)
static ssize_t device_write(struct file *filp, const char *buff, size_t len, loff_t * off)
{
	// Local variables
	int				bytes_read = 0;			// Number of bytes read from userspace
	char				lcommand[sizeof(command)];	// Local command
	int				rbg;				// Requested block group
	short unsigned int		fset;				// 16-bit flagset
	char				*ptr;				// Temporary char pointer
	struct ext3ipods_group_desc	*egd;				// ext3ipods group descriptor
	struct ext3ipods_sb_info	*sbi;				// In memory superblock
	struct buffer_head		*bh = NULL;			// Buffer head

	// Reset msg
	memset(lcommand, 0, sizeof(lcommand));

	// Read from device
	for (bytes_read=0; (bytes_read<len) && (bytes_read<(sizeof(lcommand)-1)); bytes_read++) {
		get_user(lcommand[bytes_read], buff+bytes_read);
	}

	// Reset line breaks
	while ( (lcommand[strlen(lcommand)-1] == '\n') ||
		(lcommand[strlen(lcommand)-1] == '\r'))
		 lcommand[strlen(lcommand)-1] = 0;

	// If it's a set command, execute it locally
	if (!strncmp(lcommand, "set", 3)) {
		// Check if we have a mounted ext3ipods fs
		if (!mnt_ext3ipods) {
			goto out;
		}
		// Decode requested block group
		ptr = lcommand+3;
		for (rbg=0; (*ptr >= '0' && *ptr <= '9'); ptr++) {
			rbg = (10*rbg)+(*ptr-'0');
		}
		sbi = EXT3IPODS_SB(mnt_ext3ipods->mnt_sb);
		if (rbg >= sbi->s_groups_count) {
			goto out;
		}
		// Decode new flag set
		for (ptr++, fset=0; (*ptr >= '0' && *ptr <= '9'); ptr++) {
			fset = (10*fset)+(*ptr-'0');
		}
		// Fetch group descriptor, update flags and mark things dirty
		egd = ext3ipods_get_group_desc(mnt_ext3ipods->mnt_sb, rbg, &bh);
		egd->bg_flags = fset;
		mnt_ext3ipods->mnt_sb->s_dirt = 1;
		mark_buffer_dirty(bh);
	} else {
		// Copy to the global command
		memcpy(command, lcommand, sizeof(command));
	}

out:
	// Return number of bytes read
	return(bytes_read);
}
