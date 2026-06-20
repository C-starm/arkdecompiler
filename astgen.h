#ifndef DECOMPILER_ASTGEN
#define DECOMPILER_ASTGEN

#include "base.h"
#include "lexicalenv.h"
#include "algos.h"
#include "loopconstruction.h"

namespace panda::bytecodeopt {

using compiler::BasicBlock;
using compiler::Inst;
using compiler::Opcode;


class AstGen : public compiler::Optimization, public compiler::GraphVisitor {
public:
    explicit AstGen(compiler::Graph *graph, pandasm::Function *function, 
        const BytecodeOptIrInterface *iface, pandasm::Program *prog,  es2panda::parser::Program* parser_program, 
        uint32_t methodoffset, std::map<uint32_t, LexicalEnvStack*>* method2lexicalenvstack,
        std::map<uint32_t, LexicalEnvStack*>* method2sendablelexicalenvstack, 
        std::map<uint32_t, std::string*> *patchvarspace,
        std::map<size_t, std::vector<std::string>> index2namespaces, std::vector<std::string> localnamespaces,
        std::vector<std::string> importnamespaces,
        std::map<std::string, std::vector<std::string>>* recordimportnamespaces,
        std::map<uint32_t, std::set<uint32_t>> *class2memberfuns,
        std::map<uint32_t, panda::es2panda::ir::ScriptFunction *> *method2scriptfunast, 
        std::map<uint32_t, panda::es2panda::ir::ClassDeclaration *>* ctor2classdeclast, std::set<uint32_t> *memberfuncs, 
        std::map<uint32_t, panda::es2panda::ir::Expression*> *class2father, 
        std::map<uint32_t, std::map<uint32_t,  std::set<size_t>>>* method2lexicalmap,
        std::vector<LexicalEnvStack*> *globallexical_waitlist,
        std::vector<LexicalEnvStack*> *globalsendablelexical_waitlist,
        std::map<std::string, std::string> *raw2newname,
        std::map<std::string, uint32_t> *methodname2offset,
        std::string fun_name)
        : compiler::Optimization(graph), function_(function), ir_interface_(iface), program_(prog), methodoffset_(methodoffset),
        method2lexicalenvstack_(method2lexicalenvstack), method2sendablelexicalenvstack_(method2sendablelexicalenvstack), 
        patchvarspace_(patchvarspace), parser_program_(parser_program), 
        index2namespaces_(index2namespaces), localnamespaces_(localnamespaces), importnamespaces_(importnamespaces), recordimportnamespaces_(recordimportnamespaces), class2memberfuns_(class2memberfuns),
        method2scriptfunast_(method2scriptfunast), ctor2classdeclast_(ctor2classdeclast), memberfuncs_(memberfuncs), class2father_(class2father),
        method2lexicalmap_(method2lexicalmap), globallexical_waitlist_(globallexical_waitlist), globalsendablelexical_waitlist_(globalsendablelexical_waitlist), 
        raw2newname_(raw2newname), methodname2offset_(methodname2offset), fun_name_(fun_name)
    {

        this->closure_count = 0;
        this->privatevar_count = 0;

        ArenaVector<es2panda::ir::Expression*> arguments(parser_program->Allocator()->Adapter());

        if(this->method2lexicalenvstack_->find(methodoffset) != this->method2lexicalenvstack_->end()){
            //std::cout << "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX found lexicalenvstack " << std::endl;
            //auto x = (*this->method2lexicalenvstack_)[methodoffset];
            //std::cout << "lexicalenvstack size: " << x->Size() << std::endl;
        }else{
            //std::cout << "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX not found lexicalenvstack " << std::endl;
            (*this->method2lexicalenvstack_)[methodoffset] = new LexicalEnvStack();
        }

        if(this->method2sendablelexicalenvstack_->find(methodoffset) != this->method2sendablelexicalenvstack_->end()){
            std::cout << "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX found lexicalenvstack " << std::endl;
            auto x = (*this->method2sendablelexicalenvstack_)[methodoffset];
            std::cout << "sendablelexicalenvstack size: " << x->Size() << std::endl;
        }else{
            //std::cout << "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX not found lexicalenvstack " << std::endl;
            (*this->method2sendablelexicalenvstack_)[methodoffset] = new LexicalEnvStack();
        }


        this->bb2lexicalenvstack_[graph->GetStartBlock()] = (*this->method2lexicalenvstack_)[methodoffset];
        this->bb2sendablelexicalenvstack_[graph->GetStartBlock()] = (*this->method2sendablelexicalenvstack_)[methodoffset];
        
        for (size_t i = 0; i < function->GetParamsNum(); ++i) {
            if(i <= 2){
                continue;
            }
            //std::string* argname = ;
            //panda::es2panda::util::StringView tmp_name_view = panda::es2panda::util::StringView(*argname);
            arguments.push_back(this->getParameterName(i));
        }

        ArenaVector<panda::es2panda::ir::Statement *> func_statements(parser_program->Allocator()->Adapter());
        auto body = parser_program->Allocator()->New<panda::es2panda::ir::BlockStatement>(nullptr, std::move(func_statements));
        panda::es2panda::ir::ScriptFunctionFlags flags_ {panda::es2panda::ir::ScriptFunctionFlags::NONE};
        auto funcNode = parser_program->Allocator()->New<panda::es2panda::ir::ScriptFunction>(nullptr, std::move(arguments), nullptr, body, nullptr, flags_, true, false);
        
        this->scriptfunc = funcNode;
        
        (*this->method2scriptfunast_)[methodoffset] = funcNode;

        auto newfunname = this->RemovePrefixOfFunc(fun_name);
        (*this->raw2newname_)[fun_name] = newfunname;
        panda::es2panda::util::StringView name_view = panda::es2panda::util::StringView(*new std::string(newfunname));
        auto funname_id = AllocNode<panda::es2panda::ir::Identifier>(this, name_view);
                

        funcNode->SetIdent(funname_id);

        this->id2block[graph->GetStartBlock()->GetId()] = body;

        //this->lcaFinder = std::make_unique<LCAFinder>(graph);

    }

    ~AstGen() override = default;
    bool RunImpl() override;

    const char *GetPassName() const override
    {
        return "AstGen";
    }

    bool GetStatus() const
    {
        return success_;
    }

    const ArenaVector<BasicBlock *> &GetBlocksToVisit() const override
    {
        return GetGraph()->GetBlocksRPO();
    }
    
    static void VisitSpillFill(GraphVisitor *visitor, Inst *inst);
    static void VisitConstant(GraphVisitor *visitor, Inst *inst);
    static void VisitCatchPhi(GraphVisitor *visitor, Inst *inst);

    static void VisitIf(GraphVisitor *v, Inst *inst_base);
    static void VisitIfImm(GraphVisitor *v, Inst *inst_base);
    static void IfImmZero(GraphVisitor *v, Inst *inst_base);
    static void VisitIntrinsic(GraphVisitor *visitor, Inst *inst_base);
    static void VisitLoadString(GraphVisitor *v, Inst *inst_base);
    static void VisitReturn(GraphVisitor *v, Inst *inst_base);

    static void VisitCastValueToAnyType(GraphVisitor *v, Inst *inst_base);

    static void VisitEcma(GraphVisitor *v, Inst *inst_base);
    static void IfEcma(GraphVisitor *v, compiler::IfInst *inst);

    static void VisitPhi(GraphVisitor* v, Inst* inst_base);
    static void VisitTry(GraphVisitor* v, Inst* inst_base);
    static void VisitSaveState(GraphVisitor* v, Inst* inst_base);
    static void VisitParameter(GraphVisitor* v, Inst* inst_base);
    
    BasicBlock* FindNearestVisitedPred(const std::vector<BasicBlock*>& visited, BasicBlock* block);
    panda::es2panda::ir::Expression* InverseTestExpression(AstGen *enc, Inst* inst_base, uint32_t ret, panda::es2panda::ir::Expression* src_expression, bool swap_truefalse);

    template <typename T, typename... Args>
    static T *AllocNode(AstGen * astgen, Args &&... args)
    {
        auto ret = astgen->parser_program_->Allocator()->New<T>(std::forward<Args>(args)...);
        if (ret == nullptr) {
            std::cout << "Unsuccessful allocation during parsing" << std::endl;;
        }
        return ret;
    }

    void VisitDefault(Inst *inst) override
    {
        LOG(ERROR, BYTECODE_OPTIMIZER) << "Opcode " << compiler::GetOpcodeString(inst->GetOpcode())
                                       << " not yet implemented in codegen";
        success_ = false;
    }

    panda::es2panda::ir::Expression* getParameterName(int argnum){
        // if(enc->memberfuncs_->find(enc->methodoffset_) != enc->memberfuncs_->end() && inst->GetArgNumber() < 3){
        //     arg = enc->GetIdentifierByName("this");
        // }else{
        //      arg = enc->GetIdentifierByName("arg" + std::to_string(inst->GetArgNumber()-3));
        // }

        panda::es2panda::ir::Expression* arg = nullptr;
        if(argnum == 0){
            arg = this->GetIdentifierByName("FunctionObject");
        }else if(argnum == 1){
            arg = this->GetIdentifierByName("NewTarget");
        }else if(argnum == 2){
            arg = this->GetIdentifierByName("this");
        }else{
            arg = this->GetIdentifierByName("arg" + std::to_string(argnum - 3));
        }
        return arg;
    }

    void MarkAsync(){
        auto funcNode = (*this->method2scriptfunast_)[this->methodoffset_];
        funcNode->AddFlag(es2panda::ir::ScriptFunctionFlags::ASYNC);  
    }

    es2panda::ir::BlockStatement* CopyAndCreateNewBlockStatement(es2panda::ir::Statement* rawblockstatement){
        ArenaVector<panda::es2panda::ir::Statement *> statements(this->parser_program_->Allocator()->Adapter());
        auto new_block_statement = AllocNode<es2panda::ir::BlockStatement>(this, nullptr, std::move(statements));

        int insertpos = statements.size();
        for(auto rawstatement : rawblockstatement->AsBlockStatement()->Statements()){
            new_block_statement->AddStatementAtPos(insertpos , rawstatement);
            insertpos++;
        }
        std::cout <<  "#CopyAndCreateNewBlockStatement: " << new_block_statement->Statements().size() << std::endl;;

        return new_block_statement;
    }

    void LocateAndRmoveStatement(const std::vector<BasicBlock*>& visited, panda::es2panda::ir::Statement *blockstatement, panda::es2panda::ir::Statement *statement){
        std::cout << "#LocateAndRmoveStatement search:  >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>" << std::endl;
        auto& statements = blockstatement->AsBlockStatement()->statements_;
        std::cout << "@@@@@@ pre blocksize: " <<  statements.size() << std::endl;
    
        auto it = statements.begin();
        while (it != statements.end()) {
            if (*it == statement) {
                it = statements.erase(it);
                this->inserted_statements.erase(statement);;
            } else {
                ++it;
            }
        }
        
    }


    void LocateAndReplaceAST(const std::vector<BasicBlock*>& visited, compiler::BasicBlock* block, panda::es2panda::ir::Statement *oldstatement, panda::es2panda::ir::Statement *newstatement){
        if (visited.empty()){
            HandleError("#LocateAndReplaceAST: locate block failed1");
        }

        ArenaVector<BasicBlock*> preds = block->GetPredsBlocks();
        if(preds.empty()){
            HandleError("#LocateAndReplaceAST: locate block failed2");
        }

        std::cout << "#LocateAndReplaceAST search >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>" << std::endl;
        for (BasicBlock* pre : preds) {
            if (pre == nullptr || pre == block || !contains(visited, pre)) {
                continue;
            }

            ArenaVector<panda::es2panda::ir::Statement *> statements(this->parser_program_->Allocator()->Adapter());
            auto new_block_statement = AllocNode<es2panda::ir::BlockStatement>(this, nullptr, std::move(statements));

            for(auto pre_singlestatement : this->GetBlockStatementById(pre)->AsBlockStatement()->Statements()){
                // locate branch ins: while/dowhile/if/try
                if(pre_singlestatement->IsWhileStatement() && pre_singlestatement->AsWhileStatement()->Body() == oldstatement){
                    this->id2block[block->GetId()] = new_block_statement;
                    this->AddInstAst2BlockStatemntByBlock(block, newstatement);
                    pre_singlestatement->AsWhileStatement()->body_ = new_block_statement;
                }else if(pre_singlestatement->IsIfStatement() ){

                }else if(pre_singlestatement->IsDoWhileStatement()){

                }else if(pre_singlestatement->IsTryStatement()){

                }

            }    
            
        }


        // for (BasicBlock* block : preds) {
        //     if (block != nullptr && ) {
        //         block->PrintInfo();
        //     }
        // }

        // std::unordered_set<BasicBlock*> pred_set(preds.begin(), preds.end());
        // for (auto it = visited.rbegin(); it != visited.rend(); ++it) {
        //     if (pred_set.find(*it) != pred_set.end()) {
        //         return *it;  
        //     }
        // }

        // auto fatherstatement = this->GetBlockStatementById(nearestpre);



    }


    std::string RemovePrefixOfFunc(const std::string& input) {
        if(this->raw2newname_->find(input) != this->raw2newname_->end()){
            return (*this->raw2newname_)[input];
        }

        auto coarsename = RemoveArgumentsOfFunc(input);

        size_t hashPos = coarsename.find_last_of('#');
        std::string result;
        if (hashPos != std::string::npos) {
            result = coarsename.substr(hashPos + 1);
        } else {
            result = coarsename;
        }
        
        if(result == "" || result.rfind("^", 0) == 0){
            result =  "func_" + std::to_string(count++);
        }

        if (result.rfind("#~@0>#", 0) == 0){
            result = input.substr(6);
            if(result == "" || result.rfind("^", 0) == 0){
                result = "func_" + std::to_string(count++);
            }
        }

        (*this->raw2newname_)[input] = result;

        return result;
        
    }

    panda::es2panda::ir::NumberLiteral* GetLiteralByNum(uint32_t index){
        panda::es2panda::ir::NumberLiteral* literal;
        if (this->num2literals.find(index)  != this->num2literals.end()) {
            literal = this->num2literals[index];
        } else {
            literal = AllocNode<panda::es2panda::ir::NumberLiteral>(this, index);
            this->num2literals[index] = literal;
        }
        return literal;
    }

    panda::es2panda::ir::Identifier* GetIdentifierByReg(compiler::Register reg){
        panda::es2panda::ir::Identifier* identifier;
        if (this->identifers.find(reg)  != this->identifers.end()) {
            identifier =  this->identifers[reg];
        } else {
            std::string* raw_name =  new std::string("v" + std::to_string(reg));

            panda::es2panda::util::StringView reg_name = panda::es2panda::util::StringView( *raw_name);
            identifier = AllocNode<panda::es2panda::ir::Identifier>(this, reg_name);
            this->identifers[reg] = identifier;
            this->str2identifers[*raw_name ] = identifier;
        }
        return identifier;
    }

    panda::es2panda::ir::Identifier* GetIdentifierByName(std::string raw_name){
        panda::es2panda::ir::Identifier* identifier;
        if (this->str2identifers.find(raw_name)  != this->str2identifers.end()) {
            identifier = this->str2identifers[raw_name];
        } else {
            panda::es2panda::util::StringView name_view = panda::es2panda::util::StringView(*new std::string(raw_name));
            identifier = AllocNode<panda::es2panda::ir::Identifier>(this, name_view);
            this->str2identifers[raw_name] = identifier;
        }
        return identifier;
    }


    panda::es2panda::ir::Identifier* GetIdentifierByName(std::string* raw_name){
        return this->GetIdentifierByName(*raw_name);
    }

    // uint32_t DetectDepthOfAST(es2panda::ir::Expression* expression){
    //     uint32_t astdepth = 0;
    //     expression->Iterate([&astdepth](const es2panda::ir::AstNode *astNode) -> void {
    //         std::cout << "astdepth: " << astdepth++ << std::endl;
    //     });

    //     return astdepth;
    // }


    uint32_t DetectComplexOfAST(es2panda::ir::Expression* expression){
        if (expression == nullptr) return 0;

        uint32_t astcomplex = 0;
        
        std::function<void(const es2panda::ir::AstNode*)> analyzeNode = 
            [&](const es2panda::ir::AstNode* node) {
            if (node == nullptr) return;

            std::cout << "astcomplex: " << astcomplex++ << std::endl;
            switch (node->Type()) {
                case es2panda::ir::AstNodeType::BINARY_EXPRESSION: {
                    auto binaryExpr = node->AsBinaryExpression();
                    analyzeNode(binaryExpr->Left());
                    analyzeNode(binaryExpr->Right());
                    return;
                }
                default:
                    return;
            }
            return;
        };


        analyzeNode(expression);

        return astcomplex;
    }

    void HandleNewCreatedExpression(Inst* inst, es2panda::ir::Expression* expression){
        if(inst->HasUsers()){
            
            this->SetExpressionByRegister(inst, inst->GetDstReg(), expression);

            auto curtargetid = inst->GetId();
            auto astcomplex = this->DetectComplexOfAST(expression);

            // A call has side effects, so materialising it into `vN = call(); ... vN`
            // is required when its result is consumed at MORE THAN ONE site (inlining
            // would duplicate the call). But when the result has a SINGLE user, the
            // call runs exactly once at that one use site whether we inline or not —
            // so inlining is safe and yields far more readable source (e.g.
            // `return JSON.stringify(x)` instead of `v8 = JSON.stringify(x); return v8`).
            // This removes the bulk of leftover temp registers without reordering effects.
            bool call_needs_materialize = expression->IsCallExpression() && !inst->HasSingleUser();

            // A mutable reference value (array/object literal, `new` result) has
            // IDENTITY: inlining it at >1 use site yields a SEPARATE object per
            // use, so mutations (`arr.push(x)`) and looped reads diverge / are
            // lost. Materialise into `vN = [...]; ... vN` so every use refers to
            // the one object. Single-use is safe to inline. Without this,
            // `const pairs=[...]; pairs.push(lt)` had the push land on a throwaway
            // literal rebuilt fresh at each of its use sites.
            bool mutable_ref_needs_materialize =
                (expression->IsArrayExpression() || expression->IsObjectExpression() ||
                 expression->IsNewExpression()) && !inst->HasSingleUser();

            if(astcomplex>5 || call_needs_materialize || mutable_ref_needs_materialize ||
               this->undefinedregids.find(curtargetid) != this->undefinedregids.end()){
                // dealwith untraved_reference
                auto dst_reg_identifier = this->GetIdentifierByReg(curtargetid);
                auto assignexpression = AllocNode<es2panda::ir::AssignmentExpression>(this, 
                                                                                    dst_reg_identifier,
                                                                                    expression,
                                                                                    es2panda::lexer::TokenType::PUNCTUATOR_SUBSTITUTION
                                                                                );
                auto assignstatement = AllocNode<es2panda::ir::ExpressionStatement>(this, assignexpression);
                this->AddInstAst2BlockStatemntByInst(inst, assignstatement);

                this->SetExpressionByRegister(inst, inst->GetDstReg(), dst_reg_identifier);
            }
        }else{
            if(expression->IsCallExpression()){
                auto callstatement = AllocNode<es2panda::ir::ExpressionStatement>(this, expression);
                this->AddInstAst2BlockStatemntByInst(inst, callstatement);
            }

        }
    }

    std::optional<panda::es2panda::ir::Expression*> GetExpressionByAcc(Inst* inst){
        return this->GetExpressionByRegIndex(inst, inst->GetInputsCount() - 2);
    }

    std::optional<panda::es2panda::ir::Expression*> GetExpressionByRegIndex(Inst* inst, uint32_t index){
        auto id = inst->GetInput(index).GetInst()->GetId();
        
        auto it = this->id2expression.find(id);
        if (it != this->id2expression.end()) {
            std::cout << "#GetExpressionByRegister: " << std::to_string(id) << std::endl;
            return it->second;  
        }
        

        auto untraveled_var =  this->GetIdentifierByReg(id); 
        this->SetExpressionById(id, untraveled_var);

        this->undefinedregids.insert(id);

        return untraveled_var;
        //HandleError("can't find expression in reg2expression: " + std::to_string(id));
        //return std::nullopt;
    }

    void SetExpressionById(uint32_t id, panda::es2panda::ir::Expression* value){
        if(value == nullptr){
            HandleError("#SetExpressionByRegister: can't set null expression in reg2expression");
        }
        this->id2expression[id] = value;
    }


    std::optional<panda::es2panda::ir::Expression*> GetExpressionByRegister(Inst* inst, compiler::Register key){
        auto it = this->reg2expression.find(key);
        if (it != this->reg2expression.end()) {
            std::cout << "#GetExpressionByRegister: " << std::to_string(key) << std::endl;
            return it->second;  
        }

        HandleError("can't find expression in reg2expression: " + std::to_string(key));
        
        return std::nullopt;
    }


    void SetExpressionByRegister(Inst* inst, compiler::Register key, panda::es2panda::ir::Expression* value){
        if(value == nullptr){
            HandleError("#SetExpressionByRegister: can't set null expression in reg2expression");
        }
        this->SetExpressionById(inst->GetId(), value);

        std::cout << "#SetExpressionByRegister: " << std::to_string(key) << std::endl;
        
        this->reg2expression[key] = value;
    }

    void Logid2BlockKeys(){
        std::cout << "id2block keys: ";
        for (const auto& pair : this->id2block) {
            std::cout << pair.first << ", ";
        }
        std::cout << std::endl;
    }

    void LogCurLexicalIndexes(Inst* inst){
        if(this->bb2lexicalenvstack_[inst->GetBasicBlock()]->Empty()){
            return;
        }
        this->bb2lexicalenvstack_[inst->GetBasicBlock()]->GetLexicalEnv(0).LogIndexes();
    }

    void LogSpecialBlockId(){
        std::cout << "specialblockid: ";
        for (auto it = this->specialblockid.begin(); it != this->specialblockid.end(); ++it) {
            std::cout << *it;
            if (std::next(it) != this->specialblockid.end()) {
                std::cout << ", ";
            }
        }
        std::cout << std::endl;
    }
 
    void AddInstAst2BlockStatemntByInst(Inst *inst, es2panda::ir::Statement *statement){
        BasicBlock* block = inst->GetBasicBlock();
        this->AddInstAst2BlockStatemntByBlock(block, statement);

        //if(block->IsLoopValid() && block->IsLoopHeader() && inst->GetOpcode()!= Opcode::If   && inst->GetOpcode()!= Opcode::IfImm ){
        if(block->IsLoopValid() && block->IsLoopHeader() && this->loopbranches_.find(inst) == this->loopbranches_.end()){
            auto headerblockstatements = this->whileheader2redundant[block];
            const auto &statements = headerblockstatements->Statements();
            headerblockstatements->AddStatementAtPos(statements.size(), statement);
        }
    }

    panda::es2panda::ir::Expression *GetExpressionByLiteral(panda::pandasm::LiteralArray::Literal literal){
        /*
            std::variant<bool, uint8_t, uint16_t, uint32_t, uint64_t, float, double, std::string> value_;

        */
        panda::es2panda::ir::Expression *tmp = nullptr;
        if(literal.IsBoolValue()){
            tmp = AllocNode<panda::es2panda::ir::BooleanLiteral>(this, std::get<bool>(literal.value_));
        }else if(literal.IsByteValue()){
            tmp = AllocNode<panda::es2panda::ir::NumberLiteral>(this, std::get<uint8_t>(literal.value_));
        }else if(literal.IsShortValue() || literal.tag_ == panda::panda_file::LiteralTag::METHODAFFILIATE){
            tmp = AllocNode<panda::es2panda::ir::NumberLiteral>(this, std::get<uint16_t>(literal.value_));
        }else if(literal.IsIntegerValue()){
            tmp = AllocNode<panda::es2panda::ir::NumberLiteral>(this, std::get<uint32_t>(literal.value_));
        }else if(literal.IsLongValue()){
            tmp = AllocNode<panda::es2panda::ir::NumberLiteral>(this, std::get<uint64_t>(literal.value_));
        }else if(literal.IsFloatValue()){
            tmp = AllocNode<panda::es2panda::ir::NumberLiteral>(this, std::get<float>(literal.value_));
        }else if(literal.IsDoubleValue()){
            tmp = AllocNode<panda::es2panda::ir::NumberLiteral>(this, std::get<double>(literal.value_));
        }else if(literal.IsStringValue() || literal.tag_ == panda::panda_file::LiteralTag::LITERALARRAY ){
            panda::es2panda::util::StringView literal_strview(* new std::string(std::get<std::string>(literal.value_)));
            tmp = AllocNode<panda::es2panda::ir::StringLiteral>(this, literal_strview);
        }else{
            // METHODAFFILIATE = 0x0a  
            // ASYNCMETHOD = 0x18
            // LITERALARRAY = 0x19
            std::cout << "value tag: " << static_cast<int>(literal.tag_) << std::endl;
            HandleError("unsupport literal type error");
        }
        return tmp;
    }

    void AddInstAst2BlockStatemntByBlock(BasicBlock* block, es2panda::ir::Statement *statement, uint32_t offset = 0){
        if(this->inserted_statements.find(statement ) == this->inserted_statements.end() ){
            this->inserted_statements.insert(statement);
        }else{
            return;
        }

        es2panda::ir::BlockStatement* block_statements = this->GetBlockStatementById(block);
        const auto &statements = block_statements->Statements();
        if(statements.size() > offset) {
            block_statements->AddStatementAtPos(statements.size() - offset, statement);
        }else{
            block_statements->AddStatementAtPos(statements.size() , statement);
        }
    }

    bool father_visited(BasicBlock *block){
        if(block->IsStartBlock()) {
            return true;
        }
        
        for(uint32_t index = 0; index < block->GetPredsBlocks().size(); index ++){
            auto father = block->GetPredecessor(index);

            if(std::find(visited.begin(), visited.end(), father) != visited.end()){
                return true;
            }
        }

        return false;
    }

    // Chase an IfImm/If tested instruction through transparent truthiness
    // wrappers (callruntime.istrue / callruntime.isfalse) to the underlying
    // condition value, and report whether an odd number of `isfalse` wrappers
    // were crossed (which inverts truthy<->falsy).
    Inst* UnwrapTruthiness(Inst* tested, bool* inverted){
        *inverted = false;
        Inst* cur = tested;
        for(int guard = 0; cur != nullptr && guard < 8; ++guard){
            if(!cur->IsIntrinsic()){
                break;
            }
            auto iid = cur->CastToIntrinsic()->GetIntrinsicId();
            if(iid == compiler::RuntimeInterface::IntrinsicId::CALLRUNTIME_ISTRUE_PREF_IMM8){
                cur = cur->GetInput(0).GetInst();
            }else if(iid == compiler::RuntimeInterface::IntrinsicId::CALLRUNTIME_ISFALSE_PREF_IMM8){
                *inverted = !*inverted;
                cur = cur->GetInput(0).GetInst();
            }else{
                break;
            }
        }
        return cur;
    }

    // Chase `inst` through transparent `tonumeric` wrappers to the value below.
    Inst* UnwrapToNumeric(Inst* inst){
        Inst* cur = inst;
        for(int guard = 0; cur != nullptr && guard < 8; ++guard){
            if(cur->IsIntrinsic() &&
               cur->CastToIntrinsic()->GetIntrinsicId() ==
                   compiler::RuntimeInterface::IntrinsicId::TONUMERIC_IMM8){
                cur = cur->GetInput(0).GetInst();
            }else{
                break;
            }
        }
        return cur;
    }

    // Is `edge_value` (a phi back-edge input) the induction step `phi +/- 1`?
    // i.e. an inc/dec whose operand is (modulo tonumeric) the phi itself. If so,
    // return the +/- token so the back-edge can render as `i = i + 1` directly,
    // instead of `i = vTmp; vTmp = i + 1` (which is wrong: vTmp is undefined on
    // the first iteration, and may collide with an unrelated reused register).
    bool DetectInductionStep(compiler::PhiInst* phi, Inst* edge_value,
                             es2panda::lexer::TokenType* op_out){
        Inst* step = UnwrapToNumeric(edge_value);
        if(step == nullptr || !step->IsIntrinsic()){
            return false;
        }
        auto iid = step->CastToIntrinsic()->GetIntrinsicId();
        if(iid != compiler::RuntimeInterface::IntrinsicId::INC_IMM8 &&
           iid != compiler::RuntimeInterface::IntrinsicId::DEC_IMM8){
            return false;
        }
        Inst* base = UnwrapToNumeric(step->GetInput(0).GetInst());
        if(base != static_cast<Inst*>(phi)){
            return false;
        }
        // The step value must feed ONLY this phi. If the inc/dec result (or the
        // tonumeric wrapping it) is read elsewhere — e.g. `arr[i-1]` snapshots the
        // decremented value — folding to `i = i +/- 1` would make those reads see
        // the new counter (an off-by-one). Keep the materialised `vTmp` then.
        for(auto& u : step->GetUsers()){
            if(u.GetInst() != nullptr && !u.GetInst()->IsPhi() &&
               u.GetInst() != edge_value){
                return false;
            }
        }
        if(edge_value != step){
            for(auto& u : edge_value->GetUsers()){
                if(u.GetInst() != nullptr && !u.GetInst()->IsPhi()){
                    return false;
                }
            }
        }
        *op_out = (iid == compiler::RuntimeInterface::IntrinsicId::INC_IMM8)
                      ? es2panda::lexer::TokenType::PUNCTUATOR_PLUS
                      : es2panda::lexer::TokenType::PUNCTUATOR_MINUS;
        return true;
    }

    // Does this block terminate control flow (end in return / throw)? Such a
    // block is always a branch body, never a post-construct continuation join.
    // Does this block start with a Phi? A phi means the block is a value-merge
    // (a continuation join), not a pure branch body — even if it also returns.
    bool BlockHasPhi(BasicBlock* block){
        if(block == nullptr){
            return false;
        }
        for([[maybe_unused]] auto* p : block->PhiInsts()){
            return true;
        }
        return false;
    }

    // Does control go from IfImm block `C` to `succ` when C's SEMANTIC condition
    // is TRUE? Accounts for the istrue/isfalse wrapper parity + CC + which
    // (true/false) successor `succ` is. Returns false if not determinable.
    bool EdgeTakenOnConditionTrue(BasicBlock* C, BasicBlock* succ){
        Inst* last = C->GetLastInst();
        if(last == nullptr || last->GetOpcode() != Opcode::IfImm){
            return false;
        }
        auto* ifimm = last->CastToIfImm();
        bool inverted = false;
        UnwrapTruthiness(last->GetInput(0).GetInst(), &inverted);
        bool true_succ_on_tested_truthy = (ifimm->GetCc() == compiler::CC_NE);
        bool succ_is_true = (C->GetTrueSuccessor() == succ);
        bool succ_on_tested_truthy =
            succ_is_true ? true_succ_on_tested_truthy : !true_succ_on_tested_truthy;
        // undo isfalse parity -> truthiness of the underlying (semantic) condition
        return inverted ? !succ_on_tested_truthy : succ_on_tested_truthy;
    }

    // Is `block` a pure DEFAULT merge reached only via SEMANTICALLY-FALSE edges?
    // This is the `Y` of an `A && B ? X : Y` ternary: every predecessor is an
    // IfImm and control reaches `block` exactly when that predecessor's condition
    // is FALSE (a condition failed -> default). istrue/isfalse aware.
    bool IsDefaultMergeOfFalseEdges(BasicBlock* block){
        if(block == nullptr || block->GetPredsBlocks().empty()){
            return false;
        }
        for(auto* pred : block->GetPredsBlocks()){
            Inst* last = pred->GetLastInst();
            if(last == nullptr || last->GetOpcode() != Opcode::IfImm){
                return false;
            }
            if(EdgeTakenOnConditionTrue(pred, block)){
                return false;  // reached when condition TRUE -> not a default edge
            }
        }
        return true;
    }

    // Build the boolean expression true when control leaves IfImm block `C`
    // toward successor `succ` (resolving istrue/isfalse parity + CC + which
    // successor). nullptr if not buildable.
    es2panda::ir::Expression* BuildBranchCondition(BasicBlock* C, BasicBlock* succ){
        if(C == nullptr || succ == nullptr){
            return nullptr;
        }
        auto it = this->block2rawtest_.find(C);
        if(it == this->block2rawtest_.end() || it->second == nullptr){
            return nullptr;
        }
        Inst* last = C->GetLastInst();
        if(last == nullptr || last->GetOpcode() != Opcode::IfImm){
            return nullptr;
        }
        auto* ifimm = last->CastToIfImm();
        bool inverted = false;
        UnwrapTruthiness(last->GetInput(0).GetInst(), &inverted);
        bool true_succ_on_tested_truthy = (ifimm->GetCc() == compiler::CC_NE);
        bool succ_is_true = (C->GetTrueSuccessor() == succ);
        bool succ_on_tested_truthy =
            succ_is_true ? true_succ_on_tested_truthy : !true_succ_on_tested_truthy;
        bool succ_on_raw_truthy = inverted ? !succ_on_tested_truthy : succ_on_tested_truthy;
        es2panda::ir::Expression* raw = it->second;
        if(succ_on_raw_truthy){
            return raw;
        }
        return AllocNode<es2panda::ir::UnaryExpression>(
            this, raw, es2panda::lexer::TokenType::PUNCTUATOR_EXCLAMATION_MARK);
    }

    // Detect a value-select ternary `cond ? X : Y` whose value merges at `phi`
    // (default Y reached only via if-FALSE edges; value X reached when all path
    // conditions hold). Builds a ConditionalExpression. nullptr if not this shape.
    es2panda::ir::Expression* TryBuildValueSelectTernary(compiler::PhiInst* phi){
        if(phi->GetInputsCount() != 2){
            return nullptr;
        }
        BasicBlock* merge = phi->GetBasicBlock();
        BasicBlock* dom = merge->GetDominator();
        if(dom == nullptr || dom->GetLastInst() == nullptr ||
           dom->GetLastInst()->GetOpcode() != Opcode::IfImm){
            return nullptr;
        }
        BasicBlock* b0 = phi->GetPhiInputBb(0);
        BasicBlock* b1 = phi->GetPhiInputBb(1);
        if(b0 == nullptr || b1 == nullptr){
            return nullptr;
        }
        // The DEFAULT (Y) is the shallow merge of if-FALSE edges; the VALUE (X) is
        // the deeper block (dominated by more conditions). A `&&`-chain false-merge
        // (e.g. bb3 reached via bb2's false edge) can also look like a false-edge
        // merge, so disambiguate by dominance: X is dominated by Y's block region,
        // i.e. X is the one with the longer dominator chain. Require exactly one
        // side to be the false-edge default.
        bool b0_def = IsDefaultMergeOfFalseEdges(b0);
        bool b1_def = IsDefaultMergeOfFalseEdges(b1);
        int y_idx = -1;
        if(b0_def && !b1_def){
            y_idx = 0;
        }else if(b1_def && !b0_def){
            y_idx = 1;
        }else if(b0_def && b1_def){
            // both look like false-merges: Y is the one with MORE predecessors
            // (the genuine multi-edge fail-merge); X is the deeper single-pred one.
            if(b0->GetPredsBlocks().size() != b1->GetPredsBlocks().size()){
                y_idx = (b0->GetPredsBlocks().size() > b1->GetPredsBlocks().size()) ? 0 : 1;
            }else{
                return nullptr; // ambiguous
            }
        }
        if(y_idx < 0){
            return nullptr;
        }
        size_t x_idx = 1 - (size_t)y_idx;
        BasicBlock* xbb = phi->GetPhiInputBb(x_idx);
        if(xbb == nullptr || !dom->IsDominate(xbb)){
            return nullptr;
        }
        // Walk dominator chain from xbb up to dom, collecting the branch condition
        // at each IfImm toward the side that leads to xbb.
        std::vector<es2panda::ir::Expression*> conds;
        BasicBlock* cur = xbb;
        int guard = 0;
        while(cur != nullptr && guard++ < 64){
            BasicBlock* d = cur->GetDominator();
            if(d == nullptr){
                return nullptr;
            }
            if(d->GetLastInst() != nullptr && d->GetLastInst()->GetOpcode() == Opcode::IfImm){
                auto* c = BuildBranchCondition(d, cur);
                if(c == nullptr){
                    return nullptr;
                }
                conds.push_back(c);
            }
            if(d == dom){
                break;
            }
            cur = d;
        }
        if(conds.empty()){
            return nullptr;
        }
        // Every collected branch condition must be a real TEST, not a bare value
        // the raw-test reconstruction fell back to (a lone `undefined`/`null`/
        // constant). Building `(undefined ? X : Y)` is a misdetection — bail and
        // let the default per-edge phi handling emit it instead.
        for(auto* c : conds){
            if(!IsTestLikeExpression(c)){
                return nullptr;
            }
        }
        es2panda::ir::Expression* cond = conds[0];
        for(size_t i = 1; i < conds.size(); ++i){
            cond = AllocNode<es2panda::ir::BinaryExpression>(
                this, conds[i], cond, es2panda::lexer::TokenType::PUNCTUATOR_LOGICAL_AND);
        }
        auto xexpr = *GetExpressionByRegIndex(phi, x_idx);
        auto yexpr = *GetExpressionByRegIndex(phi, (size_t)y_idx);
        // Both selected values must be INLINABLE at the phi position. A value that
        // is an object/array literal — or a `vNN` temp that was materialised
        // inside one of the (now-collapsed) branch blocks via `vN={...}; vN.k=…` —
        // is NOT valid outside that block: inlining it into `cond ? X : Y` leaves
        // the construction statements stranded and references an unbound temp.
        // Bail to the default per-edge phi handling (verbose if/else) in that case.
        if(!IsInlinableTernaryValue(xexpr) || !IsInlinableTernaryValue(yexpr)){
            return nullptr;
        }
        return AllocNode<es2panda::ir::ConditionalExpression>(this, cond, xexpr, yexpr);
    }

    // Safe to inline as a ternary arm: a literal, a member access (`Color.Gray`),
    // a call, a binary/unary/conditional expr, or a NON-temp identifier (param /
    // `undefined`). Rejected: object/array literals (need block construction) and
    // bare `vNN` temps (may be materialised inside a collapsed branch block).
    bool IsInlinableTernaryValue(es2panda::ir::Expression* e){
        if(e == nullptr){
            return false;
        }
        if(e->IsObjectExpression() || e->IsArrayExpression()){
            return false;
        }
        if(e->IsIdentifier()){
            auto nm = e->AsIdentifier()->Name().Mutf8();
            // `vNN` temp register names are block-local materialised values.
            if(nm.size() >= 2 && nm[0] == 'v' && nm[1] >= '0' && nm[1] <= '9'){
                return false;
            }
            return true;
        }
        return true;
    }

    // Can `start` reach a NORMAL function exit (a `return`/`returnundefined`
    // block, or the graph end via a returning block) WITHOUT passing through
    // `avoid`? THROW exits do not count — an exception path bypassing a block does
    // not make that block a bypassable else (otherwise save's tail, which every
    // NORMAL path passes through but a `throw` edge skips, would be misclassified).
    // Used to tell a bypassable else-branch from a must-pass continuation.
    bool ReachesExitAvoiding(BasicBlock* start, BasicBlock* avoid){
        if(start == nullptr){
            return false;
        }
        std::set<BasicBlock*> seen;
        std::vector<BasicBlock*> stack{start};
        int guard = 0;
        while(!stack.empty() && guard++ < 512){
            BasicBlock* b = stack.back();
            stack.pop_back();
            if(b == avoid){
                continue;
            }
            if(!seen.insert(b).second){
                continue;
            }
            // A returning block reached without `avoid` => bypassable. Throw-only
            // exits are ignored (they are not normal continuations).
            bool is_return_exit = false;
            for(auto* inst : b->Insts()){
                if(inst->IsIntrinsic()){
                    auto iid = inst->CastToIntrinsic()->GetIntrinsicId();
                    if(iid == compiler::RuntimeInterface::IntrinsicId::RETURN ||
                       iid == compiler::RuntimeInterface::IntrinsicId::RETURNUNDEFINED){
                        is_return_exit = true;
                        break;
                    }
                }
            }
            if(is_return_exit){
                return true;
            }
            for(auto* s : b->GetSuccsBlocks()){
                stack.push_back(s);
            }
        }
        return false;
    }

    // A usable ternary/branch condition: a comparison/logical/unary/call, or a
    // member/identifier that is NOT a bare `undefined`/`null` placeholder. Rejects
    // raw literals and the undefined/null fallbacks of a failed reconstruction.
    bool IsTestLikeExpression(es2panda::ir::Expression* e){
        if(e == nullptr){
            return false;
        }
        if(e->IsBinaryExpression() || e->IsUnaryExpression() || e->IsCallExpression()){
            return true;
        }
        if(e->IsIdentifier()){
            auto nm = e->AsIdentifier()->Name().Mutf8();
            return nm != "undefined" && nm != "null" && nm != "hole" && nm != "NaN";
        }
        if(e->IsMemberExpression()){
            return true;
        }
        return false;
    }

    // Reachability of `target` from `start` within a bounded forward walk
    // (avoids loops via a visited set; bound keeps it cheap).
    bool ReachesBlock(BasicBlock* start, BasicBlock* target){
        if(start == nullptr || target == nullptr){
            return false;
        }
        std::set<BasicBlock*> seen;
        std::vector<BasicBlock*> stack{start};
        int guard = 0;
        while(!stack.empty() && guard++ < 512){
            BasicBlock* b = stack.back();
            stack.pop_back();
            if(b == target){
                return true;
            }
            if(!seen.insert(b).second){
                continue;
            }
            for(auto* s : b->GetSuccsBlocks()){
                stack.push_back(s);
            }
        }
        return false;
    }

    // Is `target` reachable from EXACTLY ONE of `ifblock`'s two successors? If
    // both successors reach it, `target` is a post-if continuation MERGE (it must
    // be emitted after the if, not captured as a branch/else). If only one does,
    // it is a genuine single-branch body (e.g. the shared return of
    // `if(a||b){return}`) and may be nested as the else.
    bool TargetIsSingleBranchBody(BasicBlock* ifblock, BasicBlock* target){
        if(ifblock == nullptr || ifblock->GetSuccsBlocks().size() < 2){
            return false;
        }
        BasicBlock* t = ifblock->GetTrueSuccessor();
        BasicBlock* f = ifblock->GetFalseSuccessor();
        // Walk from each successor but do NOT pass back through ifblock; treat
        // `target` itself as a sink (don't walk through it to the other side).
        bool fromTrue = ReachesBlockAvoiding(t, target, ifblock, target);
        bool fromFalse = ReachesBlockAvoiding(f, target, ifblock, target);
        return fromTrue != fromFalse;  // exactly one
    }

    bool ReachesBlockAvoiding(BasicBlock* start, BasicBlock* target,
                              BasicBlock* avoid, BasicBlock* sink){
        if(start == nullptr || target == nullptr){
            return false;
        }
        std::set<BasicBlock*> seen;
        std::vector<BasicBlock*> stack{start};
        int guard = 0;
        while(!stack.empty() && guard++ < 512){
            BasicBlock* b = stack.back();
            stack.pop_back();
            if(b == target){
                return true;
            }
            if(b == avoid || b == sink){
                continue;  // don't traverse through the if-block or past the target
            }
            if(!seen.insert(b).second){
                continue;
            }
            for(auto* s : b->GetSuccsBlocks()){
                stack.push_back(s);
            }
        }
        return false;
    }

    bool BlockTerminates(BasicBlock* block){
        if(block == nullptr){
            return false;
        }
        for(auto* inst : block->Insts()){
            if(!inst->IsIntrinsic()){
                continue;
            }
            auto iid = inst->CastToIntrinsic()->GetIntrinsicId();
            if(iid == compiler::RuntimeInterface::IntrinsicId::RETURN ||
               iid == compiler::RuntimeInterface::IntrinsicId::RETURNUNDEFINED ||
               iid == compiler::RuntimeInterface::IntrinsicId::THROW_PREF_NONE){
                return true;
            }
        }
        return false;
    }

    // Detect the short-circuit (a||b / a&&b) shape at a phi.
    //   - phi P has exactly 2 inputs.
    //   - one input value `cond` comes from a predecessor `condbb` that ends in
    //     IfImm, whose tested value (modulo istrue/isfalse) IS `cond`.
    //   - the merge block (P's block) is a *direct* successor of condbb (the
    //     short-circuit edge: cond's own truthiness is the merged result).
    // On success fills condbb / cond_input_idx / merge_on_truthy and returns true.
    bool DetectShortCircuitPhi(compiler::PhiInst* phi, BasicBlock** condbb_out,
                               size_t* cond_idx_out, bool* merge_on_truthy_out){
        if(phi->GetInputsCount() != 2){
            return false;
        }
        BasicBlock* merge = phi->GetBasicBlock();
        for(size_t i = 0; i < 2; ++i){
            BasicBlock* condbb = phi->GetPhiInputBb(i);
            if(condbb == nullptr){
                continue;
            }
            Inst* last = condbb->GetLastInst();
            if(last == nullptr || last->GetOpcode() != Opcode::IfImm){
                continue;
            }
            // merge must be a direct successor of the condition block
            if(condbb->GetTrueSuccessor() != merge && condbb->GetFalseSuccessor() != merge){
                continue;
            }
            bool inverted = false;
            Inst* base = UnwrapTruthiness(last->GetInput(0).GetInst(), &inverted);
            Inst* condval = phi->GetInput(i).GetInst();
            if(base != condval){
                continue;
            }
            // The IfImm: imm 0, CC NE/EQ. Determine on which truthiness of the
            // *tested* value control reaches `merge`, then undo any isfalse
            // inversion to get the truthiness of `cond` itself.
            auto ifimm = last->CastToIfImm();
            bool merge_is_true_succ = (condbb->GetTrueSuccessor() == merge);
            // CC_NE with imm 0: true successor taken when tested != 0 (truthy).
            // CC_EQ with imm 0: true successor taken when tested == 0 (falsy).
            bool true_succ_on_tested_truthy = (ifimm->GetCc() == compiler::CC_NE);
            bool merge_on_tested_truthy =
                merge_is_true_succ ? true_succ_on_tested_truthy : !true_succ_on_tested_truthy;
            bool merge_on_cond_truthy = inverted ? !merge_on_tested_truthy : merge_on_tested_truthy;
            *condbb_out = condbb;
            *cond_idx_out = i;
            *merge_on_truthy_out = merge_on_cond_truthy;
            return true;
        }
        return false;
    }

    // Predicate form used at IfImm time: is `block` a short-circuit condition
    // block? CRITICAL: this MUST agree exactly with DetectShortCircuitPhi — if
    // VisitIfImm suppresses the `if` here but VisitPhi later fails to rebuild the
    // logical expression, the branch vanishes with no fallback. So instead of a
    // looser independent check, we require that a phi in a direct successor is
    // *actually detected* by DetectShortCircuitPhi with THIS block as its condbb.
    bool IsShortCircuitConditionBlock(BasicBlock* block){
        if(block == nullptr){
            return false;
        }
        Inst* last = block->GetLastInst();
        if(last == nullptr || last->GetOpcode() != Opcode::IfImm){
            return false;
        }
        for(auto* succ : {block->GetTrueSuccessor(), block->GetFalseSuccessor()}){
            if(succ == nullptr){
                continue;
            }
            for(auto* p : succ->PhiInsts()){
                auto* phi = p->CastToPhi();
                BasicBlock* condbb = nullptr;
                size_t cond_idx = 0;
                bool merge_on_truthy = false;
                if(DetectShortCircuitPhi(phi, &condbb, &cond_idx, &merge_on_truthy) &&
                   condbb == block){
                    return true;
                }
            }
        }
        return false;
    }

    std::optional<std::string> GetNameFromExpression(es2panda::ir::Expression* rawexpression){
        if(rawexpression->IsIdentifier()){
            auto objname = rawexpression->AsIdentifier()->Name().Mutf8();
            return objname;
        }else if(rawexpression->IsStringLiteral()){
            auto idname = rawexpression->AsStringLiteral()->Str().Mutf8();
            return idname;
        }else if(rawexpression->IsMemberExpression()){
            auto rawobj = rawexpression->AsMemberExpression()->Object();
            auto rawprop = rawexpression->AsMemberExpression()->Property();

            auto objname = GetNameFromExpression(rawobj);
            auto propname = GetNameFromExpression(rawprop);

            return (objname ? *objname : std::string("__expr__")) + "." +
                   (propname ? *propname : std::string("__expr__"));
        }else if(rawexpression->IsCallExpression()){
            auto callee = rawexpression->AsCallExpression()->Callee();
            if(callee->IsFunctionExpression()){
                auto id = callee->AsFunctionExpression()->Function()->Id();
                return GetNameFromExpression(id);
            }else if(callee->IsIdentifier()){
                return GetNameFromExpression(callee);
            }else{
                std::cout << "###: " << std::to_string(static_cast<int>(callee->Type())) << std::endl;
                // Unknown callee shape: return a placeholder name rather than
                // aborting the whole decompile or returning nullopt (which some
                // callers deref). Callers comparing against real names won't match.
                return std::string("__expr__");
            }
        }else if(rawexpression->IsNullLiteral() ){
            return "null";
        }else if(rawexpression->IsBooleanLiteral()){
            return rawexpression->AsBooleanLiteral()->Value() == true ? "true" : "false";;
        }else if(rawexpression->IsStringLiteral()){
            return rawexpression->AsStringLiteral()->Str().Mutf8();
        }else if(rawexpression->IsBigIntLiteral()){
            return rawexpression->AsBigIntLiteral()->Str().Mutf8();
        }else if(rawexpression->IsNumberLiteral()){
            return std::to_string(rawexpression->AsNumberLiteral()->Number());
        }else if(rawexpression->IsNewExpression()){
            auto callee = rawexpression->AsNewExpression()->Callee();
            return GetNameFromExpression(const_cast<es2panda::ir::Expression*>(callee));
        }else if(rawexpression->IsConditionalExpression()){
            auto c = rawexpression->AsConditionalExpression();
            auto tn = GetNameFromExpression(const_cast<es2panda::ir::Expression*>(c->Test()));
            auto cn = GetNameFromExpression(const_cast<es2panda::ir::Expression*>(c->Consequent()));
            auto an = GetNameFromExpression(const_cast<es2panda::ir::Expression*>(c->Alternate()));
            return "(" + (tn?*tn:std::string("__expr__")) + " ? " +
                   (cn?*cn:std::string("__expr__")) + " : " +
                   (an?*an:std::string("__expr__")) + ")";
        }else if(rawexpression->IsBinaryExpression()){
            auto binexpression = rawexpression->AsBinaryExpression();
            auto tokentype = es2panda::lexer::TokenToString(binexpression->OperatorType());

            auto left = GetNameFromExpression(binexpression->Left());
            auto right = GetNameFromExpression(binexpression->Right());

            return (left ? *left : std::string("__expr__")) + " " + tokentype + " " +
                   (right ? *right : std::string("__expr__"));
        }else if(rawexpression->IsArrayExpression()){
            std::stringstream ss_;
            int count = 0;
            ss_ << "[";
            auto arrayexpression = rawexpression->AsArrayExpression();
            int array_size = arrayexpression->Elements().size();
            for (auto *it : arrayexpression->Elements()) {
                if(it->IsStringLiteral()){
                    ss_ << "\"";
                }
                ss_ <<*this->GetNameFromExpression(it);
                if(it->IsStringLiteral()){
                    ss_ << "\"";
                }
                if(++count < array_size ){
                    ss_ << ", ";
                }
            }

            ss_ << "]";
            return ss_.str();
        }else if(rawexpression->IsSpreadElement()){
            auto spreadxpression = rawexpression->AsSpreadElement();
            return "..." + *this->GetNameFromExpression(spreadxpression->Argument());
        }else if(rawexpression->IsObjectExpression()){
            std::stringstream ss_;
            ss_ << "{";
            auto objectexpression = rawexpression->AsObjectExpression();
            size_t properties_size = objectexpression->Properties().size();
            size_t count = 1;
            for (auto *it : objectexpression->Properties()) {
                switch (it->Type()) {
                    case es2panda::ir::AstNodeType::PROPERTY: {
                        auto propertyexpression = it->AsProperty();
                        ss_ << *this->GetNameFromExpression(propertyexpression->Key());
                        ss_ << " : ";
                        ss_ << *this->GetNameFromExpression(propertyexpression->Value());
                        
                        if(count++ < properties_size)
                            ss_ << ", ";
                        
                        break;
                    }
                    default: {
                        ss_ << *this->GetNameFromExpression(it);
                        if(count++ < properties_size)
                            ss_ << ", ";
                        break;
                    }
                }
            }
            ss_ << "}";
            return ss_.str();
        }else{
            std::cout << "###1: " << std::to_string(static_cast<int>(rawexpression->Type())) << std::endl;
            // Unknown expression shape: placeholder rather than abort/nullopt.
            return std::string("__expr__");
        }
        return nullptr;
    }

    es2panda::ir::BlockStatement* GetBlockStatementById(BasicBlock *block){
        auto block_id = block->GetId();
        std::cout << "[*] GetBlockStatementById bbid: " << block_id << ", ";


        if(!father_visited(block)){
            HandleError("#GetBlockStatementById : cann't find father except for root" );
        }

        // case1: found blockstatment
        if (this->id2block.find(block_id) != this->id2block.end()) {
            std::cout << "@@ case 1" << std::endl;
            return this->id2block[block_id];
        }
        
        // case2: found loop
        if(block->IsLoopValid() && block->IsLoopHeader() ){
            std::cout << "@@ case 2" << std::endl;
            JudgeLoopType(block, this->loop2type, this->loop2exit, this->backedge2dowhileloop);

            //////////////////////////////////////////////////////////////////////////////////////
            ArenaVector<panda::es2panda::ir::Statement *> statements(this->parser_program_->Allocator()->Adapter());
            auto new_block_statement = AllocNode<es2panda::ir::BlockStatement>(this, nullptr, std::move(statements));
            
            this->id2block[block_id] = new_block_statement;
            return this->id2block[block_id];
        }

        ///////////////////////////////////////////////////////////////////////////////////////////

        // case3: found unique predecessor with unique successor
        if(block->GetPredsBlocks().size() == 1 && !block->IsStartBlock() && block->GetPredecessor(0)->GetSuccsBlocks().size() == 1){
            std::cout << "@@ case 3" << std::endl;
            BasicBlock* ancestor_block = block->GetPredecessor(0);

            if(this->id2block.find(ancestor_block->GetId()) != this->id2block.end()){
                this->id2block[block_id] =  this->id2block[ancestor_block->GetId()];
            }else{
                this->Logid2BlockKeys();
                HandleError("#GetBlockStatementById: find ancestor error: ", block_id);
            }
            

            return this->id2block[block_id];
        }
        
        // case4: found multi predecessor with onlyif
        if(block->GetPredsBlocks().size() == 2 && !block->IsStartBlock() && ( block->GetPredecessor(0)->IsIfBlock() || block->GetPredecessor(1)->IsIfBlock() )
        ){
            if(block->GetPredecessor(0)->IsIfBlock() && this->id2block.find(block->GetPredecessor(0)->GetId()) != this->id2block.end()){
                this->id2block[block_id] =  this->id2block[block->GetPredecessor(0)->GetId()];
            }else if(block->GetPredecessor(1)->IsIfBlock() && this->id2block.find(block->GetPredecessor(1)->GetId()) != this->id2block.end()){
                this->id2block[block_id] =  this->id2block[block->GetPredecessor(1)->GetId()];
            }else{
                this->id2block[block_id] =  this->GetBlockStatementById(block->GetPredecessor(0));
            }
        }

        // case5:create new statements
        std::cout << "@@ case 5" << std::endl;
        ArenaVector<panda::es2panda::ir::Statement *> statements(this->parser_program_->Allocator()->Adapter());
        auto new_block_statement = AllocNode<es2panda::ir::BlockStatement>(this, nullptr, std::move(statements));

        this->id2block[block_id] = new_block_statement;

        LogSpecialBlockId();

        // nested if-else
        if(this->specialblockid.find(block_id) == this->specialblockid.end() ){
            BasicBlock* ancestor_block = nullptr;

            ancestor_block = block->GetDominator();

            if(ancestor_block == nullptr){
                HandleError("GetBlockStatementById# find ancestor is nullptr");
            }
            std::cout << "@ ancestor_block: " <<  std::to_string(ancestor_block->GetId()) <<  std::endl;

            auto ancestor_block_statements = this->GetBlockStatementById(ancestor_block);
            this->id2block[block_id] =  ancestor_block_statements;

            this->AddInstAst2BlockStatemntByBlock(ancestor_block, new_block_statement);
            

            return this->id2block[block_id];
        }else{
            //HandleError("GetBlockStatementById# unsupported case, blockid: ", block_id);
            /**
             * add statement in special statements
             * a) if
             * b) try-catch 
             * c) ...
            */

        }
        
        es2panda::ir::BlockStatement* curstatement = nullptr;
        if(this->id2block.find(block_id) != this->id2block.end()){
            curstatement = this->id2block[block_id];
        }else{
            HandleError("GetBlockStatementById# unsupported case, blockid: ", block_id);
        }
        return curstatement;
    }

#include "compiler/optimizer/ir/visitor.inc"

private:
    void VisitTryBegin(const compiler::BasicBlock *bb);

public:
    pandasm::Function *function_;
    const BytecodeOptIrInterface *ir_interface_;
    pandasm::Program *program_;

    uint32_t methodoffset_;
    uint32_t closure_count;

    uint32_t privatevar_count;

    std::set<es2panda::ir::Statement*> inserted_statements;

    std::map<uint32_t, LexicalEnvStack*>* method2lexicalenvstack_;
    std::map<uint32_t, LexicalEnvStack*>* method2sendablelexicalenvstack_;

    std::map<uint32_t, std::string*> *patchvarspace_;

    es2panda::parser::Program* parser_program_;

    std::map<size_t, std::vector<std::string>> index2namespaces_;
    std::vector<std::string> localnamespaces_;
    std::vector<std::string> importnamespaces_;
    std::map<std::string, std::vector<std::string>>* recordimportnamespaces_;
    std::map<uint32_t, std::set<uint32_t>> *class2memberfuns_;
    std::map<uint32_t, panda::es2panda::ir::ScriptFunction *> *method2scriptfunast_;
    std::map<uint32_t, panda::es2panda::ir::ClassDeclaration *>* ctor2classdeclast_;

    std::set<uint32_t> *memberfuncs_;

    std::map<uint32_t, panda::es2panda::ir::Expression*> *class2father_;

    std::map<uint32_t, std::map<uint32_t,  std::set<size_t>>>* method2lexicalmap_;

    std::vector<LexicalEnvStack*> *globallexical_waitlist_;
    std::vector<LexicalEnvStack*> *globalsendablelexical_waitlist_;

    std::map<std::string, std::string> *raw2newname_;

    std::set<Inst *> loopbranches_;
    std::set<compiler::BasicBlock *> loopbranchblocks_;

    std::map<std::string, uint32_t> *methodname2offset_;

    std::string fun_name_;

    
    ///////////////////////////////////////////////////////////////////////////////////////
    uint32_t current_constructor_offset;

    std::unique_ptr<LCAFinder> lcaFinder;

    bool success_ {true};

    std::string* acc_global_str = NULL;

    panda::es2panda::ir::Expression* acc = NULL;

    panda::es2panda::ir::Expression* thisptr= NULL;

    std::map<compiler::Loop *, uint32_t> loop2type; // 0-while, 1-dowhile
    std::map<compiler::Loop *, BasicBlock*> loop2exit; 
    std::map<compiler::BasicBlock*, compiler::Loop *> backedge2dowhileloop;

    std::map<compiler::BasicBlock*, es2panda::ir::BlockStatement*> whileheader2redundant;
    std::map<compiler::BasicBlock*, es2panda::ir::BlockStatement*> whilebody2redundant;

    std::map<compiler::BasicBlock*, es2panda::ir::BlockStatement*> phiref2pendingredundant;

    // Short-circuit boolean reconstruction (a||b / a&&b). When a condition block
    // feeds its own tested value into a phi at a direct-successor merge block, the
    // diamond CFG is really a logical expression, not an if-statement. We collect
    // such condition blocks so VisitIfImm skips emitting an `if`, and the phi at
    // the merge rebuilds `cond || other` / `cond && other` instead of two
    // clobbering temp assignments (which dropped the first operand entirely).
    std::set<compiler::BasicBlock*> shortcircuit_condblocks_;

    // The IfStatement emitted for each condition block, so a phi's default
    // (fall-through) edge can be inserted right before THE matching if (not just
    // the first if in a possibly-aliased/shared block statement).
    std::map<compiler::BasicBlock*, es2panda::ir::IfStatement*> block2ifstatement_;

    // Raw reconstructed test expression (operand 0 of the IfImm) for each
    // condition block — used to rebuild a value-select ternary's path condition.
    std::map<compiler::BasicBlock*, es2panda::ir::Expression*> block2rawtest_;

    std::map<uint32_t, es2panda::ir::BlockStatement*> id2block;

    std::set<uint32_t> specialblockid;

    std::map<compiler::Register, panda::es2panda::ir::Identifier*> identifers;
    

    std::map<std::string, panda::es2panda::ir::Identifier*> str2identifers;
    std::map<uint32_t, panda::es2panda::ir::NumberLiteral*> num2literals;

    std::map<compiler::Register, panda::es2panda::ir::Expression*> reg2expression;

    LexicalEnv* acc_lexicalenv = NULL;

    std::map<compiler::BasicBlock*, LexicalEnvStack*> bb2lexicalenvstack_;

    std::map<compiler::BasicBlock*, LexicalEnvStack*> bb2sendablelexicalenvstack_;

    std::map<uint32_t, panda::es2panda::ir::Expression*> id2expression;
    std::set<uint32_t> undefinedregids;

    std::vector<BasicBlock *> visited;
    
    std::map<uint32_t, es2panda::ir::BlockStatement*> tyrid2block;
    std::map<uint32_t, panda::es2panda::ir::TryStatement*> tyridtrystatement;
    std::map<uint32_t, panda::es2panda::ir::CatchClause*> tyrid2catchclause;


    std::set<panda::es2panda::ir::Expression*> not_add_assgin_for_stlexvar; // class and instance_initializer

    panda::es2panda::ir::Identifier* constant_undefined = AllocNode<panda::es2panda::ir::Identifier>(this, "undefined");
    panda::es2panda::ir::Identifier* constant_hole = AllocNode<panda::es2panda::ir::Identifier>(this, "hole");

    panda::es2panda::ir::Identifier* constant_nan = AllocNode<panda::es2panda::ir::Identifier>(this, "NaN");
    panda::es2panda::ir::Identifier* constant_infinity = AllocNode<panda::es2panda::ir::Identifier>(this, "infinity");
    panda::es2panda::ir::Identifier* constant_catcherror = AllocNode<panda::es2panda::ir::Identifier>(this, "error");
    panda::es2panda::ir::Identifier* constant_symbol = AllocNode<panda::es2panda::ir::Identifier>(this, "Symbol");
    panda::es2panda::ir::Identifier* constant_this = AllocNode<panda::es2panda::ir::Identifier>(this, "this");
    panda::es2panda::ir::Identifier* constant_global = AllocNode<panda::es2panda::ir::Identifier>(this, "global");
    panda::es2panda::ir::Identifier* constant_newtarget = AllocNode<panda::es2panda::ir::Identifier>(this, "new.target");
    panda::es2panda::ir::Identifier* constant_restargs = AllocNode<panda::es2panda::ir::Identifier>(this, "args");
    panda::es2panda::ir::Identifier* constant_arguments = AllocNode<panda::es2panda::ir::Identifier>(this, "arguments"); 

    panda::es2panda::ir::Identifier* constant_asyncfuncmark = AllocNode<panda::es2panda::ir::Identifier>(this, "asyncfunc");

    panda::es2panda::ir::Identifier* constant_genratorcmark = AllocNode<panda::es2panda::ir::Identifier>(this, "generatorobj");

    panda::es2panda::ir::Identifier* constant_itercmark = AllocNode<panda::es2panda::ir::Identifier>(this, "iterresultobj");


    panda::es2panda::ir::Identifier* constant_resumemode = AllocNode<panda::es2panda::ir::Identifier>(this, "whether_resume_generator");

    panda::es2panda::ir::Identifier* constant_rejectmode = AllocNode<panda::es2panda::ir::Identifier>(this, "reject_resume_generator");


    panda::es2panda::ir::Expression* suspendobj = nullptr;

    panda::es2panda::ir::ScriptFunction* scriptfunc = nullptr;


    panda::es2panda::ir::BooleanLiteral* constant_true = AllocNode<panda::es2panda::ir::BooleanLiteral>(this, true);
    panda::es2panda::ir::BooleanLiteral* constant_false = AllocNode<panda::es2panda::ir::BooleanLiteral>(this, false);
    panda::es2panda::ir::NullLiteral* constant_null = AllocNode<panda::es2panda::ir::NullLiteral>(this);

    panda::es2panda::ir::NumberLiteral* constant_one = AllocNode<panda::es2panda::ir::NumberLiteral>(this, 1);
    panda::es2panda::ir::NumberLiteral* constant_zero = AllocNode<panda::es2panda::ir::NumberLiteral>(this, 0);


};

}

#endif
