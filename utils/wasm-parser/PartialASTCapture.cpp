#include <Parsers/PartialASTCapture.h>

#include <ASTError.h>

#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Common/Exception.h>

#include <string_view>
#include <utility>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

/// The names `ParserSelectQuery` passes, and the slot each of them fills.
ASTSelectQuery::Expression selectClauseSlot(std::string_view clause)
{
    using Expression = ASTSelectQuery::Expression;
    static constexpr std::pair<std::string_view, Expression> slots[] = {
        {"SELECT", Expression::SELECT},
        {"FROM", Expression::TABLES},
        {"PREWHERE", Expression::PREWHERE},
        {"WHERE", Expression::WHERE},
        {"GROUP BY", Expression::GROUP_BY},
        {"HAVING", Expression::HAVING},
        {"ORDER BY", Expression::ORDER_BY},
        {"LIMIT", Expression::LIMIT_LENGTH},
        {"OFFSET", Expression::LIMIT_OFFSET},
        {"LIMIT BY", Expression::LIMIT_BY},
        {"SETTINGS", Expression::SETTINGS},
    };

    for (const auto & [name, slot] : slots)
        if (name == clause)
            return slot;

    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unknown SELECT clause '{}' for a partial AST", clause);
}

}

void snapshotPartialAST(Expected & expected, const ASTPtr & node, IParser::Pos pos, const char * expected_what)
{
    if (!expected.enable_partial_ast_capture || !node)
        return;

    /// A capture that did not get as far as the one already taken is a backtracked alternative.
    if (expected.partial_ast_pos && expected.max_parsed_pos < expected.partial_ast_pos)
        return;

    /// At the same position, the statement that captured first is the innermost one, and it is kept:
    /// for `SELECT * FROM (SELECT a FROM`, the subquery, not the query around it. The one exception
    /// is a `SELECT` over a capture of another statement at that position - the column list of an
    /// `INSERT`, which then retries the `(` as a subquery.
    const bool is_select = node->as<ASTSelectQuery>() != nullptr;
    if (expected.partial_ast && expected.max_parsed_pos == expected.partial_ast_pos
        && (!is_select || expected.partial_ast->as<ASTSelectQuery>()))
        return;

    auto error = make_intrusive<ASTError>();
    error->begin_pos = expected.max_parsed_pos;

    /// The token the parser stopped at, for its end. `pos` is a copy, and only tokens the parser has
    /// already looked at are visited, so `Tokens::max`, which the error message is built from, stays.
    const char * last_seen = pos.max().begin;
    while (pos->begin < error->begin_pos && pos->begin < last_seen)
        ++pos;
    error->end_pos = pos->begin == error->begin_pos ? pos->end : error->begin_pos;

    for (const char * variant : expected.variants)
        error->expected.emplace_back(variant);

    ASTPtr partial = node->clone();
    const std::string_view what = expected_what;
    auto * insert = partial->as<ASTInsertQuery>();
    auto * create = partial->as<ASTCreateQuery>();
    if (is_select)
    {
        partial->as<ASTSelectQuery &>().setExpression(selectClauseSlot(what), ASTPtr(error));
    }
    else if (insert && what == "columns" && insert->columns && insert->columns->as<ASTExpressionList>())
    {
        /// The list parsed and its `)` is missing: what failed is the element after the last one.
        insert->columns->children.push_back(error);
    }
    else if (create && what == "columns_list" && create->columns_list)
    {
        /// Likewise for `CREATE TABLE t (a UInt8,`. A list that has no columns, only indices or
        /// constraints, is left without a capture rather than given a `columns` it never had.
        if (!create->columns_list->columns)
            return;
        create->columns_list->columns->children.push_back(error);
    }
    else
    {
        auto wrapper = make_intrusive<ASTPartialStatement>();
        wrapper->statement = std::move(partial);
        wrapper->error = error;
        wrapper->key = expected_what;
        wrapper->children = {wrapper->statement, wrapper->error};
        partial = std::move(wrapper);
    }

    expected.partial_ast = std::move(partial);
    expected.partial_ast_pos = expected.max_parsed_pos;
}

}
