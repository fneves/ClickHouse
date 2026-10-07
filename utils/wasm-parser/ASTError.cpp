#include <ASTError.h>

#include <Parsers/ASTJSONHelpers.h>


namespace DB
{

ASTPtr ASTError::clone() const
{
    return make_intrusive<ASTError>(*this);
}

void ASTError::writeJSON(WriteBuffer & out) const
{
    JSONObjectWriter w(out, "Error");
    w.writeUInt("begin", begin);
    w.writeUInt("end", end);

    w.writeKey("expected");
    out << '[';
    for (size_t i = 0; i < expected.size(); ++i)
    {
        if (i > 0)
            out << ',';
        writeJSONString(expected[i], out, w.getFormatSettings());
    }
    out << ']';
}

void ASTError::setOffsets(const char * query_begin)
{
    begin = static_cast<UInt64>(begin_pos - query_begin);
    end = static_cast<UInt64>(end_pos - query_begin);
}

}
