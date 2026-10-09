#include <Parsers/PartialASTCapture.h>

#include <ASTError.h>

#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTSelectQuery.h>

#include <string_view>
#include <utility>
#include <vector>


namespace DB
{

namespace
{

/// Whether `root` is within the limits `ch_parse` holds an `ast` to, counted the way `IAST::checkDepth`
/// and `IAST::checkSize` count. Iterative, and it stops at the first node past a limit, so measuring
/// a tree of any shape is cheap and cannot throw - unlike cloning it, which is recursive and, for the
/// left-deep tree of a long operator chain, runs out of the engine's stack, which `checkStackSize`
/// cannot see.
bool fitsPartialASTLimits(const IAST & root)
{
    std::vector<std::pair<const IAST *, size_t>> nodes{{&root, 0}};
    size_t size = 0;
    while (!nodes.empty())
    {
        auto [node, depth] = nodes.back();
        nodes.pop_back();
        if (depth >= PARTIAL_AST_MAX_DEPTH || ++size > PARTIAL_AST_MAX_ELEMENTS)
            return false;
        for (const auto & child : node->children)
            nodes.emplace_back(child.get(), depth + 1);
    }
    return true;
}

/// The checks every capture makes before it clones anything; false if it should not be taken.
bool startCapture(Expected & expected, const ASTPtr & node, bool is_select)
{
    if (!expected.enable_partial_ast_capture || !node)
        return false;

    /// A capture that did not get as far as the one already taken is a backtracked alternative.
    if (expected.partial_ast_pos && expected.max_parsed_pos < expected.partial_ast_pos)
        return false;

    /// At the same position, the statement that captured first is the innermost one, and it is kept:
    /// for `SELECT * FROM (SELECT a FROM`, the subquery, not the query around it. The one exception
    /// is a `SELECT` over a capture of another statement at that position - the column list of an
    /// `INSERT`, which then retries the `(` as a subquery.
    if (expected.partial_ast && expected.max_parsed_pos == expected.partial_ast_pos
        && (!is_select || expected.partial_ast->as<ASTSelectQuery>()))
        return false;

    /// A tree that could never be an `ast` is not reported either. Nothing captured before it is: that
    /// was a shallower failure, and the parse got further.
    if (!fitsPartialASTLimits(*node))
    {
        expected.partial_ast = nullptr;
        expected.partial_ast_pos = expected.max_parsed_pos;
        return false;
    }

    return true;
}

boost::intrusive_ptr<ASTError> makeError(const Expected & expected, IParser::Pos pos)
{
    auto error = make_intrusive<ASTError>();
    error->begin_pos = expected.max_parsed_pos;

    /// The token the parser stopped at, for its end. `pos` is a copy, and only tokens the parser has
    /// already looked at are visited, so `Tokens::max`, which the error message is built from, stays.
    const char * last_seen = pos.max().begin;
    while (pos->begin < error->begin_pos && pos->begin < last_seen)
        ++pos;
    error->end_pos = pos->begin == error->begin_pos ? pos->end : error->begin_pos;

    /// What is expected so far. The parse can add to it at this position later - `end of query`, for
    /// one - so `ch_parse` takes the final list instead when the error is still here.
    for (const char * variant : expected.variants)
        error->expected.emplace_back(variant);

    return error;
}

void storeCapture(Expected & expected, ASTPtr partial)
{
    expected.partial_ast = std::move(partial);
    expected.partial_ast_pos = expected.max_parsed_pos;
}

/// Whether the list of a `CREATE TABLE` holds columns only, so that what failed after it is the
/// element after the last column. With indices, constraints, projections or a primary key in it, the
/// order of the elements is not in the tree.
bool hasOnlyColumns(const ASTColumns & list)
{
    return list.columns && !list.indices && !list.constraints && !list.projections && !list.primary_key
        && !list.primary_key_from_columns;
}

}

void snapshotPartialAST(Expected & expected, const ASTPtr & node, IParser::Pos pos, ASTSelectQuery::Expression slot)
{
    if (!startCapture(expected, node, /*is_select=*/ true))
        return;

    auto error = makeError(expected, pos);
    ASTPtr partial = node->clone();
    partial->as<ASTSelectQuery &>().setExpression(slot, ASTPtr(error));
    storeCapture(expected, std::move(partial));
}

void snapshotPartialAST(Expected & expected, const ASTPtr & node, IParser::Pos pos, const char * expected_what)
{
    if (!startCapture(expected, node, /*is_select=*/ false))
        return;

    auto error = makeError(expected, pos);
    ASTPtr partial = node->clone();
    const std::string_view what = expected_what;
    auto * insert = partial->as<ASTInsertQuery>();
    auto * create = partial->as<ASTCreateQuery>();
    if (insert && what == "columns" && insert->columns && insert->columns->as<ASTExpressionList>())
    {
        /// The list parsed and its `)` is missing: what failed is the element after the last one.
        insert->columns->children.push_back(error);
        storeCapture(expected, std::move(partial));
        return;
    }

    if (create && what == "columns_list" && create->columns_list)
    {
        /// Likewise for `CREATE TABLE t (a UInt8,`. Otherwise the whole list is what failed, and the
        /// `Error` takes its place: an element that parsed after the last column cannot be told apart
        /// from one before it.
        if (hasOnlyColumns(*create->columns_list))
        {
            create->columns_list->columns->children.push_back(error);
            storeCapture(expected, std::move(partial));
            return;
        }
        create->reset(create->columns_list);
    }

    auto wrapper = make_intrusive<ASTPartialStatement>();
    wrapper->statement = std::move(partial);
    wrapper->error = error;
    wrapper->key = expected_what;
    wrapper->children = {wrapper->statement, wrapper->error};
    storeCapture(expected, std::move(wrapper));
}

}
