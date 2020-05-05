#include <fat32.h>

#include <device.h>
#include <logging.h>
#include <memory.h>
#include <string.h>

namespace fs::FAT32{
    size_t Read(struct fs_node* node, size_t offset, size_t size, uint8_t *buffer);
    size_t Write(struct fs_node* node, size_t offset, size_t size, uint8_t *buffer);
    void Open(struct fs_node* node, uint32_t flags);
    void Close(struct fs_node* node);
    int ReadDir(struct fs_node* node, struct fs_dirent* dirent, uint32_t index);
    fs_node* FindDir(struct fs_node* node, char* name);

    int Identify(PartitionDevice* part){
        fat32_boot_record_t* bootRecord = (fat32_boot_record_t*)kmalloc(512);

        if(part->Read(0, 512, (uint8_t*)bootRecord)){ // Read Volume Boot Record (First sector of partition)
            return -1; // Disk Error
        }

        int isFat = 0;

        if(bootRecord->ebr.signature == 0x28 || bootRecord->ebr.signature == 0x29){
            uint32_t dataSectors = bootRecord->bpb.largeSectorCount - (bootRecord->bpb.reservedSectors + (bootRecord->ebr.sectorsPerFAT * bootRecord->bpb.fatCount));
            uint32_t clusters = dataSectors / bootRecord->bpb.sectorsPerCluster;

            if(clusters > 65525) isFat = 1;
        }

        kfree(bootRecord);

        return isFat;
    }

    uint64_t Fat32Volume::ClusterToLBA(uint32_t cluster){
        return (bootRecord->bpb.reservedSectors + bootRecord->bpb.fatCount * bootRecord->ebr.sectorsPerFAT) + cluster * bootRecord->bpb.sectorsPerCluster - (2 * bootRecord->bpb.sectorsPerCluster);
    }

    Fat32Volume::Fat32Volume(PartitionDevice* _part, char* name){
        this->part = _part;

        fat32_boot_record_t* bootRecord = (fat32_boot_record_t*)kmalloc(512);

        if(part->Read(0, 512, (uint8_t*)bootRecord)){ // Read Volume Boot Record (First sector of partition)
            Log::Warning("Disk Error Initializing Volume"); // Disk Error
            return;
        }

        this->bootRecord = bootRecord;

        Log::Info("[FAT32] Initializing Volume\tSignature: %d, OEM ID: %s, Size: %d MB", bootRecord->ebr.signature, (char*)bootRecord->bpb.oem, bootRecord->bpb.largeSectorCount * 512 / 1024 / 1024);

        clusterSizeBytes = bootRecord->bpb.sectorsPerCluster * part->parentDisk->blocksize;

        mountPoint.flags = FS_NODE_MOUNTPOINT | FS_NODE_DIRECTORY;
        mountPoint.inode = bootRecord->ebr.rootClusterNum;

        mountPoint.read = FAT32::Read;
        mountPoint.write = FAT32::Write;
        mountPoint.open = FAT32::Open;
        mountPoint.close = FAT32::Close;
        mountPoint.findDir = FAT32::FindDir;
        mountPoint.readDir = FAT32::ReadDir;

        mountPoint.vol = this;

        strcpy(mountPoint.name, name);

        mountPointDirent.inode = bootRecord->ebr.rootClusterNum;
        mountPointDirent.type = FS_NODE_DIRECTORY;
        
        strcpy(mountPointDirent.name, name);
        
    }

    List<uint32_t>* Fat32Volume::GetClusterChain(uint32_t cluster){
        List<uint32_t>* list = new List<uint32_t>();

        uint32_t* buf = (uint32_t*)kmalloc(4096);

        uint32_t lastBlock = 0xFFFFFFFF;

        do
        {
            list->add_back(cluster);

            uint32_t block = ((cluster * 4) / 4096);
            uint32_t offset = cluster % (4096 / 4);
    
            if(block != lastBlock) {
                if(part->Read(bootRecord->bpb.reservedSectors + block * (4096 / part->parentDisk->blocksize) /* Get Sector of Block */, 4096, buf)){
                    return nullptr;
                }

                lastBlock = block;
            }
    
            cluster = buf[offset] & 0x0FFFFFFF;
    
        } while(cluster && (cluster & 0x0FFFFFFF) < 0x0FFFFFF8);
    
        return list;
    }
    
    void* Fat32Volume::ReadClusterChain(uint32_t cluster, int* clusterCount, size_t max){
        uint32_t maxCluster = (max / (bootRecord->bpb.sectorsPerCluster * part->parentDisk->blocksize)) + 1;
        List<uint32_t>* clusterChain = GetClusterChain(cluster);

        if(!clusterChain) return nullptr;

        void* buf = kmalloc(clusterChain->get_length() * clusterSizeBytes);
        void* _buf = buf;

        for(int i = 0; i < clusterChain->get_length() && maxCluster; i++){
            part->Read(ClusterToLBA(clusterChain->get_at(i)), clusterSizeBytes, buf);

            buf += clusterSizeBytes;
        }

        if(clusterCount)
            *clusterCount = clusterChain->get_length();

        delete clusterChain;

        return _buf;
    }

    void* Fat32Volume::ReadClusterChain(uint32_t cluster, int* clusterCount){
        if(cluster == 0) cluster = bootRecord->ebr.rootClusterNum;
        List<uint32_t>* clusterChain = GetClusterChain(cluster);

        if(!clusterChain) return nullptr;

        void* buf = kmalloc(clusterChain->get_length() * clusterSizeBytes);
        void* _buf = buf;

        for(int i = 0; i < clusterChain->get_length(); i++){
            part->Read(ClusterToLBA(clusterChain->get_at(i)), clusterSizeBytes, buf);

            buf += clusterSizeBytes;
        }

        if(clusterCount)
            *clusterCount = clusterChain->get_length();

        delete clusterChain;

        return _buf;
    }

    size_t Fat32Volume::Read(fs_node_t* node, size_t offset, size_t size, uint8_t *buffer){
        if(!node->inode || node->flags & FS_NODE_DIRECTORY) return 0;

        int count;
        void* _buf = ReadClusterChain(node->inode, &count, size);

        if(count * bootRecord->bpb.sectorsPerCluster * part->parentDisk->blocksize < size) return 0;

        memcpy(buffer, _buf, size);
        return size;
    }

    size_t Fat32Volume::Write(fs_node_t* node, size_t offset, size_t size, uint8_t *buffer){

    }

    void Fat32Volume::Open(fs_node_t* node, uint32_t flags){
        
    }
    
    void Fat32Volume::Close(fs_node_t* node){

    }

    int Fat32Volume::ReadDir(fs_node_t* node, fs_dirent_t* dirent, uint32_t index){
        int lfnCount = 0;
        int entryCount = 0;

        uint32_t cluster = node->inode;
        int clusterCount = 0;
        
        fat_entry_t* dirEntries = (fat_entry_t*)ReadClusterChain(cluster, &clusterCount);

        fat_entry_t* dirEntry;
        int dirEntryIndex;

        fat_lfn_entry_t** lfnEntries;

        for(int i = 0; i < clusterCount * clusterSizeBytes; i++){
            if(dirEntries[i].filename[0] == 0) return -1; // No Directory Entry at index
            else if (dirEntries[i].filename[0] == 0xE5) {
                lfnCount = 0;
                continue; // Unused Entry
            }
            else if (dirEntries[i].attributes == 0x0F) lfnCount++; // Long File Name Entry
            else if (dirEntries[i].attributes & 0x08 /*Volume ID*/){
                lfnCount = 0;
                continue;
            } else {
                if(entryCount == index){
                    dirEntry = &dirEntries[i];
                    dirEntryIndex = i;
                    break;
                }

                entryCount++;
                lfnCount = 0;
            }
        }

        lfnEntries = (fat_lfn_entry_t**)kmalloc(sizeof(fat_lfn_entry_t*) * lfnCount);

        for(int i = 0; i < lfnCount; i++){
            fat_lfn_entry_t* lfnEntry = (fat_lfn_entry_t*)(&dirEntries[dirEntryIndex -  i - 1]);

            lfnEntries[i] = lfnEntry; 
        }

        dirent->inode = dirEntry->lowClusterNum | (dirEntry->highClusterNum << 16);

        if(lfnCount){
            GetLongFilename(dirent->name, lfnEntries, lfnCount);
        } else {
            strncpy(dirent->name, (char*)dirEntry->filename, 8);
            while(strchr(dirent->name, ' ')) *strchr(dirent->name, ' ') = 0; // Remove Spaces
            if(strchr((char*)dirEntry->ext, ' ') != (char*)dirEntry->ext){
                strncpy(dirent->name + strlen(dirent->name), ".", 1);
                strncpy(dirent->name + strlen(dirent->name), (char*)dirEntry->ext, 3);
            }
        }

        if(dirEntry->attributes & FAT_ATTR_DIRECTORY) dirent->type = FS_NODE_DIRECTORY;
        else dirent->type = FS_NODE_FILE;

        return 0;
    }

    fs_node_t* Fat32Volume::FindDir(fs_node_t* node, char* name){
        int lfnCount = 0;
        int entryCount = 0;

        uint32_t cluster = node->inode;
        int clusterCount = 0;
        
        fat_entry_t* dirEntries = (fat_entry_t*)ReadClusterChain(cluster, &clusterCount);

        Log::Warning(name);

        List<int> foundEntries;
        List<int> foundEntriesLfnCount;

        fat_lfn_entry_t** lfnEntries;
        fs_node_t* _node = nullptr;

        for(int i = 0; i < clusterCount * clusterSizeBytes; i++){
            if(dirEntries[i].filename[0] == 0) return nullptr; // No Directory Entry at index
            else if (dirEntries[i].filename[0] == 0xE5) {
                lfnCount = 0;
                continue; // Unused Entry
            }
            else if (dirEntries[i].attributes == 0x0F) lfnCount++; // Long File Name Entry
            else if (dirEntries[i].attributes & 0x08 /*Volume ID*/){
                lfnCount = 0;
                continue;
            } else {
                char* _name = (char*)kmalloc(128);
                if(lfnCount){
                    lfnEntries = (fat_lfn_entry_t**)kmalloc(sizeof(fat_lfn_entry_t*) * lfnCount);
                    
                    for(int k = 0; k < lfnCount; k++){
                        fat_lfn_entry_t* lfnEntry = (fat_lfn_entry_t*)(&dirEntries[i -  k - 1]);

                        lfnEntries[k] = lfnEntry; 
                    }

                    GetLongFilename(_name, lfnEntries, lfnCount);

                    kfree(lfnEntries);
                } else {
                    strncpy(_name, (char*)dirEntries[i].filename, 8);
                    while(strchr(_name, ' ')) *strchr(_name, ' ') = 0; // Remove Spaces
                    if(strchr((char*)dirEntries[i].ext, ' ') != (char*)dirEntries[i].ext){
                        strncpy(_name + strlen(_name), ".", 1);
                        strncpy(_name + strlen(_name), (char*)dirEntries[i].ext, 3);
                    }
                }

                Log::Warning(_name);

                if(strcmp(_name, name) == 0){
                    if((((uint32_t)dirEntries[i].highClusterNum) << 16) | dirEntries[i].lowClusterNum == bootRecord->ebr.rootClusterNum || (((uint32_t)dirEntries[i].highClusterNum) << 16) | dirEntries[i].lowClusterNum == 0) 
                        return &mountPoint; // Root Directory
                    _node = (fs_node_t*)kmalloc(sizeof(fs_node_t));
                    _node->size = dirEntries[i].fileSize;
                    _node->inode = (((uint32_t)dirEntries[i].highClusterNum) << 16) | dirEntries[i].lowClusterNum;
                    if(dirEntries[i].attributes & FAT_ATTR_DIRECTORY) _node->flags = FS_NODE_DIRECTORY;
                    else _node->flags = FS_NODE_FILE;
                    strcpy(_node->name, _name);
                    break;
                }
                lfnCount = 0;
            }
        }

        if(_node){
            _node->vol = this;
            _node->read = FAT32::Read;
            _node->write = FAT32::Write;
            _node->open = FAT32::Open;
            _node->close = FAT32::Close;
            _node->findDir = FAT32::FindDir;
            _node->readDir = FAT32::ReadDir;
        }

        return _node;
    }

    size_t Read(struct fs_node* node, size_t offset, size_t size, uint8_t *buffer){
        ((Fat32Volume*)node->vol)->Read(node, offset, size, buffer);
    }

    size_t Write(struct fs_node* node, size_t offset, size_t size, uint8_t *buffer){
        ((Fat32Volume*)node->vol)->Write(node, offset, size, buffer);
    }

    void Open(struct fs_node* node, uint32_t flags){
        ((Fat32Volume*)node->vol)->Open(node, flags);
    }

    void Close(struct fs_node* node){
        ((Fat32Volume*)node->vol)->Close(node);
    }

    int ReadDir(struct fs_node* node, fs_dirent_t* dirent, uint32_t index){
        return ((Fat32Volume*)node->vol)->ReadDir(node, dirent, index);
    }

    fs_node* FindDir(struct fs_node* node, char* name){
        ((Fat32Volume*)node->vol)->FindDir(node, name);
    }
}