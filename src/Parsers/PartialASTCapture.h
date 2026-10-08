#pragma once

/// Capture of the statement tree built so far when a statement parser fails, for the standalone
/// WebAssembly parser (`utils/wasm-parser`), which reports it as `partial_ast`. Only that build
/// defines `CLICKHOUSE_PARSER_PARTIAL_AST`; everywhere else the hook is a no-op that evaluates
/// none of its arguments, so the parser and its `Expected` stay exactly as they are.
///
/// A call goes right before an existing `return false` of a statement parser, after the statement
/// keyword has committed, or where the parser recovers from a failed optional part and goes on
/// (such a capture is reported only if the parse gets no further). It never assigns to `node` and
/// never moves `pos`. For `SELECT` the last argument names the clause; for `INSERT`, `CREATE` and
/// `ALTER` it is the JSON key of the slot that failed.

#if defined(CLICKHOUSE_PARSER_PARTIAL_AST)

#include <Parsers/IAST_fwd.h>
#include <Parsers/IParser.h>

namespace DB
{

/// Clones `node` into `expected.partial_ast`, with an `Error` node in the slot of the clause named
/// `expected_what`. Defined in `utils/wasm-parser`.
void snapshotPartialAST(Expected & expected, const ASTPtr & node, IParser::Pos pos, const char * expected_what);

}

#define PARTIAL_AST_SNAPSHOT(expected, node, pos, what) snapshotPartialAST(expected, node, pos, what)

#else

#define PARTIAL_AST_SNAPSHOT(expected, node, pos, what) ((void)0)

#endif
