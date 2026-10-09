#include <Parsers/PartialASTCapture.h>

#include <ASTError.h>

#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDictionary.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTRefreshStrategy.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTViewTargets.h>

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
/// cannot see. `child`, if any, is measured as one more child of `root`.
bool fitsPartialASTLimits(const IAST & root, const IAST * child)
{
    std::vector<std::pair<const IAST *, size_t>> nodes{{&root, 0}};
    if (child)
        nodes.emplace_back(child, 1);
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

/// A tree that could never be an `ast` is not reported. Nothing captured before it is: that was a
/// shallower failure, and the parse got further. Nor anything around it at this position.
void blockCaptures(Expected & expected)
{
    expected.partial_ast = nullptr;
    expected.partial_ast_pos = expected.max_parsed_pos;
}

/// The checks every capture makes before it clones anything; false if it should not be taken.
/// `extra` is a tree that the capture adds to `node` as one more child.
bool startCapture(Expected & expected, const ASTPtr & node, bool is_select, const IAST * extra = nullptr)
{
    if (!expected.enable_partial_ast_capture || !node)
        return false;

    /// A capture that did not get as far as the one already taken is a backtracked alternative.
    if (expected.partial_ast_pos && expected.max_parsed_pos < expected.partial_ast_pos)
        return false;

    /// At the same position, the statement that captured first is the innermost one, and it is kept:
    /// for `SELECT * FROM (SELECT a FROM`, the subquery, not the query around it. The one exception
    /// is a `SELECT` over a capture of another statement at that position - the column list of an
    /// `INSERT`, which then retries the `(` as a subquery. A position with `partial_ast_pos` and no
    /// `partial_ast` is one whose innermost statement was too big, and nothing around it is taken.
    if (expected.partial_ast_pos && expected.max_parsed_pos == expected.partial_ast_pos
        && (!expected.partial_ast || !is_select || expected.partial_ast->as<ASTSelectQuery>()))
        return false;

    if (!fitsPartialASTLimits(*node, extra))
    {
        blockCaptures(expected);
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

/// The node a fragment is a part of: the node of an `ASTPartialStatement`, or the list itself.
const IAST & fragmentNode(const IAST & fragment)
{
    if (const auto * wrapper = fragment.as<ASTPartialStatement>())
        return *wrapper->statement;
    return fragment;
}

/// Whether a fragment fills the slot `key` of `parent`, or, for a null key, is an element of the list
/// `parent`.
bool fillsSlot(const IAST & parent, const char * key, const IAST & fragment)
{
    const IAST & node = fragmentNode(fragment);
    const std::string_view slot = key ? key : "";
    if (parent.as<ASTCreateQuery>())
        return (slot == "refresh_strategy" && node.as<ASTRefreshStrategy>()) || (slot == "dictionary" && node.as<ASTDictionary>());
    return false;
}

/// Empties the slot `key` of a part of a statement, which a failure after the slot parsed - a missing
/// `)` - leaves set: the `Error` takes the place of what parsed there, as it does of a broken
/// expression, and the key is written once.
void resetSlot(IAST & node, std::string_view key)
{
    if (auto * dictionary = node.as<ASTDictionary>())
    {
        if (key == "primary_key")
            dictionary->reset(dictionary->primary_key);
        else if (key == "source")
            dictionary->reset(dictionary->source);
        else if (key == "lifetime")
            dictionary->reset(dictionary->lifetime);
        else if (key == "layout")
            dictionary->reset(dictionary->layout);
        else if (key == "range")
            dictionary->reset(dictionary->range);
        else if (key == "dict_settings")
            dictionary->reset(dictionary->dict_settings);
    }
}

/// What goes into the slot that failed: the fragment captured inside it at this position, if any, and
/// otherwise a new `Error`. A fragment is taken once.
ASTPtr takeFragment(Expected & expected, const IAST & parent, const char * key)
{
    if (expected.partial_ast_fragment && expected.partial_ast_fragment_pos == expected.max_parsed_pos
        && fillsSlot(parent, key, *expected.partial_ast_fragment))
        return std::exchange(expected.partial_ast_fragment, nullptr);
    return nullptr;
}

ASTPtr wrap(ASTPtr node, ASTPtr inner, const char * key)
{
    auto wrapper = make_intrusive<ASTPartialStatement>();
    wrapper->statement = std::move(node);
    wrapper->error = std::move(inner);
    wrapper->key = key;
    wrapper->children = {wrapper->statement, wrapper->error};
    return wrapper;
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
    /// A `SELECT` of the union of an `INSERT` - not nested in another `SELECT` of it - gets the `WITH`
    /// written before the `INSERT`, where `ast` has it, unless it has a `WITH` of its own.
    const IAST * outer_with = nullptr;
    if (node && expected.partial_ast_outer_with && expected.partial_ast_selects == expected.partial_ast_outer_with_selects + 1
        && !node->as<ASTSelectQuery &>().with())
        outer_with = expected.partial_ast_outer_with.get();

    if (!startCapture(expected, node, /*is_select=*/ true, outer_with))
        return;

    auto error = makeError(expected, pos);
    ASTPtr partial = node->clone();
    auto & select = partial->as<ASTSelectQuery &>();
    select.setExpression(slot, ASTPtr(error));
    if (outer_with)
    {
        select.setExpression(ASTSelectQuery::Expression::WITH, outer_with->clone());
        select.normalizeChildrenOrder();
    }
    storeCapture(expected, std::move(partial));
}

void snapshotPartialAST(Expected & expected, const ASTPtr & node, IParser::Pos pos, const char * expected_what)
{
    if (!node)
        return;

    ASTPtr fragment = takeFragment(expected, *node, expected_what);
    if (!startCapture(expected, node, /*is_select=*/ false, fragment.get()))
        return;

    ASTPtr inner = fragment ? fragment : makeError(expected, pos);
    ASTPtr partial = node->clone();
    const std::string_view what = expected_what;
    auto * insert = partial->as<ASTInsertQuery>();
    auto * create = partial->as<ASTCreateQuery>();
    if (insert && what == "columns" && insert->columns && insert->columns->as<ASTExpressionList>())
    {
        /// The list parsed and its `)` is missing: what failed is the element after the last one.
        insert->columns->children.push_back(inner);
        storeCapture(expected, std::move(partial));
        return;
    }

    if (create && what == "aliases_list" && create->aliases_list)
    {
        create->aliases_list->children.push_back(inner);
        storeCapture(expected, std::move(partial));
        return;
    }

    if (create && what == "dictionary_attributes_list" && create->dictionary_attributes_list)
    {
        create->dictionary_attributes_list->children.push_back(inner);
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
            create->columns_list->columns->children.push_back(inner);
            storeCapture(expected, std::move(partial));
            return;
        }
        create->reset(create->columns_list);
    }

    if (create && what == "inner_engine")
    {
        /// The storage of a materialized view, which `ast` has as the inner engine of its `TO` target.
        if (!create->targets)
            create->set(create->targets, make_intrusive<ASTViewTargets>());
        create->targets->setInnerEngine(ViewTarget::To, inner);
        storeCapture(expected, std::move(partial));
        return;
    }

    storeCapture(expected, wrap(std::move(partial), std::move(inner), expected_what));
}

void snapshotPartialFragment(Expected & expected, const ASTPtr & node, IParser::Pos pos, const char * expected_what)
{
    if (!expected.enable_partial_ast_capture || !node)
        return;

    /// Nothing around a statement captured at this position or further is taken, so no part of it is.
    if (expected.partial_ast_pos && expected.max_parsed_pos <= expected.partial_ast_pos)
        return;

    ASTPtr inner = takeFragment(expected, *node, expected_what);

    /// An empty list with nothing in it that captured is not a part: the slot of the list is what failed.
    if (!expected_what && !inner && node->children.empty())
        return;

    if (!fitsPartialASTLimits(*node, inner.get()))
    {
        blockCaptures(expected);
        return;
    }

    if (!inner)
        inner = makeError(expected, pos);
    ASTPtr partial = node->clone();
    auto * dictionary = partial->as<ASTDictionary>();
    if (!expected_what)
    {
        partial->children.push_back(std::move(inner));
    }
    else if (dictionary && std::string_view(expected_what) == "primary_key" && dictionary->primary_key)
    {
        /// `PRIMARY KEY (a, b` lacks its `)`: what failed is the element after the last one.
        dictionary->primary_key->children.push_back(std::move(inner));
    }
    else
    {
        resetSlot(*partial, expected_what);
        partial = wrap(std::move(partial), std::move(inner), expected_what);
    }

    expected.partial_ast_fragment = std::move(partial);
    expected.partial_ast_fragment_pos = expected.max_parsed_pos;
}

}
