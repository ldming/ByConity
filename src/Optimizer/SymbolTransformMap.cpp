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

#include <Optimizer/SymbolTransformMap.h>

#include <Optimizer/Utils.h>
#include <Parsers/ASTTableColumnReference.h>
#include <Parsers/formatAST.h>
#include <QueryPlan/PlanVisitor.h>
#include <Interpreters/InDepthNodeVisitor.h>

namespace DB
{
class SymbolTransformMap::Visitor : public PlanNodeVisitor<Void, Void>
{
public:
    explicit Visitor(std::optional<PlanNodeId> stop_node_) : stop_node(std::move(stop_node_))
    {
    }

    Void visitAggregatingNode(AggregatingNode & node, Void & context) override
    {
        const auto * agg_step = dynamic_cast<const AggregatingStep *>(node.getStep().get());
        for (const auto & aggregate_description : agg_step->getAggregates())
        {
            auto function = Utils::extractAggregateToFunction(aggregate_description);
            addSymbolExpressionMapping(aggregate_description.column_name, function);
        }
        return visitChildren(node, context);
    }

    Void visitFilterNode(FilterNode & node, Void & context) override { return visitChildren(node, context); }

    Void visitProjectionNode(ProjectionNode & node, Void & context) override
    {
        const auto * project_step = dynamic_cast<const ProjectionStep *>(node.getStep().get());
        for (const auto & assignment : project_step->getAssignments())
        {
            if (Utils::isIdentity(assignment))
                continue;
            addSymbolExpressionMapping(assignment.first, assignment.second);
            // if (const auto * function = dynamic_cast<const ASTFunction *>(assignment.second.get()))
            // {
            //     if (function->name == "cast" && TypeCoercion::compatible)
            //     {
            //         symbol_to_cast_lossless_expressions.emplace(assignment.first, function->children[0]);
            //     }
            // }
        }
        return visitChildren(node, context);
    }

    Void visitSortingNode(SortingNode & node, Void & context) override
    {
        return visitChildren(node, context);
    }

    Void visitJoinNode(JoinNode & node, Void & context) override { return visitChildren(node, context); }
    Void visitExchangeNode(ExchangeNode & node, Void & context) override { return visitChildren(node, context); }

    Void visitTableScanNode(TableScanNode & node, Void &) override
    {
        auto table_step = dynamic_cast<const TableScanStep *>(node.getStep().get());
        for (const auto & item : table_step->getColumnAlias())
        {
            auto column_reference = std::make_shared<ASTTableColumnReference>(table_step->getStorage().get(), node.getId(), item.first);
            addSymbolExpressionMapping(item.second, column_reference);
        }

        for (const auto & item : table_step->getInlineExpressions())
        {
            auto inline_expr
                = IdentifierToColumnReference::rewrite(table_step->getStorage().get(), node.getId(), item.second->clone(), false);
            addSymbolExpressionMapping(item.first, inline_expr);
        }
        return Void{};
    }

    Void visitPlanNode(PlanNodeBase & node, Void & context) override
    {
        visitChildren(node, context);
        valid = false; // unsupported node
        return Void{};
    }

    Void visitChildren(PlanNodeBase & node, Void & context)
    {
        if (stop_node.has_value() && node.getId() == *stop_node)
            return {};
        for (auto & child : node.getChildren())
            VisitorUtil::accept(*child, *this, context);
        return Void{};
    }

public:
    std::optional<PlanNodeId> stop_node; // visit this node, but not visit its descendant
    std::unordered_map<String, ConstASTPtr> symbol_to_expressions;
    std::unordered_map<String, ConstASTPtr> symbol_to_cast_lossless_expressions;
    bool valid = true;

    void addSymbolExpressionMapping(const String & symbol, ConstASTPtr expr)
    {
        // violation may happen when matching the root node, which may contain duplicate
        // symbol names with other plan nodes. e.g. select sum(amount) as amount
        if (!symbol_to_expressions.emplace(symbol, std::move(expr)).second)
            valid = false;
    }
};

class SymbolTransformMap::Rewriter : public SimpleExpressionRewriter<Void>
{
public:
    Rewriter(
        const std::unordered_map<String, ConstASTPtr> & symbol_to_expressions_,
        std::unordered_map<String, ConstASTPtr> & expression_lineage_)
        : symbol_to_expressions(symbol_to_expressions_)
        , expression_lineage(expression_lineage_)
    {
    }

    ASTPtr visitASTIdentifier(ASTPtr & expr, Void & context) override
    {
        const auto & name = expr->as<ASTIdentifier &>().name();

        if (auto iter = expression_lineage.find(name); iter != expression_lineage.end())
            return iter->second->clone();

        if (auto iter = symbol_to_expressions.find(name); iter != symbol_to_expressions.end())
        {
            ASTPtr rewrite = ASTVisitorUtil::accept(iter->second->clone(), *this, context);
            expression_lineage[name] = rewrite;
            return rewrite->clone();
        }

        return expr;
    }

private:
    const std::unordered_map<String, ConstASTPtr> & symbol_to_expressions;
    std::unordered_map<String, ConstASTPtr> & expression_lineage;
};

std::optional<SymbolTransformMap> SymbolTransformMap::buildFrom(PlanNodeBase & plan, std::optional<PlanNodeId> stop_node)
{
    Visitor visitor(stop_node);
    Void context;
    VisitorUtil::accept(plan, visitor, context);
    std::optional<SymbolTransformMap> ret;
    if (visitor.valid)
        ret = SymbolTransformMap{visitor.symbol_to_expressions, visitor.symbol_to_cast_lossless_expressions};
    return ret;
}

ASTPtr SymbolTransformMap::inlineReferences(const ConstASTPtr & expression) const
{
    Rewriter rewriter{symbol_to_expressions, expression_lineage};
    Void context;
    return ASTVisitorUtil::accept(expression->clone(), rewriter, context);
}

String SymbolTransformMap::toString() const
{
    String str;
    str += "expression_lineage: ";
    for (const auto & x: expression_lineage)
        str += x.first + " = " + serializeAST(*x.second) + ", ";
    str += "symbol_to_expressions: ";
    for (const auto & x: symbol_to_expressions)
        str += x.first + " = " + serializeAST(*x.second) + ", ";
    return str;
}

void SymbolTranslationMap::addStorageTranslation(ASTPtr ast, String name, const IStorage * storage, UInt32 unique_id)
{
    ast = IdentifierToColumnReference::rewrite(storage, unique_id, ast, true);
    translation.emplace(std::move(ast), std::move(name));
}

std::optional<String> SymbolTranslationMap::tryGetTranslation(const ASTPtr & expr) const
{
    std::optional<String> result = std::nullopt;

    if (auto iter = translation.find(expr); iter != translation.end())
        result = iter->second;

    return result;
}

ASTPtr SymbolTranslationMap::translateImpl(ASTPtr ast) const
{
    // expression which can be translated
    if (auto column = tryGetTranslation(ast))
        return std::make_shared<ASTIdentifier>(*column);

    // ASTFunction
    if (const auto * func = ast->as<ASTFunction>())
    {
        ASTs translated_arguments;

        if (func->arguments)
            for (const auto & arg : func->arguments->children)
                translated_arguments.push_back(translateImpl(arg));

        return makeASTFunction(func->name, translated_arguments);
    }

    // other ast type
    return ast;
}

ASTPtr IdentifierToColumnReference::rewrite(const IStorage * storage, UInt32 unique_id, ASTPtr ast, bool clone)
{
    if (clone)
        ast = ast->clone();
    IdentifierToColumnReference rewriter{storage, unique_id};
    Void context;
    return ASTVisitorUtil::accept(ast, rewriter, context);
}

IdentifierToColumnReference::IdentifierToColumnReference(const IStorage * storage_, UInt32 unique_id_)
    : storage(storage_), unique_id(unique_id_)
{
    if (storage == nullptr)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "storage ptr is NULL");
    storage_metadata = storage->getInMemoryMetadataPtr();
}

ASTPtr IdentifierToColumnReference::visitASTIdentifier(ASTPtr & node, Void &)
{
    const auto & iden = node->as<ASTIdentifier &>();
    const auto & columns = storage_metadata->getColumns();
    if (columns.hasColumnOrSubcolumn(GetColumnsOptions::AllPhysical, iden.name()))
        return std::make_shared<ASTTableColumnReference>(storage, unique_id, iden.name());
    return node;
}

ASTPtr ColumnReferenceToIdentifier::rewrite(ASTPtr ast, bool clone)
{
    if (clone)
        ast = ast->clone();
    ColumnReferenceToIdentifier rewriter;
    Void context;
    return ASTVisitorUtil::accept(ast, rewriter, context);
}

ASTPtr ColumnReferenceToIdentifier::visitASTTableColumnReference(ASTPtr & node, Void &)
{
    const auto & column_ref = node->as<ASTTableColumnReference &>();
    return std::make_shared<ASTIdentifier>(column_ref.column_name);
}
}
