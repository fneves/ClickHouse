#pragma once

#include <Parsers/IAST.h>

#include <string>
#include <vector>


namespace DB
{

/** The slot of a `partial_ast` (see `Parsers/PartialASTCapture.h`) that the parser failed in.
  *
  * Serialize-only: it has no SQL formatting, and `IAST::createFromJSON` does not know its type, so
  * `ch_format_json` rejects a document that contains one.
  */
class ASTError : public IAST
{
public:
    /// Where the parser stopped: `max_parsed_pos`, and the end of the token that starts there.
    /// `ch_parse` turns them into byte offsets with `setOffsets` once the query start is known.
    const char * begin_pos = nullptr;
    const char * end_pos = nullptr;

    UInt64 begin = 0;
    UInt64 end = 0;

    /// What the parser would have accepted at `begin_pos`.
    std::vector<std::string> expected;

    String getID(char) const override { return "Error"; }
    ASTPtr clone() const override;
    void writeJSON(WriteBuffer & out) const override;

    void setOffsets(const char * query_begin);
};

}
