/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <QueryPlan/TableScanStep.h>
#include <QueryPlan/ExecutePlanElement.h>

#include <Analyzers/TypeAnalyzer.h>
#include <Formats/FormatSettings.h>
#include <Interpreters/ActionsVisitor.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/RuntimeFilter/RuntimeFilterManager.h>
#include <Interpreters/TableJoin.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/misc.h>
#include <MergeTreeCommon/CnchBucketTableCommon.h>
#include <Optimizer/PredicateUtils.h>
#include <Optimizer/Rule/Rewrite/PushIntoTableScanRules.h>
#include <Optimizer/RuntimeFilterUtils.h>
#include <Optimizer/SymbolTransformMap.h>
#include <Optimizer/SymbolsExtractor.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTableColumnReference.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/queryToString.h>
#include <Processors/MergeTreeSelectPrepareProcessor.h>
#include <Processors/QueryPipeline.h>
#include <Processors/ResizeProcessor.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/Transforms/ExpressionTransform.h>
#include <QueryPlan/IQueryPlanStep.h>
#include <QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <QueryPlan/PlanSerDerHelper.h>
#include <QueryPlan/QueryPlan.h>
#include <QueryPlan/planning_common.h>
#include <Storages/IStorage_fwd.h>
#include <Storages/MergeTree/MergeTreeWhereOptimizer.h>
#include <Storages/RemoteFile/IStorageCnchFile.h>
#include <Storages/MergeTree/Index/TableScanExecutorWithIndex.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/StorageCloudMergeTree.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <fmt/format.h>
#include <Common/FieldVisitorToString.h>
#include "Interpreters/DatabaseCatalog.h"
#include <Common/Stopwatch.h>
#include <Interpreters/RuntimeFilter/RuntimeFilterManager.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_PREWHERE;
    extern const int INVALID_LIMIT_EXPRESSION;
    extern const int PROJECTION_SELECTION_ERROR;
}

namespace _scan_execute_impl
{
    // a struct used in projection selection with any necessary intermediate states
    struct ProjectionMatchContext;
    using ProjectionMatchContexts = std::vector<ProjectionMatchContext>;

    const UInt32 NODE_ID_TABLE_SCAN = 0;
    const UInt32 NODE_ID_FILTER = 1;
    const UInt32 NODE_ID_PROJECTION = 2;
    const UInt32 NODE_ID_AGGREGATION = 3;

    /// implementations
    struct ProjectionMatchContext
    {
        const ProjectionDescription & projection_desc;
        NamesAndTypesList missing_columns;
        SymbolTranslationMap column_translation;
        NameToType column_types;
        Assignments rewritten_assignments;
        NameToType rewritten_types;
        ASTPtr rewritten_filter;
        NameSet required_column_set;
        MergeTreeDataSelectAnalysisResultPtr read_analysis;

        ProjectionMatchContext(const ProjectionDescription & projection_desc_,
            const PartGroup & part_group,
            const IStorage * storage,
            const NamesAndTypesList & table_columns);

        ExecutePlanElement buildExecutePlanElement(PartGroup part_group, const SelectQueryInfo & query_info) const;

        Names requiredColumns() const
        {
            Names result {required_column_set.begin(), required_column_set.end()};
            return result;
        }
    };

    ProjectionMatchContext::ProjectionMatchContext(
        const ProjectionDescription & projection_desc_,
        const PartGroup & part_group,
        const IStorage * storage,
        const NamesAndTypesList & table_columns)
        : projection_desc(projection_desc_)
    {
        const auto & data_part_columns = part_group.getColumns();

        for (const auto & column: table_columns)
            if (!data_part_columns.contains(column.name))
                missing_columns.push_back(column);

        for (size_t i = 0; i < projection_desc.column_names.size(); ++i)
        {
            column_translation.addStorageTranslation(
                projection_desc.column_asts[i], projection_desc.column_names[i], storage, NODE_ID_TABLE_SCAN);
            column_types.emplace(projection_desc.column_names[i], projection_desc.data_types[i]);
        }
    }

    ExecutePlanElement ProjectionMatchContext::buildExecutePlanElement(PartGroup part_group, const SelectQueryInfo & query_info) const
    {
        std::shared_ptr<FilterStep> rewritten_filter_step = nullptr;
        std::shared_ptr<ProjectionStep> rewritten_projection_step = nullptr;
        auto require_columns = requiredColumns();

        NamesAndTypes names_and_types;
        for (const auto & column: require_columns)
            names_and_types.emplace_back(column, column_types.at(column));

        DataStream input_stream {.header = Block{names_and_types}};

        if (rewritten_filter)
            rewritten_filter_step = std::make_shared<FilterStep>(input_stream, rewritten_filter);

        rewritten_projection_step = std::make_shared<ProjectionStep>(rewritten_filter ? rewritten_filter_step->getOutputStream(): input_stream,
                                                                     rewritten_assignments, rewritten_types);

        ActionsDAGPtr prewhere_actions;

        if (query_info.prewhere_info)
        {
            auto & prewhere_info = query_info.prewhere_info;
            prewhere_actions = prewhere_info->prewhere_actions->clone();
            prewhere_actions->foldActionsByProjection(
                required_column_set, projection_desc.sample_block_for_keys, prewhere_info->prewhere_column_name, false);

            // This is hacky. Since `query_info.query` is not the original query, `prewhere_actions` may prune off
            // columns mistakenly. Here, we add used required columns back.
            // For example. projection desc: SELECT toDate(at), count() GROUP BY toDate(at)
            // query: SELECT toDate(at), count() PREWHERE toDate(at) = '2022-01-01' GROUP BY toDate(at)
            auto & outputs = prewhere_actions->getOutputs();
            for (const auto & input: prewhere_actions->getInputs())
                if (std::find(outputs.begin(), outputs.end(), input) == outputs.end())
                    outputs.emplace_back(input);
        }

        return ExecutePlanElement {
            std::move(part_group),
            read_analysis,
            &projection_desc,
            std::move(require_columns),
            rewritten_projection_step,
            rewritten_filter_step,
            prewhere_actions
        };
    }

    struct PartSchemaKey
    {
        // use ordered set to ensure hash function stable
        std::set<std::string_view> columns;
        std::set<std::string_view> projections;

        explicit PartSchemaKey(MergeTreeData::DataPartPtr part);

        bool operator==(const PartSchemaKey & other) const
        {
            return columns == other.columns && projections == other.projections;
        }

        struct Hash
        {
            size_t operator()(const PartSchemaKey & key) const
            {
                size_t res = 17;

                for (const auto & column: key.columns)
                    res = res * 31 + std::hash<std::string_view>()(column);

                for (const auto & projection: key.projections)
                    res = res * 31 + std::hash<std::string_view>()(projection);

                return res;
            }
        };
    };


    PartSchemaKey::PartSchemaKey(MergeTreeData::DataPartPtr part)
    {
        std::vector<std::string_view> columns_vector;
        std::vector<std::string_view> projections_vector;

        for (const auto & column_entry: part->getColumns())
            columns_vector.emplace_back(column_entry.name);
        for (const auto & projection_entry: part->getProjectionParts())
            projections_vector.emplace_back(projection_entry.first);

        columns.insert(columns_vector.begin(), columns_vector.end());
        projections.insert(projections_vector.begin(), projections_vector.end());
    }
}

using namespace _scan_execute_impl;

class TableScanExecutor
{
public:
    TableScanExecutor(TableScanStep & step, const MergeTreeMetaBase & storage_, ContextPtr context_);
    ExecutePlan buildExecutePlan();

private:
    bool match(ProjectionMatchContext & candidate) const;
    MergeTreeDataSelectAnalysisResultPtr estimateReadMarks(const PartGroup & part_group) const;
    void estimateReadMarksForProjection(const PartGroup & part_group, ProjectionMatchContext & candidate) const;
    void prunePartsByIndex(MergeTreeData::DataPartsVector & parts) const;
    PartGroups groupPartsBySchema(const MergeTreeData::DataPartsVector & parts);
    ASTPtr rewriteExpr(ASTPtr expr, ProjectionMatchContext & candidate) const;

    struct NameWithAST
    {
        String name;
        ASTPtr flatten_ast;
    };

    bool match_projection = false;

    const MergeTreeMetaBase & storage;
    StorageMetadataPtr storage_metadata;
    MergeTreeDataSelectExecutor merge_tree_reader;
    const SelectQueryInfo & select_query_info;
    ContextPtr context;
    Poco::Logger * log;

    bool has_aggregate;
    std::optional<SymbolTransformMap> query_lineage;
    Names query_required_columns;
    NameToType column_types_before_agg;
    std::vector<NameWithAST> aggregate_keys;
    std::vector<NameWithAST> aggregate_descs;
    ASTPtr flatten_filter;
    ASTPtr flatten_prewhere;
    std::shared_ptr<PartitionIdToMaxBlock> max_added_blocks;
};

TableScanExecutor::TableScanExecutor(TableScanStep & step, const MergeTreeMetaBase & storage_, ContextPtr context_)
    : storage(storage_)
    , storage_metadata(storage.getInMemoryMetadataPtr())
    , merge_tree_reader(storage)
    , select_query_info(step.getQueryInfo())
    , context(std::move(context_))
    , log(&Poco::Logger::get("TableScanExecutor"))
{
    if (storage_metadata->projections.empty())
        return;

    if (step.hasInlineExpressions())
        return;

    has_aggregate = step.getPushdownAggregation() != nullptr;
    query_required_columns = step.getRequiredColumns(TableScanStep::OutputAndPrewhere);
    query_lineage = [&]() {
        PlanNodePtr node;
        QueryPlanStepPtr table_scan_without_pushdown_steps = std::make_shared<TableScanStep>(
            context,
            step.getStorageID(),
            step.getColumnAlias(),
            step.getQueryInfo(),
            step.getMaxBlockSize());
        node = PlanNodeBase::createPlanNode(NODE_ID_TABLE_SCAN, table_scan_without_pushdown_steps);

        if (const auto & filter = step.getPushdownFilter())
            node = PlanNodeBase::createPlanNode(NODE_ID_FILTER, filter, {node});

        if (const auto &  projection = step.getPushdownProjection())
            node = PlanNodeBase::createPlanNode(NODE_ID_PROJECTION, projection, {node});

        if (const auto &  aggregation = step.getPushdownAggregation())
            node = PlanNodeBase::createPlanNode(NODE_ID_AGGREGATION, aggregation, {node});

        return SymbolTransformMap::buildFrom(*node);
    }();

    if (!query_lineage)
        return;

    if (has_aggregate)
    {
        const auto * query_aggregate = step.getPushdownAggregationCast();
        column_types_before_agg = query_aggregate->getInputStreams()[0].header.getNamesToTypes();

        for (const auto & origin_grouping_key: query_aggregate->getKeys())
            aggregate_keys.emplace_back(NameWithAST{origin_grouping_key, query_lineage->inlineReferences(origin_grouping_key)});

        for (const auto & query_aggregate_desc: query_aggregate->getAggregates())
            aggregate_descs.emplace_back(NameWithAST{query_aggregate_desc.column_name,
                                                     query_lineage->inlineReferences(query_aggregate_desc.column_name)});
    }

    if (const auto * query_filter_step = step.getPushdownFilterCast())
    {
        const auto & query_filter = query_filter_step->getFilter();
        flatten_filter = query_lineage->inlineReferences(query_filter);
    }

    const auto * select_query = select_query_info.getSelectQuery();

    if (auto prewhere = select_query->prewhere())
    {
        NameSet columns{step.getColumnNames().begin(), step.getColumnNames().end()};
        flatten_prewhere = IdentifierToColumnReference::rewrite(step.getStorage().get(), NODE_ID_TABLE_SCAN, prewhere);
    }

    const auto & settings = context->getSettingsRef();
    if (settings.select_sequential_consistency)
    {
        if (auto replicated = dynamic_pointer_cast<const StorageReplicatedMergeTree>(storage.shared_from_this()))
            max_added_blocks = std::make_shared<PartitionIdToMaxBlock>(replicated->getMaxAddedBlocks());
    }

    match_projection = true;
}

ExecutePlan TableScanExecutor::buildExecutePlan()
{
    if (!match_projection)
        return {};

    PartGroups part_groups;
    {
        auto parts = storage.getDataPartsVector();
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](auto & part) { return part->info.isFakeDropRangePart(); }), parts.end());

        LOG_DEBUG(log, "Num of parts before part pruning: {}", std::to_string(parts.size()));

        prunePartsByIndex(parts);

        LOG_DEBUG(log, "Num of parts after part pruning: {}", std::to_string(parts.size()));

        if (parts.size() > 100000)
            throw Exception(ErrorCodes::PROJECTION_SELECTION_ERROR, "Projection selection error: too many parts before part grouping");

        part_groups = groupPartsBySchema(parts);

        LOG_DEBUG(log, "Num of part groups before part grouping: {}", std::to_string(part_groups.size()));

        if (part_groups.size() > 100)
            throw Exception(ErrorCodes::PROJECTION_SELECTION_ERROR, "Projection selection error: Too many part groups before projection selection");
    }

    ExecutePlan execute_plan;
    auto table_columns = storage_metadata->getColumns().getAllPhysical();

    for (auto & part_group: part_groups)
    {
        ProjectionMatchContexts projection_candidates;

        // collect qualified projection candidate
        for (const auto & projection_desc: storage_metadata->projections)
            if (part_group.hasProjection(projection_desc.name))
            {
                projection_candidates.emplace_back(projection_desc, part_group, &storage, table_columns);
                if (!match(projection_candidates.back()))
                    projection_candidates.pop_back();
            }

        // select best projection by marks to read
        ProjectionMatchContext * selected_candidate = nullptr;
        size_t min_sum_marks = std::numeric_limits<size_t>::max();

        auto normal_read_result = estimateReadMarks(part_group);

        // Add 1 to base sum_marks so that we prefer projections even when they have equal number of marks to read.
        // NOTE: It is not clear if we need it. E.g. projections do not support skip index for now.
        if (!normal_read_result->error())
            min_sum_marks = normal_read_result->marks() + 1;

        for (auto & candidate: projection_candidates)
        {
            estimateReadMarksForProjection(part_group, candidate);
            if (!candidate.read_analysis->error() && candidate.read_analysis->marks() < min_sum_marks)
            {
                selected_candidate = &candidate;
                min_sum_marks = candidate.read_analysis->marks();
            }
        }

        if (selected_candidate)
            execute_plan.push_back(selected_candidate->buildExecutePlanElement(std::move(part_group), select_query_info));
        else
            execute_plan.push_back(ExecutePlanElement(part_group, std::move(normal_read_result), part_group.parts));
    }

    if (log->debug())
    {
        size_t total_parts = 0;
        size_t total_marks = 0;
        size_t normal_parts = 0;
        size_t projection_parts = 0;
        size_t marks_of_normal_parts = 0;
        size_t marks_of_projection_parts = 0;
        for (const auto & e: execute_plan)
        {
            size_t parts = e.part_group.partsNum();
            size_t marks = e.read_analysis->marks();

            total_parts += parts;
            total_marks += marks;
            if (e.projection_desc)
            {
                projection_parts += parts;
                marks_of_projection_parts += marks;
            }
            else
            {
                normal_parts += parts;
                marks_of_normal_parts += marks;
            }
        }
        String str = fmt::format(
            "data input pipeline with projection(summary): total {} parts, total {} marks, "
            "{} normal parts, {} projection parts, {} marks of normal parts, {} marks of projection parts",
            total_parts, total_marks, normal_parts, projection_parts, marks_of_normal_parts, marks_of_projection_parts);
        LOG_DEBUG(log, str);

        str = "data input pipeline with projection(detail): ";
        for (const auto & e: execute_plan)
        {
            str.push_back('\n');
            str.append(e.toString());
        }
        LOG_DEBUG(log, str);
    }

    return execute_plan;
}

bool TableScanExecutor::match(ProjectionMatchContext & candidate) const
{
    const auto & projection_desc = candidate.projection_desc;

    if (!has_aggregate && projection_desc.type == ProjectionDescription::Type::Aggregate)
        return false;

    // quick filter by required columns, proceed to match if query required columns belong to
    // projection required columns.
    //
    // e.g., given
    //   query: select toYear(date), sum(amount) from t1 group by toYear(date);
    //   projection: select toYear(date), key, sum(amount) from t1 group by toYear(date), key;
    // it produce:
    //   query required columns: date, amount
    //   projection require columns: date, key, amount
    NameSet projection_required_columns {projection_desc.required_columns.begin(), projection_desc.required_columns.end()};
    for (const auto & query_column: query_required_columns)
        if (!projection_required_columns.contains(query_column) && !candidate.missing_columns.contains(query_column))
            return false;

    // translate query column based expression to projection column based expression,
    // this is implemented by below steps:
    //   1. flatten query column based expression to table column based expression,
    //      by query column lineage information
    //   2. translate table column based expression to projection column,
    //      by projection column translation information
    //   3. if there are table columns unable to translate, either it's a missing key,
    //      rewrite to a literal with the default value of the column type; or this
    //      projection can not serve this query
    //
    // e.g. given
    //   query plan: Aggregating(key: $3, function: sum($2)) -> Projection($3 := toYear($1) + 1) -> TableScan($1 := date, $2 := amount)
    //   projection: select toYear(date), key, sum(amount) from t1 group by toYear(date), key;
    // the procedure of rewriting grouping key(i.e. `$3`) is as below:
    //   1. flatten `$3` => toYear(`$1`) + 1 => toYear(`date`) + 1
    //   2. translate toYear(`date`) + 1 => `toYear(date)` + 1
    if (projection_desc.type == ProjectionDescription::Type::Aggregate)
    {
        // match & rewrite grouping keys
        for (const auto & aggregate_key: aggregate_keys)
        {
            auto rewritten_expr = rewriteExpr(aggregate_key.flatten_ast, candidate);
            if (!rewritten_expr)
                return false;
            candidate.rewritten_assignments.emplace_back(aggregate_key.name, rewritten_expr);
            candidate.rewritten_types.emplace(aggregate_key.name, column_types_before_agg.at(aggregate_key.name));
        }

        // match & rewrite aggregates
        for (const auto & aggregate_desc: aggregate_descs)
        {
            auto projection_agg_column_opt = candidate.column_translation.tryGetTranslation(aggregate_desc.flatten_ast);
            // TODO @wangtao: support if aggregate argument are missing columns
            if (!projection_agg_column_opt)
                return false;
            auto & projection_agg_column = *projection_agg_column_opt;
            candidate.rewritten_assignments.emplace_back(aggregate_desc.name, std::make_shared<ASTIdentifier>(projection_agg_column));
            candidate.rewritten_types.emplace(aggregate_desc.name, candidate.column_types.at(projection_agg_column));
            candidate.required_column_set.emplace(projection_agg_column);
        }

        // match & rewrite where
        if (flatten_filter)
        {
            auto rewritten_filter = rewriteExpr(flatten_filter, candidate);
            if (!rewritten_filter)
                return false;
            candidate.rewritten_filter = std::move(rewritten_filter);
        }

        // partition filter always matches, since projection parts have the same partition schema as the raw parts

        // match prewhere
        // the real rewrite action(`foldActionsByProjection`) is deferred to `buildExecutePlanElement`
        if (flatten_prewhere)
        {
            auto rewritten_prewhere = rewriteExpr(flatten_prewhere, candidate);
            if (!rewritten_prewhere)
                return false;
        }

        return true;
    }
    else
    {
        // to prevent a query both use normal projection & aggregate projection
        if (has_aggregate)
            return false;

        return false;
    }
}

MergeTreeDataSelectAnalysisResultPtr TableScanExecutor::estimateReadMarks(const PartGroup & part_group) const
{
    return merge_tree_reader.estimateNumMarksToRead(part_group.parts,
        query_required_columns,
        storage_metadata,
        storage_metadata,
        select_query_info,
        context,
        context->getSettingsRef().max_threads,
        max_added_blocks);
}

void TableScanExecutor::estimateReadMarksForProjection(const PartGroup & part_group, ProjectionMatchContext & candidate) const
{
    MergeTreeData::DataPartsVector projection_parts;

    for (const auto & part: part_group.parts)
    {
        const auto & projections = part->getProjectionParts();
        auto it = projections.find(candidate.projection_desc.name);
        if (it == projections.end())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "projection not found in a part group, does part grouping perform correctly?");
        projection_parts.push_back(it->second);
    }

    candidate.read_analysis = merge_tree_reader.estimateNumMarksToRead(projection_parts,
        candidate.requiredColumns(),
        storage_metadata,
        candidate.projection_desc.metadata,
        select_query_info,
        context,
        context->getSettingsRef().max_threads,
        max_added_blocks);

}

void TableScanExecutor::prunePartsByIndex(MergeTreeData::DataPartsVector & parts) const
{
    if (parts.empty())
        return;

    auto part_values = MergeTreeDataSelectExecutor::filterPartsByVirtualColumns(storage, parts, select_query_info, context);

    if (part_values && part_values->empty())
    {
        parts.clear();
        return;
    }

    ReadFromMergeTree::AnalysisResult result;
    if (select_query_info.partition_filter)
    {
        /// If partition filter exist reconstruct query info
        SelectQueryOptions options;
        auto mutable_context = Context::createCopy(context);
        ASTPtr copy_select = select_query_info.query->clone();
        auto & copy_select_query = copy_select->as<ASTSelectQuery &>();
        std::vector<String> setting_names{"enable_partition_filter_push_down"};
        for (auto & setting_name : setting_names)
        {
            SettingChange setting;
            setting.name = setting_name;
            setting.value = Field(false);
            if (copy_select_query.settings())
            {
                auto * set_ast = copy_select_query.settings()->as<ASTSetQuery>();
                auto it = std::find_if(set_ast->changes.begin(), set_ast->changes.end(), [&](const SettingChange & change) {
                    return change.name == setting_name;
                });
                if (it != set_ast->changes.end())
                    it->value = Field(false);
                else
                    set_ast->changes.emplace_back(setting);
            }
            else
            {
                ASTSetQuery set_ast;
                set_ast.is_standalone = false;
                set_ast.changes.emplace_back(setting);
                copy_select_query.setExpression(ASTSelectQuery::Expression::SETTINGS, std::make_shared<ASTSetQuery>(set_ast));
            }
        }

        copy_select_query.setExpression(ASTSelectQuery::Expression::WHERE, select_query_info.partition_filter->clone());
        copy_select_query.setExpression(ASTSelectQuery::Expression::PREWHERE, nullptr);
        auto interpreter = std::make_shared<InterpreterSelectQuery>(copy_select, mutable_context, options);
        interpreter->execute();
        LOG_TRACE(&Poco::Logger::get("TableScanExecutor::prunePartsByIndex"), "Construct partition filter query {}", queryToString(copy_select));
        MergeTreeDataSelectExecutor::filterPartsByPartition(
            parts, part_values, storage_metadata, storage, interpreter->getQueryInfo(), context, max_added_blocks.get(), log, result.index_stats);
    }
    else
    {
        MergeTreeDataSelectExecutor::filterPartsByPartition(
            parts,
            part_values,
            storage_metadata,
            storage,
            select_query_info,
            context,
            max_added_blocks.get(),
            log,
            result.index_stats);
    }
}

PartGroups TableScanExecutor::groupPartsBySchema(const MergeTreeData::DataPartsVector & parts)
{
    std::unordered_map<PartSchemaKey, MergeTreeData::DataPartsVector, PartSchemaKey::Hash> parts_by_schema;

    for (const auto & part: parts)
    {
        PartSchemaKey key(part);
        parts_by_schema[std::move(key)].emplace_back(part);
    }

    PartGroups groups;

    for (auto & it : parts_by_schema)
        groups.emplace_back(PartGroup{std::move(it.second)});

    return groups;
}

ASTPtr TableScanExecutor::rewriteExpr(ASTPtr expr, ProjectionMatchContext & candidate) const
{
    if (auto projection_column = candidate.column_translation.tryGetTranslation(expr))
    {
        candidate.required_column_set.emplace(*projection_column);
        return std::make_shared<ASTIdentifier>(*projection_column);
    }

    if (auto * col_ref = expr->as<ASTTableColumnReference>())
    {
        if (auto pair = candidate.missing_columns.tryGetByName(col_ref->column_name))
            return std::make_shared<ASTLiteral>(pair->type->getDefault());

        return nullptr;
    }
    else if (auto * func = expr->as<ASTFunction>(); func && func->arguments && !func->arguments->children.empty())
    {
        ASTs rewritten_arguments;

        for (const auto & argument: func->arguments->children)
        {
            rewritten_arguments.push_back(rewriteExpr(argument, candidate));
            if (rewritten_arguments.back() == nullptr)
                return nullptr;
        }

        return makeASTFunction(func->name, rewritten_arguments);
    }
    else
        return expr;
}

void TableScanStep::makeSetsForIndex(const ASTPtr & node, ContextPtr context, PreparedSets & prepared_sets) const
{
    auto settings = context->getSettingsRef();
    SizeLimits size_limits_for_set(settings.max_rows_in_set, settings.max_bytes_in_set, settings.set_overflow_mode);

    if (!node || !storage || !storage->supportsIndexForIn())
        return;

    for (auto & child : node->children)
    {
        /// Don't descend into lambda functions
        const auto * func = child->as<ASTFunction>();
        if (func && func->name == "lambda")
            continue;

        makeSetsForIndex(child, context, prepared_sets);
    }

    const auto * func = node->as<ASTFunction>();
    auto metadata_snapshot = storage->getInMemoryMetadataPtr();
    if (func && functionIsInOrGlobalInOperator(func->name))
    {
        const IAST & args = *func->arguments;
        const ASTPtr & left_in_operand = args.children.at(0);

        if (storage && storage->mayBenefitFromIndexForIn(left_in_operand, context, metadata_snapshot))
        {
            const ASTPtr & arg = args.children.at(1);
            if (arg->as<ASTSubquery>() || arg->as<ASTTableIdentifier>())
            {
                // if (settings.use_index_for_in_with_subqueries)
                //     tryMakeSetForIndexFromSubquery(arg, query_options);
            }
            else
            {
                auto input = storage->getInMemoryMetadataPtr()->getColumns().getAll();
                Names output;
                output.emplace_back(left_in_operand->getColumnName());
                auto temp_actions = createExpressionActions(context, input, output, left_in_operand);
                if (temp_actions->tryFindInOutputs(left_in_operand->getColumnName()))
                {
                    makeExplicitSet(func, *temp_actions, true, context, size_limits_for_set, prepared_sets);
                }
            }
        }
    }
}

TableScanStep::TableScanStep(
    ContextPtr context,
    StorageID storage_id_,
    const NamesWithAliases & column_alias_,
    const SelectQueryInfo & query_info_,
    size_t max_block_size_,
    String alias_,
    PlanHints hints_,
    Assignments inline_expressions_,
    std::shared_ptr<AggregatingStep> aggregation_,
    std::shared_ptr<ProjectionStep> projection_,
    std::shared_ptr<FilterStep> filter_)
    : ISourceStep(DataStream{}, hints_)
    , storage_id(storage_id_)
    , column_alias(column_alias_)
    , query_info(query_info_)
    , max_block_size(max_block_size_)
    , inline_expressions(std::move(inline_expressions_))
    , pushdown_aggregation(std::move(aggregation_))
    , pushdown_projection(std::move(projection_))
    , pushdown_filter(std::move(filter_))
    , log(&Poco::Logger::get("TableScanStep"))
    , alias(alias_)
{
    storage = DatabaseCatalog::instance().getTable(storage_id, context);
    storage_id.uuid = storage->getStorageUUID();
    formatOutputStream(context);
}

void TableScanStep::formatOutputStream(ContextPtr context)
{
    column_names.clear();
    table_output_stream.header.clear();
    for (auto & item : column_alias)
    {
        column_names.emplace_back(item.first);
    }

   // TODO: in long term, we should use different constructor for server/worker
    Block header = storage->getStorageSnapshot(storage->getInMemoryMetadataPtr(), context)->getSampleBlockForColumns(getRequiredColumns());

    NameToNameMap name_to_name_map;
    for (auto & item : column_alias)
    {
        name_to_name_map[item.first] = item.second;
    }

    const auto select_expression_list = query_info.query->as<ASTSelectQuery>()->select();
    select_expression_list->children.clear();

    for (const auto & item : header)
    {
        select_expression_list->children.emplace_back(std::make_shared<ASTIdentifier>(item.name));
        if (name_to_name_map.contains(item.name))
        {
            table_output_stream.header.insert(ColumnWithTypeAndName{item.type, name_to_name_map[item.name]});
        }
    }

    TypeAnalyzer type_analyzer = TypeAnalyzer::create(context, header.getNamesToTypes());
    for (const auto & inline_expr : inline_expressions)
    {
        select_expression_list->children.emplace_back(inline_expr.second->clone());
        table_output_stream.header.insert(ColumnWithTypeAndName{type_analyzer.getType(inline_expr.second), inline_expr.first});
    }

    *output_stream = table_output_stream;

    if (pushdown_filter != nullptr)
    {
        pushdown_filter->setInputStreams({*output_stream});
        *output_stream = pushdown_filter->getOutputStream();
    }

    if (pushdown_projection != nullptr)
    {
        pushdown_projection->setInputStreams({*output_stream});
        *output_stream = pushdown_projection->getOutputStream();
    }

    if (pushdown_aggregation != nullptr)
    {
        pushdown_aggregation->setInputStreams({*output_stream});
        *output_stream = pushdown_aggregation->getOutputStream();
    }
}

void TableScanStep::optimizeWhereIntoPrewhre(ContextPtr context)
{
    auto & query = *query_info.query->as<ASTSelectQuery>();
    if (storage && query.where() && !query.prewhere())
    {
        /// PREWHERE optimization: transfer some condition from WHERE to PREWHERE if enabled and viable
        if (const auto & column_sizes = storage->getColumnSizes(); !column_sizes.empty())
        {
            /// Extract column compressed sizes.
            std::unordered_map<std::string, UInt64> column_compressed_sizes;
            for (const auto & [name, sizes] : column_sizes)
                column_compressed_sizes[name] = sizes.data_compressed;
            MergeTreeWhereOptimizer{
                query_info, context, std::move(column_compressed_sizes), storage->getInMemoryMetadataPtr(), column_names, log};
        }
    }
}

SelectQueryInfo TableScanStep::fillQueryInfo(ContextPtr context)
{
    SelectQueryInfo copy_query_info = query_info;
    makeSetsForIndex(getFilterFromQueryInfo(query_info), context, copy_query_info.sets);
    if (query_info.partition_filter)
        makeSetsForIndex(query_info.partition_filter, context, copy_query_info.sets);
    return copy_query_info;
}

ASTs cloneChildrenReplacement(ASTs ast_children_replacement)
{
    ASTs cloned_replacement;
    cloned_replacement.reserve(ast_children_replacement.size());
    for (const auto & ast_child : ast_children_replacement)
    {
        cloned_replacement.push_back(ast_child->clone());
    }
    return cloned_replacement;
}

void TableScanStep::rewriteInForBucketTable(ContextPtr context) const
{
    // todo: remove dynamic cast
    const auto * cloud_merge_tree = dynamic_cast<StorageCloudMergeTree *>(storage.get());
    if (!cloud_merge_tree)
        return;

    if (!storage->isBucketTable() || !context->getSettingsRef().optimize_skip_unused_shards)
        return;

    auto metadata_snapshot = cloud_merge_tree->getInMemoryMetadataPtr();
    const bool need_optimise
        = metadata_snapshot->getColumnsForClusterByKey().size() == 1 && !cloud_merge_tree->getRequiredBucketNumbers().empty();
    if (!need_optimise)
        return;

    auto * query = query_info.query->as<ASTSelectQuery>();

    // NOTE: Have try-catch for every rewrite_in in order to find the WHERE clause that caused the error
    RewriteInQueryVisitor::Data data;
    LOG_TRACE(log, "Before rewriteInForBucketTable:\n: {}", query->dumpTree());
    if (query->where())
    {
        auto ast_children_replacement = cloud_merge_tree->convertBucketNumbersToAstLiterals(query->where(), context);
        if (!ast_children_replacement.empty())
        {
            data.ast_children_replacement = cloneChildrenReplacement(ast_children_replacement);
            ASTPtr & where = query->refWhere();
            RewriteInQueryVisitor(data).visit(where);
        }
    }
    if (query->prewhere())
    {
        auto ast_children_replacement = cloud_merge_tree->convertBucketNumbersToAstLiterals(query->prewhere(), context);
        if (!ast_children_replacement.empty())
        {
            data.ast_children_replacement = cloneChildrenReplacement(ast_children_replacement);
            ASTPtr & pre_where = query->refPrewhere();
            RewriteInQueryVisitor(data).visit(pre_where);
        }
    }
    LOG_TRACE(log, "After rewriteInForBucketTable:\n: {}", query->dumpTree());
}

bool TableScanStep::rewriteDynamicFilterIntoPrewhere(ASTSelectQuery * query)
{
    if (auto filter = query->getWhere())
    {
        auto filters = RuntimeFilterUtils::extractRuntimeFilters(filter);
        if (filters.first.empty())
            return false;

        std::vector<ConstASTPtr> prewhere_predicates;
        std::vector<ConstASTPtr> where_predicates{filters.second.begin(), filters.second.end()};
        std::vector<RuntimeFilterDescription> descriptions;
        for (const auto & predicate : filters.first)
            descriptions.emplace_back(RuntimeFilterUtils::extractDescription(predicate).value());

        auto it = std::max_element(descriptions.begin(), descriptions.end(),
                                   [](const auto & lhs, const auto & rhs) {
            return lhs.filter_factor < rhs.filter_factor;
        });

        for (size_t i = 0; i < filters.first.size(); ++i)
        {
            if (ASTEquality::compareTree(descriptions[i].expr, it->expr) && storage->supportsPrewhere())
            {
                prewhere_predicates.emplace_back(filters.first[i]);
                continue;
            }

            where_predicates.emplace_back(filters.first[i]);
        }

        if (auto prewhere = query->getPrewhere())
            prewhere_predicates.emplace_back(prewhere);

        if (!prewhere_predicates.empty())
        {
            auto prewhere = PredicateUtils::combineConjuncts(prewhere_predicates);
            LOG_DEBUG(log, "after rewrite runtime filter into prewhere, query prewhere: {}", prewhere->getColumnName());
            query->setExpression(ASTSelectQuery::Expression::PREWHERE, std::move(prewhere));
        }
        else
        {
            LOG_DEBUG(log, "after rewrite runtime filter into prewhere, query prewhere: nullptr");
            query->setExpression(ASTSelectQuery::Expression::PREWHERE, nullptr);
        }

        if (!where_predicates.empty())
        {
            auto where = PredicateUtils::combineConjuncts(where_predicates);
            LOG_DEBUG(log, "after rewrite runtime filter into praewhere, query where: {}", where->getColumnName());
            query->setExpression(ASTSelectQuery::Expression::WHERE, std::move(where));
        }
        else
        {
            LOG_DEBUG(log, "after rewrite runtime filter into prewhere, query where: nullptr");
            query->setExpression(ASTSelectQuery::Expression::WHERE, nullptr);
        }

        return true;
    }
    return false;
}

void TableScanStep::initializePipeline(QueryPipeline & pipeline, const BuildQueryPipelineSettings & build_context)
{
    auto * query = query_info.query->as<ASTSelectQuery>();
    bool use_expand_pipe = build_context.is_expand;
    if (!build_context.is_expand && query->getWhere()
        && build_context.context->getSettingsRef().enable_runtime_filter_pipeline_poll)
    {
        auto && ids = RuntimeFilterUtils::extractRuntimeFilterId(query->getWhere());
        if (!ids.empty())
        {
            Pipe pipe(std::make_shared<MergeTreeSelectPrepareProcessor>(
                *this,
                build_context,
                table_output_stream.header,
                std::move(ids),
                build_context.context->getSettingsRef().wait_runtime_filter_timeout));
            pipeline.init(std::move(pipe));
            pipeline.addTransform(std::make_shared<ResizeProcessor>(
                table_output_stream.header, 1, build_context.context->getSettingsRef().max_threads));
            return;
        }
    }

    Stopwatch stage_watch, total_watch;
    total_watch.start();
    stage_watch.start();
    storage = DatabaseCatalog::instance().getTable(storage_id, build_context.context);

    auto * merge_tree_storage = dynamic_cast<MergeTreeMetaBase *>(storage.get());
    bool is_merge_tree = merge_tree_storage != nullptr;

    bool use_projection_index = build_context.context->getSettingsRef().optimizer_index_projection_support && is_merge_tree
        && build_context.context->getSettingsRef().enable_ab_index_optimization;

    bool use_optimizer_projection_selection
        = build_context.context->getSettingsRef().optimizer_projection_support && is_merge_tree && !use_projection_index;

    rewriteInForBucketTable(build_context.context);
    stage_watch.start();
    bool need_rw = rewriteDynamicFilterIntoPrewhere(query);
    if (need_rw)
    {
        const auto & setting = build_context.context->getSettingsRef();
        size_t wait_ms = use_expand_pipe ? 0 : setting.wait_runtime_filter_timeout;
        if (auto where = query->getWhere())
            query->setExpression(
                ASTSelectQuery::Expression::WHERE,
                rewriteRuntimeFilter(
                    where, build_context.distributed_settings.query_id, wait_ms, false, false, setting.enable_range_cover));

        if (auto prewhere = query->getPrewhere())
            query->setExpression(
                ASTSelectQuery::Expression::PREWHERE,
                rewriteRuntimeFilter(
                    prewhere,
                    build_context.distributed_settings.query_id,
                    wait_ms,
                    true,
                    setting.runtime_filter_rewrite_bloom_filter_into_prewhere,
                    setting.enable_range_cover));
        LOG_DEBUG(log, "init pipeline stage wait runtime filter cost {} ms", stage_watch.elapsedMillisecondsAsDouble());
    }

    // todo support _shard_num rewrite
    // if ()
    // {
    //     VirtualColumnUtils::rewriteEntityInAst(query_info.query, "_shard_num", shard_info.shard_num, "toUInt32");
    // }

    /**
     * reconstuct query level info based on query
     */
    SelectQueryOptions options;
    options.distributedStages();
    if (use_optimizer_projection_selection)
        options.ignoreProjections();

    stage_watch.restart();
    ASTPtr partition_filter;
    if (query_info.partition_filter)
        partition_filter = query_info.partition_filter->clone();
    auto interpreter = std::make_shared<InterpreterSelectQuery>(query_info.query, build_context.context, options);
    interpreter->execute(true);
    query_info = interpreter->getQueryInfo();
    query_info = fillQueryInfo(build_context.context);
    LOG_DEBUG(log, "init pipeline stage run time: make up query info, {} ms", stage_watch.elapsedMillisecondsAsDouble());

    // always do filter underneath, as WHERE filter won't reuse PREWHERE result in optimizer mode
    if (query_info.prewhere_info)
        query_info.prewhere_info->need_filter = true;

    query_info.partition_filter = partition_filter;

    if (use_projection_index)
    {
        auto storage_snapshot = storage->getStorageSnapshot(storage->getInMemoryMetadataPtr(), build_context.context);
        auto input_columns_block = storage_snapshot->getSampleBlockForColumns(getRequiredColumns());
        auto input_columns = input_columns_block.getNamesAndTypesList();
        auto required_columns_block = storage_snapshot->getSampleBlockForColumns(getRequiredColumns(OutputAndPrewhere));
        auto required_columns = required_columns_block.getNamesAndTypesList();

        if (log->debug())
        {
            String msg;

            for (const auto & ass : inline_expressions)
            {
                msg += fmt::format("from: {}, source: {}\n", ass.first, queryToString(*ass.second));
            }

            auto output_types = table_output_stream.getNamesToTypes();
            for (const auto & ass : inline_expressions)
            {
                msg += fmt::format("name: {}, type: {}\n", ass.first, output_types.at(ass.first)->getName());
            }

            msg += fmt::format(
                "actions: {}\n", ProjectionStep::createActions(inline_expressions, input_columns, build_context.context)->dumpDAG());

            // msg += fmt::format("output_columns: {}\n required_columns: {}", column_alias, required_columns.toString());

            LOG_DEBUG(log, fmt::format("pushdown_index_projection: {}", msg));
        }

        MergeTreeIndexInfo::BuildIndexContext index_building_context{
                .input_columns = input_columns,
                .required_columns = required_columns,
                .context = build_context.context,
                .prepared_sets = query_info.sets,
                .outputs = column_alias /* no meaning */};

        query_info.index_context = MergeTreeIndexContext::buildFromProjection(inline_expressions, index_building_context, storage_snapshot->metadata);
    }

    ExecutePlan execute_plan;

    stage_watch.restart();
    if (use_optimizer_projection_selection)
        execute_plan = TableScanExecutor(*this, *merge_tree_storage, build_context.context).buildExecutePlan();
    else if (use_projection_index)
        execute_plan = TableScanExecutorWithIndex(*this, build_context.context).buildExecutePlan();
    LOG_DEBUG(log, "init pipeline stage run time: projection match, {} ms", stage_watch.elapsedMillisecondsAsDouble());

    size_t max_streams = build_context.context->getSettingsRef().max_threads;
    if (max_block_size < build_context.context->getSettingsRef().max_block_size)
        max_streams = 1; // single block single stream.

    if (max_streams > 1 && !storage->isRemote())
        max_streams *= build_context.context->getSettingsRef().max_streams_to_max_threads_ratio;

    if (execute_plan.empty())
    {
        auto storage_snapshot = storage->getStorageSnapshot(storage->getInMemoryMetadataPtr(), build_context.context);
        auto pipe = storage->read(
            interpreter->getRequiredColumns(), storage_snapshot, query_info, build_context.context, QueryProcessingStage::Enum::FetchColumns, max_block_size, max_streams);

        QueryPlanStepPtr step;
        if (pipe.empty())
        {
            auto header = storage_snapshot->getSampleBlockForColumns(getRequiredColumns());
            auto null_pipe = InterpreterSelectQuery::generateNullSourcePipe(header, query_info);
            auto read_from_pipe = std::make_shared<ReadFromPreparedSource>(std::move(null_pipe));
            read_from_pipe->setStepDescription("Read from NullSource");
            step = read_from_pipe;
        }
        else
            step = std::make_shared<ReadFromStorageStep>(std::move(pipe), step_description);


        if (auto * source = dynamic_cast<ISourceStep *>(step.get()))
            source->initializePipeline(pipeline, build_context);

        aliasColumns(pipeline, build_context, "normal read");
        setQuotaAndLimits(pipeline, options, build_context);

        if (auto * filter_step = getPushdownFilterCast())
            filter_step->transformPipeline(pipeline, build_context);
        if (auto * projection_step = getPushdownProjectionCast())
            projection_step->transformPipeline(pipeline, build_context);
        if (auto * aggregate_step = getPushdownAggregationCast())
            aggregate_step->transformPipeline(pipeline, build_context);

        LOG_DEBUG(log, "init pipeline total run time: {} ms", total_watch.elapsedMillisecondsAsDouble());
        return;
    }

    MergeTreeDataSelectExecutor merge_tree_reader{*merge_tree_storage};
    auto metadata_snapshot = storage->getInMemoryMetadataPtr();
    auto context = build_context.context;
    auto storage_snapshot = storage->getStorageSnapshot(metadata_snapshot, context);
    Pipes pipes;

    // num of pipes may be smaller than num of plan elements since MergeTreeDataSelectExecutor
    // can infer an empty result for a part group. hence we record a mapping of pipe->plan element
    std::vector<size_t> plan_element_ids;
    size_t total_output_ports = 0;

    for (size_t plan_element_id = 0; plan_element_id < execute_plan.size(); ++plan_element_id)
    {
        const auto & plan_element = execute_plan[plan_element_id];
        bool use_projection = plan_element.projection_desc != nullptr;
        QueryPipelinePtr sub_pipeline;

        LOG_DEBUG(log, fmt::format("plan_element: {}", plan_element.toString()));

        if (plan_element.read_analysis->marks() == 0)
            continue;

        if (use_projection)
        {
            SelectQueryInfo projection_query_info = query_info;
            if (projection_query_info.prewhere_info)
            {
                projection_query_info.prewhere_info = std::make_shared<PrewhereInfo>(*projection_query_info.prewhere_info);
                projection_query_info.prewhere_info->prewhere_actions = plan_element.prewhere_actions;
            }
            MergeTreeData::DeleteBitmapGetter null_getter = [](auto & /*part*/) { return nullptr; };
            auto proj_snapshot = std::make_shared<StorageSnapshot>(
                storage_snapshot->storage, storage_snapshot->metadata, storage_snapshot->object_columns);
            proj_snapshot->addProjection(plan_element.projection_desc);

            auto read_plan = merge_tree_reader.readFromParts(
                {},
                null_getter,
                plan_element.projection_required_columns,
                proj_snapshot,
                projection_query_info,
                context,
                max_block_size,
                max_streams,
                nullptr,
                plan_element.read_analysis);

            sub_pipeline = read_plan->buildQueryPipeline(QueryPlanOptimizationSettings::fromContext(context),
                                                         BuildQueryPipelineSettings::fromContext(context));

            if (plan_element.rewritten_filter_step)
                plan_element.rewritten_filter_step->transformPipeline(*sub_pipeline, build_context);

            if (plan_element.rewritten_projection_step)
                plan_element.rewritten_projection_step->transformPipeline(*sub_pipeline, build_context);
        }
        else
        {
            MergeTreeMetaBase::DeleteBitmapGetter delete_bitmap_getter;
            if (metadata_snapshot->hasUniqueKey())
            {
                /// get a consistent snapshot of delete bitmaps for query,
                /// otherwise concurrent upserts that modify part's delete bitmap will cause incorrect query result
                auto delete_bitmap_snapshot = merge_tree_storage->getLatestDeleteSnapshot(plan_element.parts);
                /// move delete_bitmap_snapshot into the closure because delete_bitmap_getter will be used after this function returns
                delete_bitmap_getter = [snapshot = std::move(delete_bitmap_snapshot)](const auto & part) -> ImmutableDeleteBitmapPtr
                {
                    if (auto it = snapshot.find(part); it != snapshot.end())
                        return it->second;
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "Not found delete bitmap for part " + part->name);
                };
            }
            else
            {
                delete_bitmap_getter = [](const auto & part) { return part->getDeleteBitmap(); };
            }

            auto query_info_for_index = query_info;
            query_info_for_index.read_bitmap_index = plan_element.read_bitmap_index;

            auto read_plan = merge_tree_reader.readFromParts(
                {},
                delete_bitmap_getter,
                getRequiredColumns(),
                storage_snapshot,
                query_info_for_index,
                context,
                max_block_size,
                max_streams,
                nullptr,
                plan_element.read_analysis);

            sub_pipeline = read_plan->buildQueryPipeline(QueryPlanOptimizationSettings::fromContext(context),
                                                         BuildQueryPipelineSettings::fromContext(context));

            String pipeline_name = "execute plan read";
            if (plan_element.read_bitmap_index)
                pipeline_name += "(with index)";
            aliasColumns(*sub_pipeline, build_context, "execute plan read");

            if (pushdown_filter)
                getPushdownFilterCast()->transformPipeline(*sub_pipeline, build_context);

            if (pushdown_projection)
                getPushdownProjectionCast()->transformPipeline(*sub_pipeline, build_context);
        }

        auto pipe = QueryPipeline::getPipe(std::move(*sub_pipeline));
        total_output_ports += pipe.numOutputPorts();
        pipes.emplace_back(std::move(pipe));
        plan_element_ids.emplace_back(plan_element_id);
    }

    if (pushdown_aggregation)
    {
        const auto & settings = context->getSettingsRef();
        const auto & aggregate_step = *getPushdownAggregationCast();
        const auto & aggregate_param = aggregate_step.getParams();
        auto many_data = std::make_shared<ManyAggregatedData>(total_output_ports);
        size_t counter = 0;
        AggregatorListPtr aggregator_list_ptr = std::make_shared<AggregatorList>();

        for (size_t pipe_id = 0; pipe_id < pipes.size(); ++pipe_id)
        {
            auto & pipe = pipes[pipe_id];
            const auto & plan_element = execute_plan[plan_element_ids.at(pipe_id)];
            bool use_projection = plan_element.projection_desc != nullptr;

            const auto & header_before_aggregation = pipe.getHeader();

            ColumnNumbers keys;
            for (const auto & key : aggregate_step.getKeys())
                keys.push_back(header_before_aggregation.getPositionByName(key));

            AggregateDescriptions aggregates = aggregate_step.getAggregates();
            if (!use_projection)
            {
                for (auto & descr : aggregates)
                    if (descr.arguments.empty())
                        for (const auto & name : descr.argument_names)
                            descr.arguments.push_back(header_before_aggregation.getPositionByName(name));
            }

            AggregatingTransformParamsPtr transform_params;

            if (use_projection)
            {
                Aggregator::Params params(
                    header_before_aggregation,
                    keys,
                    aggregates,
                    aggregate_param.overflow_row,
                    settings.max_rows_to_group_by,
                    settings.group_by_overflow_mode,
                    settings.group_by_two_level_threshold,
                    settings.group_by_two_level_threshold_bytes,
                    settings.max_bytes_before_external_group_by,
                    settings.spill_buffer_bytes_before_external_group_by,
                    settings.empty_result_for_aggregation_by_empty_set,
                    context->getTemporaryVolume(),
                    settings.max_threads,
                    settings.min_free_disk_space_for_temporary_data,
                    settings.compile_expressions,
                    settings.min_count_to_compile_aggregate_expression,
                    header_before_aggregation,
                    settings.enable_lc_group_by_opt); // The source header is also an intermediate header

                transform_params = std::make_shared<AggregatingTransformParams>(std::move(params), aggregator_list_ptr, aggregate_step.isFinal());

                /// This part is hacky.
                /// We want AggregatingTransform to work with aggregate states instead of normal columns.
                /// It is almost the same, just instead of adding new data to aggregation state we merge it with existing.
                ///
                /// It is needed because data in projection:
                /// * is not merged completely (we may have states with the same key in different parts)
                /// * is not split into buckets (so if we just use MergingAggregated, it will use single thread)
                transform_params->only_merge = true;
            }
            else
            {
                Aggregator::Params params(
                    header_before_aggregation,
                    keys,
                    aggregates,
                    aggregate_param.overflow_row,
                    settings.max_rows_to_group_by,
                    settings.group_by_overflow_mode,
                    settings.group_by_two_level_threshold,
                    settings.group_by_two_level_threshold_bytes,
                    settings.max_bytes_before_external_group_by,
                    settings.spill_buffer_bytes_before_external_group_by,
                    settings.empty_result_for_aggregation_by_empty_set,
                    context->getTemporaryVolume(),
                    settings.max_threads,
                    settings.min_free_disk_space_for_temporary_data,
                    settings.compile_aggregate_expressions,
                    settings.min_count_to_compile_aggregate_expression,
                    {},
                    settings.enable_lc_group_by_opt);

                transform_params = std::make_shared<AggregatingTransformParams>(std::move(params), aggregator_list_ptr, aggregate_step.isFinal());
            }

            pipe.resize(pipe.numOutputPorts(), true, true);

            auto merge_threads = max_streams;
            auto temporary_data_merge_threads = settings.aggregation_memory_efficient_merge_threads
                ? static_cast<size_t>(settings.aggregation_memory_efficient_merge_threads)
                : static_cast<size_t>(settings.max_threads);

            pipe.addSimpleTransform([&](const Block & header)
            {
                return std::make_shared<AggregatingTransform>(
                    header, transform_params, many_data, counter++, merge_threads, temporary_data_merge_threads);
            });
        }
    }

    Pipe pipe;

    if (!pipes.empty())
    {
        pipe = Pipe::unitePipes(std::move(pipes));
        pipe.resize(1);
    }
    else
    {
        pipe = Pipe(std::make_shared<NullSource>(output_stream->header));
    }

    auto step = std::make_unique<ReadFromStorageStep>(std::move(pipe), "ExecutePlanStep");
    step->initializePipeline(pipeline, build_context);

    setQuotaAndLimits(pipeline, options, build_context);

    std::ostringstream step_desc;
    bool first = true;
    for (auto & plan_element: execute_plan)
    {
        if (first)
            first = false;
        else
            step_desc << ", ";

        if (plan_element.projection_desc)
            step_desc << plan_element.part_group.partsNum() << " parts from projection `" << plan_element.projection_desc->name << "`";
        else if (plan_element.read_bitmap_index)
            step_desc << plan_element.part_group.partsNum() << " parts from bitmap index";
        else
            step_desc << plan_element.part_group.partsNum() << " parts from raw data";
    }
    setStepDescription(step_desc.str());

    LOG_DEBUG(log, "init pipeline total run time: {} ms, table scan descriptiion: {}", total_watch.elapsedMillisecondsAsDouble(), step_desc.str());
}

void TableScanStep::toProto(Protos::TableScanStep & proto, bool) const
{
    storage_id.toProto(*proto.mutable_storage_id());
    for (auto & [name, alias] : column_alias)
    {
        auto proto_element = proto.add_column_alias();
        proto_element->set_name(name);
        proto_element->set_alias(alias);
    }

    query_info.toProto(*proto.mutable_query_info());
    proto.set_max_block_size(max_block_size);

    serializeAssignmentsToProto(inline_expressions, *proto.mutable_inline_expressions());

    if (pushdown_aggregation)
    {
        pushdown_aggregation->toProto(*proto.mutable_pushdown_aggregation());
    }
    if (pushdown_projection)
    {
        pushdown_projection->toProto(*proto.mutable_pushdown_projection());
    }
    if (pushdown_filter)
    {
        pushdown_filter->toProto(*proto.mutable_pushdown_filter());
    }
}

std::shared_ptr<TableScanStep> TableScanStep::fromProto(const Protos::TableScanStep & proto, ContextPtr context)
{
    auto storage_id = StorageID::fromProto(proto.storage_id(), context);
    NamesWithAliases column_alias;
    for (const auto & proto_element : proto.column_alias())
    {
        auto name = proto_element.name();
        auto alias = proto_element.alias();
        column_alias.emplace_back(name, alias);
    }
    SelectQueryInfo query_info;
    query_info.fillFromProto(proto.query_info());
    auto max_block_size = proto.max_block_size();
    auto inline_expressions = deserializeAssignmentsFromProto(proto.inline_expressions());

    std::shared_ptr<AggregatingStep> pushdown_aggregation;
    std::shared_ptr<ProjectionStep> pushdown_projection;
    std::shared_ptr<FilterStep> pushdown_filter;
    if (proto.has_pushdown_aggregation())
        pushdown_aggregation = AggregatingStep::fromProto(proto.pushdown_aggregation(), context);

    if (proto.has_pushdown_projection())
        pushdown_projection = ProjectionStep::fromProto(proto.pushdown_projection(), context);
    if (proto.has_pushdown_filter())
        pushdown_filter = FilterStep::fromProto(proto.pushdown_filter(), context);
    auto step = std::make_shared<TableScanStep>(
        context,
        storage_id,
        column_alias,
        query_info,
        max_block_size,
        String{} /*alias*/,
        PlanHints{},
        inline_expressions,
        pushdown_aggregation,
        pushdown_projection,
        pushdown_filter);
    return step;
}

std::shared_ptr<IQueryPlanStep> TableScanStep::copy(ContextPtr /*context*/) const
{
    SelectQueryInfo copy_query_info = query_info; // fixme@kaixi: deep copy here
    copy_query_info.query = query_info.query->clone();
    auto new_prewhere = copy_query_info.query->as<ASTSelectQuery &>().prewhere();

    return std::make_unique<TableScanStep>(
        output_stream.value(),
        storage,
        storage_id,
        original_table,
        column_names,
        column_alias,
        copy_query_info,
        max_block_size,
        alias,
        hints,
        inline_expressions,
        pushdown_aggregation,
        pushdown_projection,
        pushdown_filter,
        table_output_stream);
}

std::shared_ptr<IStorage> TableScanStep::getStorage() const
{
    return storage;
}

void TableScanStep::cleanStorage()
{
    if (storage)
    {
        storage_id = storage->getStorageID();
        storage = nullptr;
    }
}

void TableScanStep::allocate(ContextPtr context)
{
    // init query_info.syntax_analyzer
    if (!query_info.syntax_analyzer_result)
    {
        auto storage_snapshot = storage->getStorageSnapshot(storage->getInMemoryMetadataPtr(), context);
        Block header = storage_snapshot->getSampleBlockForColumns(getRequiredColumns());

        auto tree_rewriter_result
            = std::make_shared<TreeRewriterResult>(header.getNamesAndTypesList(), storage, storage_snapshot);
        tree_rewriter_result->required_source_columns = header.getNamesAndTypesList();
        tree_rewriter_result->analyzed_join = std::make_shared<TableJoin>();
        query_info.syntax_analyzer_result = tree_rewriter_result;
    }

    original_table = storage_id.table_name;
    storage_id = storage->prepareTableRead(getRequiredColumns(), query_info, context);

    // update query info
    if (query_info.query)
    {
        query_info = fillQueryInfo(context);
        /// trigger preallocate tables
        // if (!cnch->isOnDemandMode())
        // cnch->read(column_names, query_info, context, processed_stage, max_block_size, 1);
        // cnch->genPlanSegmentQueryAndAllocate(column_names, query_info, context);
        auto db_table = getDatabaseAndTable(query_info.query->as<ASTSelectQuery &>(), 0);
        if (!db_table->table.empty())
        {
            if (db_table->table != storage_id.table_name)
            {
                if (query_info.query)
                {
                    ASTSelectQuery & select_query = query_info.query->as<ASTSelectQuery &>();
                    select_query.replaceDatabaseAndTable(storage_id);
                }
            }
        }
    }
}

bool TableScanStep::hasLimit() const
{
    auto * query = query_info.query->as<ASTSelectQuery>();
    return query->limitLength().get();
}

static UInt64 getUIntValue(const ASTPtr & node, const ContextPtr & context)
{
    const auto & [field, type] = evaluateConstantExpression(node, context);

    if (!isNativeNumber(type))
        throw Exception(
            "Illegal type " + type->getName() + " of LIMIT expression, must be numeric type", ErrorCodes::INVALID_LIMIT_EXPRESSION);

    Field converted = convertFieldToType(field, DataTypeUInt64());
    if (converted.isNull())
        throw Exception(
            "The value " + applyVisitor(FieldVisitorToString(), field) + " of LIMIT expression is not representable as UInt64",
            ErrorCodes::INVALID_LIMIT_EXPRESSION);

    return converted.safeGet<UInt64>();
}

bool TableScanStep::setLimit(size_t limit, const ContextMutablePtr & context)
{
    auto & query = *query_info.query->as<ASTSelectQuery>();
    auto limit_length = query.getLimitLength();
    if (limit_length)
    {
        if (getUIntValue(limit_length, context) <= limit)
            return false;
    }
    query.setExpression(ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<ASTLiteral>(limit));

    if (!query.distinct && !query.prewhere() && !query.where() && !query.groupBy() && !query.having() && !query.orderBy()
        && !query.limitBy() && query.limitLength() && limit < max_block_size)
    {
        max_block_size = std::max(static_cast<size_t>(1), limit);
    }

    return true;
}

size_t TableScanStep::getMaxBlockSize() const
{
    return max_block_size;
}

ASTPtr TableScanStep::rewriteRuntimeFilter(
    const ASTPtr & filter, const String & query_id, size_t wait_ms, bool is_prewhere, bool enable_bf, bool range_cover)
{
    if (!filter)
        return nullptr;

    Stopwatch timer{CLOCK_MONOTONIC_COARSE};
    timer.start();

    auto filters = RuntimeFilterUtils::extractRuntimeFilters(filter);
    if (filters.first.empty())
        return filter;

    LOG_DEBUG(log, "runtime filter {} before rewrite: {}", is_prewhere ? "prewhere" : "where", filter->getColumnName());

    std::vector<ConstASTPtr> predicates = std::move(filters.second);
    for (auto & runtime_filter : filters.first)
    {
        auto description = RuntimeFilterUtils::extractDescription(runtime_filter).value();
        auto runtime_filters = RuntimeFilterUtils::createRuntimeFilterForTableScan(description, query_id, wait_ms, enable_bf, range_cover);
        predicates.insert(predicates.end(), runtime_filters.begin(), runtime_filters.end());
    }

    auto res = PredicateUtils::combineConjuncts(predicates);
    LOG_DEBUG(log, "runtime filter {} after rewrite: {}. with {} ms", is_prewhere ? "prewhere" : "where", res->getColumnName(), timer.elapsedMilliseconds());
    return res;
}

void TableScanStep::aliasColumns(QueryPipeline & pipeline, const BuildQueryPipelineSettings & build_context, const String & pipeline_name)
{
    const auto & cur_header = pipeline.getHeader();
    const auto & header_for_alias = table_output_stream.header;
    LOG_DEBUG(
        log,
        fmt::format(
            "aliasColumns({}), current header: {}, desired header: {}",
            pipeline_name,
            cur_header.dumpStructure(),
            header_for_alias.dumpStructure()));

    if (!blocksHaveEqualStructure(cur_header, header_for_alias))
    {
        NamesWithAliases aliases;
        ASTPtr select = std::make_shared<ASTExpressionList>();

        for (const auto & column : column_alias)
        {
            select->children.emplace_back(std::make_shared<ASTIdentifier>(column.first));
            aliases.emplace_back(column);
        }

        for (const auto & inline_expr : inline_expressions)
        {
            auto expr_col_name = inline_expr.second->getColumnName();
            if (cur_header.has(expr_col_name))
                select->children.emplace_back(std::make_shared<ASTIdentifier>(expr_col_name));
            else
                select->children.emplace_back(inline_expr.second->clone());
            aliases.emplace_back(expr_col_name, inline_expr.first);
        }

        auto actions = createExpressionActions(build_context.context, cur_header.getNamesAndTypesList(), aliases, select, true);
        LOG_DEBUG(log, fmt::format("aliasColumns({}), actions: {}", pipeline_name, actions->dumpDAG()));
        auto expression = std::make_shared<ExpressionActions>(actions, build_context.getActionsSettings());
        pipeline.addSimpleTransform(
            [&](const Block & header) -> ProcessorPtr { return std::make_shared<ExpressionTransform>(header, expression); });
    }
}

void TableScanStep::setQuotaAndLimits(QueryPipeline & pipeline, const SelectQueryOptions & options, const BuildQueryPipelineSettings & build_context)
{
    auto context = build_context.context;
    const auto & settings = context->getSettingsRef();

    StreamLocalLimits limits;
    SizeLimits leaf_limits;
    std::shared_ptr<const EnabledQuota> quota;

    /// Set the limits and quota for reading data, the speed and time of the query.
    if (!options.ignore_limits)
    {
        limits = getLimitsForStorage(settings, options);
        leaf_limits = SizeLimits(settings.max_rows_to_read_leaf, settings.max_bytes_to_read_leaf, settings.read_overflow_mode_leaf);
    }

    if (!options.ignore_quota && (options.to_stage == QueryProcessingStage::Complete))
        quota = context->getQuota();

    auto table_lock = storage->lockForShare(context->getInitialQueryId(), context->getSettingsRef().lock_acquire_timeout);


    /// Table lock is stored inside pipeline here.
    pipeline.setLimits(limits);

    /**
      * Leaf size limits should be applied only for local processing of distributed queries.
      * Such limits allow to control the read stage on leaf nodes and exclude the merging stage.
      * Consider the case when distributed query needs to read from multiple shards. Then leaf
      * limits will be applied on the shards only (including the root node) but will be ignored
      * on the results merging stage.
      */
    if (!storage->isRemote())
        pipeline.setLeafLimits(leaf_limits);

    if (quota)
        pipeline.setQuota(quota);

    /// Order of resources below is important.
    if (context)
        pipeline.addInterpreterContext(std::move(context));

    if (storage)
        pipeline.addStorageHolder(storage);

    if (table_lock)
        pipeline.addTableLock(std::move(table_lock));

    for (const auto & processor : pipeline.getProcessors())
        processors.emplace_back(processor);
}

void TableScanStep::setReadOrder(SortDescription read_order)
{
    if (!read_order.empty())
    {
        query_info.input_order_info = std::make_shared<InputOrderInfo>(read_order, read_order[0].direction);
    }
}

Names TableScanStep::getRequiredColumns(GetFlags flags) const
{
    LinkedHashSet<String> result;

    auto add_columns_in_expr = [&](const auto & expr) {
        auto columns = SymbolsExtractor::extract(expr);
        result.insert(columns.begin(), columns.end());
    };

    if (flags & GetFlags::Output)
        result.insert(column_names.begin(), column_names.end());

    if (flags & GetFlags::Prewhere)
        if (auto prewhere = getPrewhere())
            add_columns_in_expr(prewhere);

    if (flags & GetFlags::BitmapIndex)
    {
        for (const auto & item : inline_expressions)
        {
            const auto * func = item.second->as<ASTFunction>();
            if (func && Poco::toLower(func->name) == "arraysetcheck")
                add_columns_in_expr(item.second);
        }
    }

    return {result.begin(), result.end()};
}

bool TableScanStep::hasPrewhere() const
{
    return getPrewhere() != nullptr;
}

ASTPtr TableScanStep::getPrewhere() const
{
    return query_info.getSelectQuery()->prewhere();
}

void TableScanStep::setInlineExpressions(Assignments new_inline_expressions, ContextPtr context)
{
    inline_expressions = std::move(new_inline_expressions);
    formatOutputStream(context);
}

NameToNameMap TableScanStep::getColumnToAliasMap() const
{
    NameToNameMap ret;
    for (const auto & column_to_alias : column_alias)
        ret.emplace(column_to_alias.first, column_to_alias.second);
    return ret;
}

NameToNameMap TableScanStep::getAliasToColumnMap() const
{
    NameToNameMap ret;
    for (const auto & column_to_alias : column_alias)
        ret.emplace(column_to_alias.second, column_to_alias.first);
    return ret;
}
}
