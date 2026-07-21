#include "base.h"

#include <cstdlib>
#include <cstring>
#include <ostream>
#include <streambuf>

// void HandleError(const std::string& errorMessage) {
//     std::cerr << "Error: " << errorMessage << std::endl;
//     std::exit(EXIT_FAILURE);
// }

// ---------------------------------------------------------------------------
// Gated debug output (see base.h). Default OFF: a plain decompile is quiet and
// cannot flood the disk on a large abc. Enable with env XABC_DEBUG=1.
bool g_xabc_debug = false;

namespace {
// A streambuf that discards everything — the null sink used when debug is off.
class NullBuffer : public std::streambuf {
public:
    int overflow(int c) override { return c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};
NullBuffer g_null_buffer;
std::ostream g_null_stream(&g_null_buffer);
bool g_xabc_debug_inited = false;
}  // namespace

void XabcInitDebugFromEnv() {
    const char* v = std::getenv("XABC_DEBUG");
    g_xabc_debug = (v != nullptr) &&
                   (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
                    std::strcmp(v, "on") == 0 || std::strcmp(v, "TRUE") == 0);
    g_xabc_debug_inited = true;
}

std::ostream& XabcDbgStream() {
    // Lazy env read so callers that never call XabcInitDebugFromEnv() (e.g. unit
    // harnesses) still get the correct default-off behaviour.
    if (!g_xabc_debug_inited) {
        XabcInitDebugFromEnv();
    }
    return g_xabc_debug ? std::cerr : g_null_stream;
}


std::string RemoveArgumentsOfFunc(const std::string& input) {
    std::string result;
    size_t endPos = input.find('(');
    if (endPos != std::string::npos) {
        std::string beforeParen = input.substr(0, endPos);
        size_t colonPos = beforeParen.find_last_not_of(':');
        if (colonPos != std::string::npos) {
            beforeParen = beforeParen.substr(0, colonPos+1);
            return beforeParen;
        }
    }

    return input;
}

bool IsArray(const panda::panda_file::LiteralTag &tag)
{
    switch (tag) {
        case panda::panda_file::LiteralTag::ARRAY_U1:
        case panda::panda_file::LiteralTag::ARRAY_U8:
        case panda::panda_file::LiteralTag::ARRAY_I8:
        case panda::panda_file::LiteralTag::ARRAY_U16:
        case panda::panda_file::LiteralTag::ARRAY_I16:
        case panda::panda_file::LiteralTag::ARRAY_U32:
        case panda::panda_file::LiteralTag::ARRAY_I32:
        case panda::panda_file::LiteralTag::ARRAY_U64:
        case panda::panda_file::LiteralTag::ARRAY_I64:
        case panda::panda_file::LiteralTag::ARRAY_F32:
        case panda::panda_file::LiteralTag::ARRAY_F64:
        case panda::panda_file::LiteralTag::ARRAY_STRING:
            return true;
        default:
            return false;
    }
}

std::optional<std::vector<std::string>> GetLiteralArrayByOffset(panda::pandasm::Program* program, uint32_t offset){
    std::vector<std::string> res;
    std::stringstream hexStream;
    hexStream << "0x" << std::hex << offset;
    std::string offsetString = hexStream.str();
    
    for (const auto &[key, lit_array] :  program->literalarray_table) {
        const auto &tag = lit_array.literals_[0].tag_;
                
        if (key.find(offsetString) == std::string::npos || IsArray(tag)) {
            continue;
        }
        
        for (size_t i = 0; i < lit_array.literals_.size(); i++) {
            const auto &tag = lit_array.literals_[i].tag_;
            const auto &val = lit_array.literals_[i].value_;
            if(tag == panda::panda_file::LiteralTag::METHOD || tag == panda::panda_file::LiteralTag::GETTER || 
                tag == panda::panda_file::LiteralTag::SETTER || tag == panda::panda_file::LiteralTag::GENERATORMETHOD){
                res.push_back(std::get<std::string>(val));
            }
        }
        return res;
    }

    return std::nullopt;
}


std::optional<std::string> FindKeyByValue(const std::map<std::string, uint32_t>& myMap, const uint32_t& value) {
    for (const auto& pair : myMap) {
        if (pair.second == value) {
            return pair.first;
        }
    }
    return std::nullopt;
}

std::optional<panda::pandasm::LiteralArray> FindLiteralArrayByOffset(panda::pandasm::Program *program_, uint32_t offset) {
    for (const auto& [key, value] : program_->literalarray_table) {
        if (ParseHexFromKey(key) == offset) {
            return value;
        }
    }
    return std::nullopt;
}


uint32_t ParseHexFromKey(const std::string& key) {
    std::istringstream iss(key);
    std::string temp;
    std::string hexString;

    while (iss >> temp) {
        if (temp.find("0x") == 0 || temp.find("0X") == 0) {
            hexString = temp;
            break;
        }
    }

    uint32_t hexNumber = 0;
    if (!hexString.empty()) {
        std::istringstream(hexString) >> std::hex >> hexNumber;
    }

    return hexNumber;
}

