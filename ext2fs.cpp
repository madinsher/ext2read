/**
 * Ext2read
 * File: ext2fs.cpp
 **/
/**
 * Copyright (C) 2005 2010 by Manish Regmi   (regmi dot manish at gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 **/

#include <stdio.h>
#include <stddef.h>
#include "ext2read.h"
#include "lvm.h"

/* Compile time checks that the on-disk superblock layout is what the code expects. */
typedef char ext2_sb_size_check[(sizeof(EXT2_SUPER_BLOCK) == 1024) ? 1 : -1];
typedef char ext2_sb_desc_size_check[(offsetof(EXT2_SUPER_BLOCK, s_desc_size) == 0xFE) ? 1 : -1];
typedef char ext2_sb_blocks_hi_check[(offsetof(EXT2_SUPER_BLOCK, s_blocks_count_hi) == 0x150) ? 1 : -1];

Ext2Partition::Ext2Partition(lloff_t size, lloff_t offset, int ssize, FileHandle phandle, LogicalVolume *vol)
{
    int ret;

    desc = NULL;
    desc_size = sizeof(EXT2_GROUP_DESC);
    totalGroups = 0;
    last_block = (lloff_t)-1;
    total_sectors = size;
    relative_sect = offset;
    handle = phandle;
    sect_size = ssize;
    onview = false;
    inode_buffer = NULL;
    lvol = vol;
    buffercache.setMaxCost(MAX_CACHE_SIZE);
    //has_extent = 1;
    ret = mount();
    if(ret < 0)
    {
        is_valid = false;
        return;
    }

    root = read_inode(EXT2_ROOT_INO);
    if(!root)
    {
        is_valid = false;
        LOG("Cannot read the root of %s \n", linux_name.c_str());
        return;
    }

    root->file_name = linux_name;
    root->file_type = 0x02;   //FIXME: do not hardcode
    is_valid = true;
}

Ext2Partition::~Ext2Partition()
{
    free (desc);
}

void Ext2Partition::set_linux_name(const char *name, int disk, int partition)
{
    char dchar = 'a' + disk;
    char str_buffer[10];
    sprintf(str_buffer,"%d",partition);

    linux_name = name;
    linux_name.append(1, dchar);
    linux_name.append(str_buffer);
}

string &Ext2Partition::get_linux_name()
{
    return linux_name;
}

string &Ext2Partition::get_volume_name()
{
    return volume_name;
}

int Ext2Partition::ext2_readblock(lloff_t blocknum, void *buffer)
{
    char *newbuffer;
    int nsects = blocksize/sect_size;
    int ret;
    lloff_t sectno;

    newbuffer = buffercache.take(blocknum);
    if(!newbuffer)
    {
        newbuffer = new char[blocksize];
        if(!newbuffer)
            return -1;

        if(lvol)
        {
            sectno = lvol->lvm_mapper((lloff_t)nsects * blocknum);
        }
        else
        {
            sectno = (lloff_t)((lloff_t)nsects * blocknum) + relative_sect;
        }
        ret = read_disk(handle, newbuffer, sectno, nsects, sect_size);
        if(ret < 0)
        {
            delete [] newbuffer;
            return ret;
        }
    }

    memcpy(buffer, newbuffer, blocksize);
    buffercache.insert(blocknum, newbuffer, 1);
    return 0;
}

int Ext2Partition::mount()
{
    EXT2_SUPER_BLOCK sblock;
    lloff_t gSizeb, gSizes;		/* Size of total group desc in bytes and sectors */
    lloff_t blocks_count;
    char *tmpbuf;

    read_disk(handle, &sblock, relative_sect + 2, 2, sect_size);	/* read superBlock of root */
    if(sblock.s_magic != EXT2_SUPER_MAGIC)
    {
        LOG("Bad Super Block. The drive %s is not ext2 formatted.\n", linux_name.c_str());
        return -1;
    }

    if(sblock.s_feature_incompat & EXT2_FEATURE_INCOMPAT_COMPRESSION)
    {
        LOG("File system compression is used which is not supported.\n");
    }
    blocksize = EXT2_BLOCK_SIZE(&sblock);
    inodes_per_group = EXT2_INODES_PER_GROUP(&sblock);
    inode_size = EXT2_INODE_SIZE(&sblock);

    volume_name = sblock.s_volume_name;

    /* With the 64bit feature (the default for mkfs.ext4 since 2016) group descriptors are
     * s_desc_size (normally 64) bytes long instead of 32, and block counts have a high half. */
    desc_size = sizeof(EXT2_GROUP_DESC);
    blocks_count = sblock.s_blocks_count;
    if(sblock.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    {
        desc_size = (sblock.s_desc_size >= sizeof(EXT2_GROUP_DESC)) ? sblock.s_desc_size : 64;
        blocks_count |= ((lloff_t) sblock.s_blocks_count_hi) << 32;
    }

    LOG("Block size %d, inp %d, inodesize %d, descsize %d\n", blocksize, inodes_per_group, inode_size, desc_size);
    totalGroups = (uint32_t)((blocks_count - sblock.s_first_data_block + EXT2_BLOCKS_PER_GROUP(&sblock) - 1) /
                             EXT2_BLOCKS_PER_GROUP(&sblock));
    gSizeb = (lloff_t)desc_size * totalGroups;
    gSizes = (gSizeb + sect_size - 1) / sect_size;

    desc = (EXT2_GROUP_DESC *) calloc((size_t) totalGroups, desc_size);
    if(desc == NULL)
    {
        LOG("Not enough Memory: mount: desc: Exiting\n");
        exit(1);
    }

    if((tmpbuf = (char *) malloc((size_t)(gSizes * sect_size))) == NULL)
    {
        LOG("Not enough Memory: mount: tmpbuf: Exiting\n");
        exit(1);
    }

    /* The group descriptors start in the block right after the one holding the superblock */
    read_disk(handle, tmpbuf,
              relative_sect + (lloff_t)(sblock.s_first_data_block + 1) * (blocksize/sect_size),
              (int) gSizes, sect_size);

    memcpy(desc, tmpbuf, (size_t) gSizeb);

    free(tmpbuf);

    return 0;
}

/* Returns the block number of the inode table of the given group. */
lloff_t Ext2Partition::get_inode_table(uint32_t group)
{
    char *p = (char *) desc + (size_t) group * desc_size;
    lloff_t block = ((EXT2_GROUP_DESC *) p)->bg_inode_table;

    if(desc_size >= 64)
    {
        uint32_t hi;
        memcpy(&hi, p + EXT4_BG_INODE_TABLE_HI_OFFSET, sizeof(hi));
        block |= ((lloff_t) hi) << 32;
    }
    return block;
}

EXT2DIRENT *Ext2Partition::open_dir(Ext2File *parent)
{
    EXT2DIRENT *dirent;

    if(!parent)
        return NULL;

    dirent = new EXT2DIRENT;
    dirent->parent = parent;
    dirent->next = NULL;
    dirent->dirbuf = NULL;
    dirent->read_bytes = 0;
    dirent->next_block = 0;

    return dirent;
}

Ext2File *Ext2Partition::read_dir(EXT2DIRENT *dirent)
{
    string filename;
    Ext2File *newEntry;
    char *pos;
    char *bufend;
    int ret;

    if(!dirent)
        return NULL;
    if(!dirent->dirbuf)
    {
        dirent->dirbuf = (EXT2_DIR_ENTRY *) new char[blocksize];
        if(!dirent->dirbuf)
            return NULL;
        ret = read_data_block(&dirent->parent->inode, dirent->next_block, dirent->dirbuf);
        if(ret < 0)
            return NULL;

        dirent->next_block++;
    }

    bufend = (char *) dirent->dirbuf + blocksize;

    again:
    if(!dirent->next)
        dirent->next = dirent->dirbuf;
    else
    {
        pos = (char *) dirent->next;
        /* A corrupt rec_len must neither loop forever nor run backwards: treat it as end of block. */
        if((dirent->next->rec_len < 8) || (dirent->next->rec_len & 3))
            pos = bufend;
        else
            pos += dirent->next->rec_len;
        dirent->next = (EXT2_DIR_ENTRY *) pos;
        if(IS_BUFFER_END(dirent->next, dirent->dirbuf, blocksize) || ((pos + 8) > bufend))
        {
            dirent->next = NULL;
            if(dirent->read_bytes < dirent->parent->file_size)
            {
                //LOG("DIR: Reading next block %d parent %s\n", dirent->next_block, dirent->parent->file_name.c_str());
                ret = read_data_block(&dirent->parent->inode, dirent->next_block, dirent->dirbuf);
                if(ret < 0)
                    return NULL;

                dirent->next_block++;
                goto again;
            }
            return NULL;
        }
    }

    dirent->read_bytes += dirent->next->rec_len;

    /* inode 0 marks an unused entry. With metadata_csum every leaf block also ends with such an
     * entry (the checksum tail) and htree interior blocks consist of one: skip it, do not stop. */
    if(dirent->next->inode == 0)
        goto again;

    filename.assign(dirent->next->name, dirent->next->name_len);
    if((filename.compare(".") == 0) ||
       (filename.compare("..") == 0))
        goto again;


    newEntry = read_inode(dirent->next->inode);
    if(!newEntry)
    {
        LOG("Error reading Inode %d parent inode %d.\n", dirent->next->inode, dirent->parent->inode_num);
        goto again;
    }

    newEntry->file_type = dirent->next->filetype;
    newEntry->file_name = filename;

    return newEntry;
}

void Ext2Partition::close_dir(EXT2DIRENT *dirent)
{
    delete [] dirent->dirbuf;
    delete dirent;
}

Ext2File *Ext2Partition::read_inode(uint32_t inum)
{
    uint32_t group, index;
    lloff_t blknum;
    int inode_index, ret = 0;
    Ext2File *file = NULL;
    EXT2_INODE *src;

    if(inum == 0)
        return NULL;

    if(!inode_buffer)
    {
        inode_buffer = (char *)malloc(blocksize);
        if(!inode_buffer)
            return NULL;
    }

    group = (inum - 1) / inodes_per_group;

    if(group >= totalGroups)
    {
        LOG("Error Reading Inode %X. Invalid Inode Number\n", inum);
        return NULL;
    }

    index = ((inum - 1) % inodes_per_group) * inode_size;
    inode_index = (index % blocksize);
    blknum = get_inode_table(group) + (index / blocksize);


    if(blknum != last_block) {
        ret = ext2_readblock(blknum, inode_buffer);
        if (ret < 0) {
            LOG("Disk read failed\n");
            return NULL;
        }
    }


    file = new Ext2File;
    if(!file)
    {
        LOG("Allocation of File Failed. \n");
        return NULL;
    }
    src = (EXT2_INODE *)(inode_buffer + inode_index);
    file->inode = *src;

    LOG("BLKNUM is %d, inode_index %d\n", file->inode.i_size, inode_index);
    file->inode_num = inum;
    file->file_size = (lloff_t) src->i_size | ((lloff_t) src->i_size_high << 32);
    if(file->file_size == 0)
    {
        LOG("Inode %d with file size 0\n", inum);
    }
    file->partition = (Ext2Partition *)this;
    file->onview = false;

    last_block = blknum;

    return file;
}


int Ext2Partition::read_data_block(EXT2_INODE *ino, lloff_t lbn, void *buf)
{
    lloff_t block;

    if(INODE_HAS_EXTENT(ino))
        block = extent_to_logical(ino, lbn);
    else
        block = fileblock_to_logical(ino, lbn);

    if(block == 0)
        return -1;

    return ext2_readblock(block, buf);
}

lloff_t Ext2Partition::extent_binarysearch(EXT4_EXTENT_HEADER *header, lloff_t lbn, bool isallocated)
{
    EXT4_EXTENT *extent;
    EXT4_EXTENT_IDX *index;
    EXT4_EXTENT_HEADER *child;
    lloff_t physical_block = 0;
    lloff_t block;

    if(header->eh_magic != EXT4_EXT_MAGIC)
    {
        LOG("Invalid magic in Extent Header: %X\n", header->eh_magic);
        return 0;
    }
    extent = EXT_FIRST_EXTENT(header);
    //    LOG("HEADER: magic %x Entries: %d depth %d\n", header->eh_magic, header->eh_entries, header->eh_depth);
    if(header->eh_depth == 0)
    {        
        for(int i = 0; i < header->eh_entries; i++)
        {         
            //          LOG("EXTENT: Block: %d Length: %d LBN: %d\n", extent->ee_block, extent->ee_len, lbn);
            if((lbn >= extent->ee_block) &&
               (lbn < (extent->ee_block + extent->ee_len)))
            {
                physical_block = ext_to_block(extent) + lbn;
                physical_block = physical_block - (lloff_t)extent->ee_block;
                if(isallocated)
                    delete [] header;
                //                LOG("Physical Block: %d\n", physical_block);
                return physical_block;
            }
            extent++; // Pointer increment by size of Extent.
        }
        return 0;
    }

    index = EXT_FIRST_INDEX(header);
    for(int i = 0; i < header->eh_entries; i++)
    {
        //        LOG("INDEX: Block: %d Leaf: %d \n", index->ei_block, index->ei_leaf_lo);
        if((i == (header->eh_entries - 1)) ||
           (lbn < (index + 1)->ei_block))
        {
            child = (EXT4_EXTENT_HEADER *) new char [blocksize];
            block = idx_to_block(index);
            ext2_readblock(block, (void *) child);

            return extent_binarysearch(child, lbn, true);
        }
        index++;
    }

    // We reach here if we do not find the key
    if(isallocated)
        delete [] header;

    return physical_block;
}

lloff_t Ext2Partition::extent_to_logical(EXT2_INODE *ino, lloff_t lbn)
{
    lloff_t block;
    struct ext4_extent_header *header;

    header = get_ext4_header(ino);
    block = extent_binarysearch(header, lbn, false);

    return block;
}

uint32_t Ext2Partition::fileblock_to_logical(EXT2_INODE *ino, uint32_t lbn)
{
    uint32_t block, indlast, dindlast;
    uint32_t tmpblk, sz;
    uint32_t *indbuffer;
    uint32_t *dindbuffer;
    uint32_t *tindbuffer;

    if(lbn < EXT2_NDIR_BLOCKS)
    {
        return ino->i_block[lbn];
    }

    sz = blocksize / sizeof(uint32_t);
    indlast = sz + EXT2_NDIR_BLOCKS;
    indbuffer = new uint32_t [sz];
    if((lbn >= EXT2_NDIR_BLOCKS) && (lbn < indlast))
    {
        block = ino->i_block[EXT2_IND_BLOCK];
        ext2_readblock(block, indbuffer);
        lbn -= EXT2_NDIR_BLOCKS;
        block = indbuffer[lbn];
        delete [] indbuffer;
        return block;
    }

    dindlast = (sz * sz) + indlast;
    dindbuffer = new uint32_t [sz];
    if((lbn >= indlast) && (lbn < dindlast))
    {
        block = ino->i_block[EXT2_DIND_BLOCK];
        ext2_readblock(block, dindbuffer);

        tmpblk = lbn - indlast;
        block = dindbuffer[tmpblk/sz];
        ext2_readblock(block, indbuffer);

        lbn = tmpblk % sz;
        block = indbuffer[lbn];

        delete [] dindbuffer;
        delete [] indbuffer;
        return block;
    }

    tindbuffer = new uint32_t [sz];
    if(lbn >= dindlast)
    {
        block = ino->i_block[EXT2_TIND_BLOCK];
        ext2_readblock(block, tindbuffer);

        tmpblk = lbn - dindlast;
        block = tindbuffer[tmpblk/(sz * sz)];
        ext2_readblock(block, dindbuffer);

        block = tmpblk / sz;
        lbn = tmpblk % sz;
        block = dindbuffer[block];
        ext2_readblock(block, indbuffer);
        block = indbuffer[lbn];

        delete [] tindbuffer;
        delete [] dindbuffer;
        delete [] indbuffer;

        return block;
    }

    // We should not reach here
    return 0;
}
