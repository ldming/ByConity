#include <Catalog/Catalog.h>
#include <Storages/TableMetaEntry.h>
#include <Storages/PartCacheManager.h>
#include <Storages/TableMetaEntry.h>

#include <Core/Types.h>

namespace DB
{

Catalog::PartitionMap TableMetaEntry::getPartitions(const Strings & wanted_partition_ids)
{
    Catalog::PartitionMap res;
    if (wanted_partition_ids.size() < 100)
    {
        /// When wanted_partition_ids is small, then go with find
        for (const auto & partition_id : wanted_partition_ids)
        {
            if (auto it = partitions.find(partition_id); it != partitions.end())
                res.emplace(partition_id, *it);
        }
    }
    else
    {
        /// Otherwise, leverage wait free scan
        std::unordered_set<String> wanted_partition_set;
        for (const auto & partition_id : wanted_partition_ids)
            wanted_partition_set.insert(partition_id);

        for (auto it = partitions.begin(); it != partitions.end(); it++)
        {
            auto & partition_info_ptr = *it;
            if (wanted_partition_set.contains(partition_info_ptr->partition_id))
                res.emplace(partition_info_ptr->partition_id, partition_info_ptr);
        }
    }
    return res;
}

Strings TableMetaEntry::getPartitionIDs()
{
    Strings partition_ids;
    partition_ids.reserve(partitions.size());
    for (auto it = partitions.begin(); it != partitions.end(); it++)
        partition_ids.push_back((*it)->partition_id);
    return partition_ids;
}

std::vector<std::shared_ptr<MergeTreePartition>> TableMetaEntry::getPartitionList()
{
    std::vector<std::shared_ptr<MergeTreePartition>> partition_list;
    partition_list.reserve(partitions.size());
    for (auto it = partitions.begin(); it != partitions.end(); it++)
        partition_list.push_back((*it)->partition_ptr);
    return partition_list;
}

void TableMetaEntry::forEachPartition(std::function<void(PartitionInfoPtr)> callback)
{
    for (auto it = partitions.begin(); it != partitions.end(); it++)
        callback(*it);
}
}
