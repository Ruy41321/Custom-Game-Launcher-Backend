#include "domain/CanonicalJson.h"

namespace launcher::domain {

void appendCanonicalJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                static constexpr char HEX[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(HEX[(static_cast<unsigned char>(character) >> 4) & 0x0F]);
                out.push_back(HEX[static_cast<unsigned char>(character) & 0x0F]);
            } else {
                out.push_back(character);
            }
            break;
        }
    }
    out.push_back('"');
}

} // namespace launcher::domain
