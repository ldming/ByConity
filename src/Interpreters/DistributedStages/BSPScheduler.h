#include <Interpreters/DistributedStages/Scheduler.h>

namespace DB
{

// Only if the dependencies were executed done, the segment would be scheduled.
//
// Scheduler::genTopology -> Scheduler::scheduleTask -> BSPScheduler::submitTasks
//                                                                  |
//                                                        Scheduler::dispatchTask -rpc-> Worker node
//                                                                  |                       | rpc
//                                                                  <--------- BSPScheduler::onSegmentFinished
class BSPScheduler : public Scheduler
{
    struct PendingTaskIntances
    {
        // nodes -> [segment instance who prefer to be scheduled to it]
        std::unordered_map<AddressInfo, std::unordered_set<SegmentTaskInstance, SegmentTaskInstance::Hash>, AddressInfo::Hash> for_nodes;
        // segment isntances who have no preference.
        std::unordered_set<SegmentTaskInstance, SegmentTaskInstance::Hash> no_prefs;
    };

public:
    BSPScheduler(const String & query_id_, ContextPtr query_context_, std::shared_ptr<DAGGraph> dag_graph_ptr_)
        : Scheduler(query_id_, query_context_, dag_graph_ptr_)
    {
    }

    void submitTasks(PlanSegment * plan_segment_ptr, const SegmentTask & task) override;
    // We do nothing on task scheduled.
    void onSegmentScheduled(const SegmentTask & task) override
    {
        (void)task;
    }
    void onSegmentFinished(const size_t & segment_id, bool is_succeed, bool is_canceled) override;
    void onQueryFinished() override;

    void updateSegmentStatusCounter(const size_t & segment_id, const UInt64 & parallel_index);

private:
    std::pair<bool, SegmentTaskInstance> getInstanceToSchedule(const AddressInfo & worker);
    void triggerDispatch(const std::vector<WorkerNode> & available_workers);
    void prepareTask(PlanSegment * plan_segment_ptr, size_t parallel_size) override;

    std::mutex segment_status_counter_mutex;
    std::unordered_map<size_t, std::unordered_set<UInt64>> segment_status_counter;

    std::mutex nodes_alloc_mutex;
    // segment id -> nodes running its instance
    std::unordered_map<size_t, std::unordered_set<AddressInfo, AddressInfo::Hash>> occupied_workers;
    // segment id -> [segment instance, node]
    std::unordered_map<size_t, std::unordered_map<UInt64, AddressInfo>> segment_parallel_locations;
    PendingTaskIntances pending_task_instances;
};

}
