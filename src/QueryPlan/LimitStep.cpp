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

#include <QueryPlan/LimitStep.h>
#include <Processors/QueryPipeline.h>
#include <Processors/LimitTransform.h>
#include <IO/Operators.h>
#include <Common/JSONBuilder.h>

namespace DB
{

static ITransformingStep::Traits getTraits()
{
    return ITransformingStep::Traits
    {
        {
            .preserves_distinct_columns = true,
            .returns_single_stream = false,
            .preserves_number_of_streams = true,
            .preserves_sorting = true,
        },
        {
            .preserves_number_of_rows = false,
        }
    };
}

LimitStep::LimitStep(
    const DataStream & input_stream_,
    size_t limit_, size_t offset_,
    bool always_read_till_end_,
    bool with_ties_,
    SortDescription description_,
    bool partial_)
    : ITransformingStep(input_stream_, input_stream_.header, getTraits())
    , limit(limit_), offset(offset_)
    , always_read_till_end(always_read_till_end_)
    , with_ties(with_ties_), description(std::move(description_))
    , partial(partial_)
{
}

void LimitStep::setInputStreams(const DataStreams & input_streams_)
{
    input_streams = input_streams_;
    output_stream->header = input_streams_[0].header;
}

void LimitStep::updateInputStream(DataStream input_stream_)
{
    input_streams.clear();
    input_streams.emplace_back(std::move(input_stream_));
    output_stream = createOutputStream(input_streams.front(), output_stream->header, getDataStreamTraits());
}

void LimitStep::transformPipeline(QueryPipeline & pipeline, const BuildQueryPipelineSettings &)
{
    auto transform = std::make_shared<LimitTransform>(
        pipeline.getHeader(), limit, offset, pipeline.getNumStreams(), always_read_till_end, with_ties, description);

    pipeline.addTransform(std::move(transform));
}

void LimitStep::describeActions(FormatSettings & settings) const
{
    String prefix(settings.offset, ' ');
    settings.out << prefix << "Limit " << limit << '\n';
    settings.out << prefix << "Offset " << offset << '\n';

    if (with_ties || always_read_till_end)
    {
        settings.out << prefix;

        String str;
        if (with_ties)
            settings.out << "WITH TIES";

        if (always_read_till_end)
        {
            if (!with_ties)
                settings.out << ", ";

            settings.out << "Reads all data";
        }

        settings.out << '\n';
    }
}

void LimitStep::describeActions(JSONBuilder::JSONMap & map) const
{
    map.add("Limit", limit);
    map.add("Offset", offset);
    map.add("With Ties", with_ties);
    map.add("Reads All Data", always_read_till_end);
}

void LimitStep::toProto(Protos::LimitStep & proto, bool) const
{
    ITransformingStep::serializeToProtoBase(*proto.mutable_query_plan_base());
    proto.set_limit(limit);
    proto.set_offset(offset);
    proto.set_always_read_till_end(always_read_till_end);
    proto.set_with_ties(with_ties);
    for (const auto & element : description)
        element.toProto(*proto.add_description());
    proto.set_partial(partial);
}

std::shared_ptr<LimitStep> LimitStep::fromProto(const Protos::LimitStep & proto, ContextPtr)
{
    auto [step_description, base_input_stream] = ITransformingStep::deserializeFromProtoBase(proto.query_plan_base());
    auto limit = proto.limit();
    auto offset = proto.offset();
    auto always_read_till_end = proto.always_read_till_end();
    auto with_ties = proto.with_ties();
    SortDescription description;
    for (const auto & proto_element : proto.description())
    {
        SortColumnDescription element;
        element.fillFromProto(proto_element);
        description.emplace_back(std::move(element));
    }
    auto partial = proto.partial();
    auto step = std::make_shared<LimitStep>(base_input_stream, limit, offset, always_read_till_end, with_ties, description, partial);
    step->setStepDescription(step_description);
    return step;
}

std::shared_ptr<IQueryPlanStep> LimitStep::copy(ContextPtr) const
{
    return std::make_shared<LimitStep>(input_streams[0], limit, offset, always_read_till_end, with_ties, description, partial);
}
}
