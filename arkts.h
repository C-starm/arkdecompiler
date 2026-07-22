#ifndef DECOMPILER_ARKTS
#define DECOMPILER_ARKTS

#include <unordered_set>
#include "base.h"

namespace panda::es2panda::ir {

class ArkTSGen {
public:
    class Nullable {
    public:
        explicit Nullable(const ir::AstNode *node) : node_(node) {}

        const ir::AstNode *Node() const
        {
            return node_;
        }

    private:
        const ir::AstNode *node_;
    };

    class Optional {
    public:
        using Val = std::variant<const char *, const ir::AstNode *, bool>;
        explicit Optional(const ir::AstNode *node) : value_(node) {}
        explicit Optional(const char *string) : value_(const_cast<char *>(string)) {}
        explicit Optional(bool boolean) : value_(boolean) {}

        const Val &Value() const
        {
            return value_;
        }

    private:
        Val value_;
    };

    class Property {
    public:
        class Ignore {
        public:
            Ignore() = default;
        };

        enum class Constant {
            PROP_NULL,
            EMPTY_ARRAY,
            PROP_UNDEFINED
        };

        using Val =
            std::variant<const char *, lexer::TokenType, std::initializer_list<Property>, util::StringView, bool,
                         double, const ir::AstNode *, std::vector<const ir::AstNode *>, Constant, Nullable, Ignore>;

        Property(const char *string) : value_(string) {}
        Property(util::StringView str) : value_(str) {}
        Property(bool boolean) : value_(boolean) {}
        Property(double number) : value_(number) {}
        Property(lexer::TokenType token) : value_(token) {}
        Property(std::initializer_list<Property> props) : value_(props) {}
        Property(const ir::AstNode *node) : value_(const_cast<ir::AstNode *>(node)) {}

        Property(Constant constant) : value_(constant) {}
        Property(Nullable nullable) 
        {
            if (nullable.Node()) {
                value_ = nullable.Node();
            } else {
                value_ = Property::Constant::PROP_NULL;
            }
        }

        Property(Optional optional)
        {
            const auto &value = optional.Value();
            if (std::holds_alternative<const ir::AstNode *>(value) && std::get<const ir::AstNode *>(value)) {
                value_ = std::get<const ir::AstNode *>(value);
                return;
            }

            if (std::holds_alternative<const char *>(value) && std::get<const char *>(value)) {
                value_ = std::get<const char *>(value);
                return;
            }

            if (std::holds_alternative<bool>(value) && std::get<bool>(value)) {
                value_ = std::get<bool>(value);
                return;
            }

            value_ = Ignore();
        }

        template <typename T>
        Property(const ArenaVector<T> &array)
        {
            if (array.empty()) {
                value_ = Constant::EMPTY_ARRAY;
                return;
            }

            std::vector<const ir::AstNode *> nodes;
            nodes.reserve(array.size());

            for (auto &it : array) {
                nodes.push_back(it);
            }

            value_ = std::move(nodes);
        }

        const char *Key() const
        {
            return key_;
        }

        const Val &Value() const
        {
            return value_;
        }

    private:
        const char *key_;
        Val value_ {false};
    };

    explicit ArkTSGen(const BlockStatement *program, util::StringView sourceCode);
    explicit ArkTSGen(const ir::AstNode *node);

    void EmitStatement(const ir::AstNode *node);

    void Add(std::initializer_list<Property> props);
    void Add(const ArkTSGen::Property &prop);

    static const char *ModifierToString(ModifierFlags flags);
    static const char *TypeOperatorToString(TSOperatorType operatorType);

    std::string Str() const
    {
        return ss_.str();
    }

private:
    using WrapperCb = std::function<void()>;

    template <typename T>
    void AddList(T props)
    {
        for (auto it = props.begin(); it != props.end();) {
            Serialize(*it);

            do {
                if (++it == props.end()) {
                    return;
                }
            } while (std::holds_alternative<Property::Ignore>((*it).Value()));

            ss_ << ',';
        }
    }

    void Indent();

    void EmitBlockStatement(const ir::AstNode *node);

    void WriteTrailingSemicolon(); //;
    void WriteSpace();
    void WriteLeftBrace();    // {
    void WriteRightBrace();   // }
    void WriteLeftBracket();  // [
    void WriteRightBracket(); // ]

    void WriteLeftParentheses(); // (
    void WriteRightParentheses(); // )
    

    void WriteColon();// :
    void WriteEqual();// =
    void WriteComma();// ,
    void WriteDot(); // .
    void WriteSpreadDot(); // ...
    void WriteKeyWords(std::string keyword); 
    void WriteNewLine();
    void WriteIndent();

    void EmitExpression(const ir::AstNode *node);
    void EmitExpressionStatement(const ir::AstNode *node);
    void EmitVariableDeclarationStatement(const ir::AstNode *node);
    void EmitVariableDeclaratorStatement(const ir::AstNode *node);
    void EmitReturnStatement(const ir::AstNode *node);
    void EmitBreakStatement(const ir::AstNode *node);

    void EmitDebuggerStatement(const ir::AstNode *node);
    void EmitIfStatement(const ir::AstNode *node);
    void EmitTryStatement(const ir::AstNode *node);
    void EmitThrowStatement(const ir::AstNode *node);
    void EmitWhileStatement(const ir::AstNode *node);
    void EmitDoWhileStatement(const ir::AstNode *node);

    void EmitImportSpecifier(const ir::AstNode *node);
    void EmitImportDeclaration(const ir::AstNode *node);

    void EmitExportAllDeclaration(const ir::AstNode *node);
    void EmitExportSpecifier(const ir::AstNode *node);
    void EmitExportNamedDeclaration(const ir::AstNode *node);

    void EmitClassDeclaration(const ir::AstNode *node);
    void EmitMethodDefinition(const ir::AstNode *node);

    void EmitFunctionDeclaration(const ir::AstNode *node);

    void Serialize(const ArkTSGen::Property &prop);
    void SerializePropKey(const char *str);
    void SerializeString(const char *str);
    void SerializeString(const util::StringView &str);
    void SerializeNumber(size_t number);
    void SerializeNumber(double number);
    void SerializeBoolean(bool boolean);
    void SerializeObject(const ir::AstNode *object);
    void SerializeToken(lexer::TokenType token);
    void SerializePropList(std::initializer_list<ArkTSGen::Property> props);
    void SerializeConstant(Property::Constant constant);
    void Wrap(const WrapperCb &cb, char delimStart = '{', char delimEnd = '}');

    void SerializeArray(std::vector<const ir::AstNode *> array);

    lexer::LineIndex index_;
    std::stringstream ss_;
    int32_t indent_;
    int32_t singleindent_ = 2;
    bool dumpNodeOnly_ = true;

    // Emit-recursion guard. A malformed AST can contain a CYCLE (a statement whose
    // body transitively points back to an ancestor); the recursive emit then loops
    // forever and overflows the stack (SIGSEGV) — observed on real HAPs where three
    // records crashed xabc this way.
    //
    // Two layers:
    //  1) emit_path_ — the set of AstNode pointers currently on the recursion stack.
    //     Re-entering a node already on the path IS the cycle; we cut it immediately
    //     (O(path length)), so a pathological wide cycle is severed the first time it
    //     closes instead of being expanded thousands of levels deep (which, even with
    //     a depth cap, blows up to an enormous/slow output).
    //  2) emit_depth_ / kEmitDepthLimit — a plain depth backstop for
    //     legitimately-but-absurdly-deep or indirectly-recursive shapes the path set
    //     might not catch. 2000 is far beyond any real hand-written nesting yet well
    //     under the frame count that overflows an 8 MB stack.
    // On either trip we write a marker comment and unwind, turning a hard crash into a
    // skipped subtree so the rest of the file still decompiles.
    std::unordered_set<const ir::AstNode*> emit_path_;
    int32_t emit_depth_ = 0;
    bool emit_depth_tripped_ = false;
    static constexpr int32_t kEmitDepthLimit = 2000;





};
}  // namespace panda::es2panda::ir

#endif  // ASTDUMP_H
