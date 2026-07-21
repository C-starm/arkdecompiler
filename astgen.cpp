#include "astgen.h"

#include "astgen_ecma.inl"

namespace panda::bytecodeopt {

using panda_file::LiteralTag;


void AstGen::VisitTryBegin(const compiler::BasicBlock *bb)
{
    XABC_DBG << "[+] VisitTryBegin  >>>>>>>>>>>>>>>>>" << std::endl;
    XABC_DBG << "[-] VisitTryBegin  >>>>>>>>>>>>>>>>>" << std::endl;
}


BasicBlock* AstGen::FindNearestVisitedPred(const std::vector<BasicBlock*>& visited, BasicBlock* block) {
    if (visited.empty()) return nullptr;
    
    ArenaVector<BasicBlock*> preds = block->GetPredsBlocks();
    if (preds.empty()) return nullptr;
    
    std::unordered_set<BasicBlock*> pred_set(preds.begin(), preds.end());
    

    for (auto it = visited.rbegin(); it != visited.rend(); ++it) {
        if (pred_set.find(*it) != pred_set.end()) {
            return *it;  
        }
    }
    
    return nullptr; 
}

bool AstGen::RunImpl()
{
    
    for (auto *bb : GetGraph()->GetBlocksRPO()) {
        XABC_DBG << "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ visit bbid: " << bb->GetId() << std::endl;
        //if(bb->IsLoopValid() && !bb->GetLoop()->IsRoot() ){
        if(bb->IsLoopValid()  ){
            auto loop = bb->GetLoop();
            auto backedges = loop->GetBackEdges();
            auto innerloop = loop->GetInnerLoops();
            auto blocks = loop->GetBlocks();
            XABC_DBG << "Loop Size: " << backedges.size()  << " , innerloop: " << innerloop.size()  << " , block: " << blocks.size() << std::endl;

        }


        if(bb->IsLoopValid() && bb->IsLoopHeader()){ 
            JudgeLoopType(bb, this->loop2type, this->loop2exit, this->backedge2dowhileloop);
            /////////////////////////////////////////////////////////////////
            ArenaVector<panda::es2panda::ir::Statement *> statements(this->parser_program_->Allocator()->Adapter());
            auto new_block_statement =  AllocNode<es2panda::ir::BlockStatement>(this, nullptr, std::move(statements));
            this->whileheader2redundant[bb] = new_block_statement;
        }

        

        ///////////////////////////////////////////////////////////////////////////////////////////////////
        auto nearestpre = this->FindNearestVisitedPred(this->visited, bb);


        if(bb != this->GetGraph()->GetStartBlock()) {
            if(nearestpre != nullptr && this->bb2lexicalenvstack_[nearestpre] != nullptr){
                XABC_DBG << "!!!!!!!!!!!!!!!!!!!! found pre id for bb2lexicalenvstack_: " << nearestpre->GetId() << std::endl;
                this->bb2lexicalenvstack_[bb] = new LexicalEnvStack(*this->bb2lexicalenvstack_[nearestpre]);
                this->bb2sendablelexicalenvstack_[bb] = new LexicalEnvStack(*this->bb2sendablelexicalenvstack_[nearestpre]);

                XABC_DBG << "size: " << (*this->bb2lexicalenvstack_[nearestpre]).Size()  << std::endl;
                XABC_DBG << "sendable size: " << (*this->bb2sendablelexicalenvstack_[nearestpre]).Size()  << std::endl;
            }else{
                XABC_DBG << "!!!!!!!!!!!!!!!!!!!! not found pre id for bb2lexicalenvstack_: "<< "curid: " << bb->GetId()  << std::endl;
                this->bb2lexicalenvstack_[bb] = new LexicalEnvStack();
                this->bb2sendablelexicalenvstack_[bb] = new LexicalEnvStack();
            }
        }
        
        this->visited.push_back(bb);        
        ///////////////////////////////////////////////////////////////////////////////////////////////////
        this->GetBlockStatementById(bb);

        for (const auto &inst : bb->AllInsts()) {
            VisitInstruction(inst);
            if (!GetStatus()) {
                return false;
            }
        }

        // check whehter add break statement
        if(!bb->IsStartBlock()){
            BasicBlock* father = bb->GetPredecessor(0);
            if(father->IsLoopValid() && !father->GetLoop()->IsRoot()){
                if(bb->GetLoop() != father->GetLoop()  ){
                    if(bb->GetSuccsBlocks().size() == 1){
                        //XABC_DBG << "truesucc: " << bb->GetTrueSuccessor()->GetId() << ", falsesucc: " <<  loop2exit[father->GetLoop()]->GetId() << std::endl;
                        if(bb->GetTrueSuccessor() == loop2exit[father->GetLoop()]){
                            this->GetBlockStatementById(bb);
                            auto breakstatement = AllocNode<es2panda::ir::BreakStatement>(this);

                            this->AddInstAst2BlockStatemntByBlock(bb, breakstatement);

                        }
                    }

                }
            }
        }


        uint32_t offset = 0;
        if(bb->IsIfBlock()){
            offset = 1;
        }
        //check if add redundant block
        if(this->whilebody2redundant.find(bb) != this->whilebody2redundant.end()){
            this->inserted_statements.erase(this->whilebody2redundant[bb]);

            this->AddInstAst2BlockStatemntByBlock(bb, this->whilebody2redundant[bb], 1);
            this->whilebody2redundant.erase(bb);
        }
    
        if(this->phiref2pendingredundant.find(bb) != this->phiref2pendingredundant.end()){
            this->inserted_statements.erase(this->phiref2pendingredundant[bb]);

            // Deferred phi back-edge assignments normally go at offset 1 (before a
            // trailing if, and — happily — before the inline `index = guard - 1`
            // step of a reverse loop, which must run AFTER the `guard = index`
            // copy). BUT a lone induction increment (`i = i + N`) whose loop body's
            // last statements READ `i` (e.g. HMAC `ipad[i]=..; opad[i]=..`) must go
            // at the very END, or it lands between those reads and corrupts them.
            // Detect that exact case (single statement, `i = i <op> literal`) and
            // append at the end; everything else keeps the offset-1 behaviour.
            uint32_t phi_offset = 1;
            auto* pend = this->phiref2pendingredundant[bb];
            if(pend->Statements().size() == 1){
                auto* st = pend->Statements()[0];
                if(st->IsExpressionStatement() &&
                   st->AsExpressionStatement()->GetExpression()->IsAssignmentExpression()){
                    auto* asg = st->AsExpressionStatement()->GetExpression()->AsAssignmentExpression();
                    auto* rhs = asg->Right();
                    if(asg->Left()->IsIdentifier() && rhs->IsBinaryExpression()){
                        auto* be = rhs->AsBinaryExpression();
                        // i <op> <literal>  with left operand == the assigned i
                        if(be->Left()->IsIdentifier() &&
                           be->Left()->AsIdentifier()->Name().Mutf8() ==
                               asg->Left()->AsIdentifier()->Name().Mutf8() &&
                           be->Right()->IsNumberLiteral()){
                            phi_offset = 0;  // append at end
                        }
                    }
                }
            }
            this->AddInstAst2BlockStatemntByBlock(bb, this->phiref2pendingredundant[bb], phi_offset);
            this->phiref2pendingredundant.erase(bb);
        }
        
    }

    if (!GetStatus()) {
        return false;
    }

    // Final pass: append each registered `continue`-shaped loop's induction step
    // to the TAIL of its (now fully built) while-body container, so `i = i +/- N`
    // runs every iteration including the continue path. Done here (not during
    // while-construction) because the body is empty at construction time —
    // appending then would put the step at the body TOP (off-by-one).
    this->FlushLatchHoists();

    // Visit try-blocks in order they were declared
    for (auto *bb : GetGraph()->GetTryBeginBlocks()) {
        VisitTryBegin(bb);
    }

    return true;
}


void AstGen::VisitSpillFill(GraphVisitor *visitor, Inst *inst_base)
{
    XABC_DBG << "[+] VisitSpillFill  >>>>>>>>>>>>>>>>>" << std::endl;
    auto *enc = static_cast<AstGen *>(visitor);
    auto inst = inst_base->CastToSpillFill();

    for (auto sf : inst->GetSpillFills()) {
        if(sf.SrcType() != compiler::LocationType::REGISTER || sf.DstType() != compiler::LocationType::REGISTER ){
            return; // skip unsupported SpillFill type
        }
        auto it = enc->reg2expression.find(sf.SrcValue());
        if (it == enc->reg2expression.end()) {
            XABC_DBG << "VisitSpillFill # SpillFill none register"  << std::endl; 
        }else{
            enc->SetExpressionByRegister(inst, sf.DstValue(), *enc->GetExpressionByRegister(inst, sf.SrcValue()));
        }
    }
    XABC_DBG << "[-] VisitSpillFill  >>>>>>>>>>>>>>>>>" << std::endl;
}


void AstGen::VisitConstant(GraphVisitor *visitor, Inst *inst_base)
{
    XABC_DBG << "[+] VisitConstant  >>>>>>>>>>>>>>>>>" << std::endl;
    auto *enc = static_cast<AstGen *>(visitor);
    auto inst = inst_base->CastToConstant();
    auto type = inst->GetType();
   
    es2panda::ir::Expression* number;
    switch (type) {
        case compiler::DataType::INT64:
        case compiler::DataType::UINT64:
            number = AllocNode<es2panda::ir::NumberLiteral>(enc, 
                                                            inst->GetInt64Value()
                                                        );
            break;
        case compiler::DataType::FLOAT64:
            number = AllocNode<es2panda::ir::NumberLiteral>(enc, 
                                                            inst->GetDoubleValue()
                                                        );
            break;
        case compiler::DataType::INT32:
        case compiler::DataType::UINT32:
            number = AllocNode<es2panda::ir::NumberLiteral>(enc, 
                                                            inst->GetInt32Value()
                                                        );
            break;
        default:
            XABC_DBG << "S3" << std::endl;
            UNREACHABLE();
            LOG(ERROR, BYTECODE_OPTIMIZER) << "VisitConstant with unknown type" << type;
            enc->success_ = false;
    }

    enc->SetExpressionByRegister(inst, inst->GetDstReg(), number);
    
    XABC_DBG << "[-] VisitConstant  >>>>>>>>>>>>>>>>>" << std::endl;
}


void AstGen::VisitIf(GraphVisitor *v, Inst *inst_base)
{
    XABC_DBG << "[+] VisitIf  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen *>(v);
    auto inst = inst_base->CastToIf();

    auto left_expression = *enc->GetExpressionByRegIndex(inst, 0);
    auto right_expression = *enc->GetExpressionByRegIndex(inst, 1);

    panda::es2panda::ir::Expression* test_expression;

    switch (inst->GetCc()) {
        case compiler::CC_EQ:
            test_expression = AllocNode<es2panda::ir::BinaryExpression>(enc, 
                                                        left_expression,
                                                        right_expression,
                                                        BinIntrinsicIdToToken(compiler::RuntimeInterface::IntrinsicId::EQ_IMM8_V8));
            break;
        case compiler::CC_NE:
            test_expression = AllocNode<es2panda::ir::BinaryExpression>(enc, 
                                                        left_expression,
                                                        right_expression,
                                                        BinIntrinsicIdToToken(compiler::RuntimeInterface::IntrinsicId::NOTEQ_IMM8_V8));
            break;
        default:
            XABC_DBG << "S5" << std::endl;
            UNREACHABLE();
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////
    /// deal with while/do-while
    auto block = inst->GetBasicBlock();
    auto block_statement = enc->GetBlockStatementById(block);

    if(block->IsLoopValid() && block->IsLoopHeader()){
        XABC_DBG << "1%%%%%%%%%%%%%%%%%!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        if(enc->loop2type[block->GetLoop()] == 1){
            XABC_DBG << "[+] do-while ===" << std::endl;

            XABC_DBG << "[-] do-while ===" << std::endl;
        }else{
            XABC_DBG << "[+] while ===" << std::endl;

            auto true_statements =   enc->GetBlockStatementById(inst->GetBasicBlock()->GetTrueSuccessor());
            auto false_statements =  enc->GetBlockStatementById(inst->GetBasicBlock()->GetFalseSuccessor());
    
            if(enc->loop2exit[inst->GetBasicBlock()->GetLoop() ] == inst->GetBasicBlock()->GetTrueSuccessor() ){
                std::swap(true_statements, false_statements);
            }

            auto whilestatement = AllocNode<es2panda::ir::WhileStatement>(enc,
                                    nullptr,
                                    test_expression, 
                                    true_statements);

            enc->AddInstAst2BlockStatemntByInst(inst, whilestatement);
            enc->AddInstAst2BlockStatemntByInst(inst, false_statements);

            XABC_DBG << "[-] while ===" << std::endl;
        }
    }else{
        XABC_DBG << "2%%%%%%%%%%%%%%%%%!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        auto true_statements =   enc->GetBlockStatementById(inst->GetBasicBlock()->GetTrueSuccessor());
        auto false_statements =  enc->GetBlockStatementById(inst->GetBasicBlock()->GetFalseSuccessor());

        auto ifStatement = AllocNode<es2panda::ir::IfStatement>(enc, test_expression, true_statements, false_statements);
        true_statements->SetParent(block_statement);
        false_statements->SetParent(block_statement);
        
        enc->AddInstAst2BlockStatemntByInst(inst, ifStatement);

    }

    /////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////

    XABC_DBG << "[-] VisitIf  >>>>>>>>>>>>>>>>>" << std::endl;
}

bool IsLoopBranch(AstGen *enc, BasicBlock *block);  // defined below

uint32_t onlyOneBranch(BasicBlock* father, AstGen * enc){
    //XABC_DBG << "if block: " << std::to_string(father->GetId()) << std::endl;
    auto true_branch = father->GetTrueSuccessor();
    auto false_branch = father->GetFalseSuccessor();

    // 0: if-and-else

    // 1: only if

    // 2: only else

    // loop-header === 0
    if(father->IsLoopValid() && father->IsLoopHeader()){
        return 0;
    }

    BasicBlock* analysis_block = nullptr;
    if(true_branch->GetPredsBlocks().size() >= 2){
        analysis_block = true_branch;
    }else if(false_branch->GetPredsBlocks().size() >= 2){
        analysis_block = false_branch;
    }else if(true_branch->GetPredsBlocks().size() == 1 && false_branch->GetPredsBlocks().size() == 1){
        return 0;
    }else{
        return 0; // unhandled branch shape -> sentinel
    }

    // If `father` does not dominate the join candidate, that join belongs to an
    // OUTER scope (it is reachable without going through this if). It must NOT be
    // pulled into this if as a branch body — doing so duplicates the merge/tail
    // into every predecessor and drops it from the non-dominated path (the
    // missing-else / triplicated-tail bug). Emit only the successor this if
    // actually dominates; the join is emitted later at its own dominator.
    //
    // BUT: only when the candidate is a genuine *continuation* join. A block that
    // TERMINATES (ends in return/throw) AND has no phi is a pure branch body,
    // never a post-construct join — e.g. `if (a || b) { return false; }` lowers
    // to two conditions whose taken edges share one return block; that return
    // block is the else of the enclosing if and must stay nested, not hoisted.
    // A terminating block that HAS a phi is still a value-merge continuation
    // (e.g. `let s=...; if(a && b){s+=x} ...; return s` where the `&&` lowers to
    // nested ifs and the return-merge has >1 pred): it must be hoisted, else the
    // tail+return gets buried in one branch and the other paths return undefined.
    // Loop-condition / break branches are left to the existing loop handling
    // (VisitIfImm forces ret=0 for IsLoopBranch blocks), so skip those here.
    // NB: async functions appear wholly "loop valid" (generator-resume loop), so
    // we must NOT skip on IsLoopValid alone — use the precise IsLoopBranch.
    bool candidate_is_pure_branch_body =
        enc->BlockTerminates(analysis_block) && !enc->BlockHasPhi(analysis_block);
    // Nested-else exemption: when `analysis_block` is `father`'s OWN direct
    // successor AND father's OTHER successor can reach an exit WITHOUT passing
    // through it, the block is a bypassable structured else of a nested
    // `if(A){ if(B){x}else{shared} }` (father=B's if; `shared` also reached when
    // !A). Flattening it here scrambles the nesting. A true post-if continuation
    // (save's tail) is NOT bypassable — the other branch must pass through it —
    // so it still hoists.
    bool candidate_is_nested_else = false;
    if(father->GetTrueSuccessor() == analysis_block ||
       father->GetFalseSuccessor() == analysis_block){
        BasicBlock* other = (father->GetTrueSuccessor() == analysis_block)
                                ? father->GetFalseSuccessor()
                                : father->GetTrueSuccessor();
        candidate_is_nested_else = enc->ReachesExitAvoiding(other, analysis_block);
    }
    if(!father->IsDominate(analysis_block) &&
       !candidate_is_pure_branch_body &&
       !candidate_is_nested_else &&
       !IsLoopBranch(enc, father) &&
       !(father->IsLoopValid() && father->IsLoopHeader())){
        if(analysis_block == true_branch){
            // true successor is the outer join -> only the (dominated) else body
            return 2;
        }else{
            // false successor is the outer join -> only the (dominated) if body
            return 1;
        }
    }

    BasicBlock* other_father = nullptr;
    BasicBlock* start_block = father->GetGraph()->GetStartBlock();
    if(analysis_block->GetPredecessor(0) == father){
        other_father = analysis_block->GetPredecessor(1);
    }else{
        other_father = analysis_block->GetPredecessor(0);
    }

    if(contains(enc->visited, other_father)){
        // other_father is father's ancestor
        // while (1 === 1) {
        //     let x;
        //     if (x == 1) {
        //         break;
        //     }
        // }

        // do {
        //     let x;
        //     if (x == 1) {
        //         break;
        //     }
        // } while (1 === 1)

        // let x;
        // for (let y = 0; y < 1; ++y) {
        //     if (x == 1) {
        //         break;
        //     }
        //}

        return 0;
    }

    XABC_DBG << "analysis_block: " << std::to_string(analysis_block->GetId()) << std::endl;
    XABC_DBG << "true branch: " << std::to_string(true_branch->GetId()) << std::endl;
    XABC_DBG << "false_branch: " << std::to_string(false_branch->GetId()) << std::endl;
    XABC_DBG << "father: " << std::to_string(father->GetId()) << std::endl;
    XABC_DBG << "other_fater: " << std::to_string(other_father->GetId()) << std::endl;

    uint32_t count = 0;
    while(other_father != father && other_father != start_block){
        XABC_DBG << "count: " << count << std::endl;
        other_father = other_father->GetPredecessor(0);
        XABC_DBG << "predecessor id: " << other_father->GetId() << std::endl;
    }

    if(other_father == father ){
        if(analysis_block == true_branch){
            return 2;
        }else{
            return 1;
        }
    }else if(other_father == start_block){
        return 0;
    }else{
        //XABC_DBG << "end other_father: " << std::to_string(other_father->GetId()) << std::endl;
        return 0; // bad method -> sentinel
    }
    
    return 0;
}

panda::es2panda::ir::Expression* AstGen::InverseTestExpression(AstGen *enc, Inst* inst_base, uint32_t ret, panda::es2panda::ir::Expression* src_expression, bool swap_truefalse){
    [[maybe_unused]] auto inst = inst_base->CastToIfImm();
    auto src_binary = src_expression->AsBinaryExpression();
    auto raw_opeator = src_binary->OperatorType();
    auto new_operator = BinInverseToken2Token(raw_opeator);

    if(raw_opeator != new_operator){
        // The compare instruction can have MORE THAN ONE user (e.g. it also feeds
        // a short-circuit phi). Inverting the operator IN PLACE would corrupt
        // every other use (turning `a === b` into `a !== b` at the phi too).
        // Build a fresh inverted node and leave the shared original untouched.
        if((swap_truefalse == false && inst->GetCc() == compiler::CC_EQ) ||
           (swap_truefalse == true  && inst->GetCc() == compiler::CC_NE)){
            auto new_src_expression = AllocNode<es2panda::ir::BinaryExpression>(enc,
                                                        src_binary->Left(),
                                                        src_binary->Right(),
                                                        new_operator);
            return new_src_expression;
        }else{
            return src_expression;
        }
    }else{
        panda::compiler::RuntimeInterface::IntrinsicId cmpid;
        if(swap_truefalse == true){
            if(inst->GetCc() == compiler::CC_EQ){
                cmpid = compiler::RuntimeInterface::IntrinsicId::NOTEQ_IMM8_V8;
            }else{
                cmpid = compiler::RuntimeInterface::IntrinsicId::EQ_IMM8_V8;
            }   
        }else{
            if(inst->GetCc() == compiler::CC_EQ){
                cmpid = compiler::RuntimeInterface::IntrinsicId::EQ_IMM8_V8;
            }else{
                cmpid = compiler::RuntimeInterface::IntrinsicId::NOTEQ_IMM8_V8;
            }       
        }

        auto new_src_expression = AllocNode<es2panda::ir::BinaryExpression>(enc,
                                                    src_expression,
                                                    enc->constant_zero,
                                                    BinIntrinsicIdToToken(cmpid));
        return new_src_expression;
    }
}

bool IsLoopBranch(AstGen *enc, BasicBlock *block){
    if(enc->backedge2dowhileloop.find(block) != enc->backedge2dowhileloop.end()){
        return true;
    }

    if(block->IsLoopValid() && !block->GetLoop()->IsRoot() && IsLoopConditionBranch(block)  &&  enc->loop2type[block->GetLoop()] == 0 &&
        (block->IsLoopHeader() || enc->loopbranchblocks_.find(block->GetLoop()->GetHeader()) == enc->loopbranchblocks_.end())
    ){
        return true;
    }

    return false;
}
void AstGen::VisitIfImm(GraphVisitor *v, Inst *inst_base)
{
    XABC_DBG << "[+] VisitIfImm  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen *>(v);
    auto inst = inst_base->CastToIfImm();
    auto imm = inst->GetImm();
    auto block = inst->GetBasicBlock();
    panda::es2panda::ir::Expression* test_expression;

    // Short-circuit boolean (a||b / a&&b): this branch is not real control flow,
    // it is the evaluation of a logical operator whose result is merged by a phi
    // at the successor. Do NOT emit an `if` — the merge phi rebuilds the logical
    // expression. The tested compare's expression is already bound in
    // id2expression, so the phi can read it. Skip loop headers (genuine loops).
    if (imm == 0 && !IsLoopBranch(enc, block) &&
        enc->backedge2dowhileloop.find(block) == enc->backedge2dowhileloop.end() &&
        !(block->IsLoopValid() && block->IsLoopHeader()) &&
        enc->IsShortCircuitConditionBlock(block)) {
        enc->shortcircuit_condblocks_.insert(block);
        XABC_DBG << "[short-circuit] skip if for bb " << block->GetId() << std::endl;
        XABC_DBG << "[-] VisitIfImm  >>>>>>>>>>>>>>>>>" << std::endl;
        return;
    }

    if (imm == 0) {
        auto src_expression = *enc->GetExpressionByRegIndex(inst, 0);

        // Remember the SEMANTIC condition expression (unwrapping istrue/isfalse)
        // so a value-select ternary whose value merges at a downstream phi can
        // rebuild its path condition (A && B ? X : Y) with correct polarity. The
        // IfImm operand may be `isfalse(A)`/`istrue(A)`; we want `A`'s expression.
        {
            bool inv_unused = false;
            Inst* base = enc->UnwrapTruthiness(inst->GetInput(0).GetInst(), &inv_unused);
            es2panda::ir::Expression* cond_expr = src_expression;
            if(base != nullptr){
                auto it = enc->id2expression.find(base->GetId());
                if(it != enc->id2expression.end() && it->second != nullptr){
                    cond_expr = it->second;
                }
            }
            if(cond_expr != nullptr){
                enc->block2rawtest_[block] = cond_expr;
            }
        }

        auto ret = onlyOneBranch(inst->GetBasicBlock(), enc);

        es2panda::ir::Statement* true_statements = nullptr;
        es2panda::ir::Statement* false_statements = nullptr;
        
        // if(enc->backedge2dowhileloop.find(block) == enc->backedge2dowhileloop.end() && block->IsLoopValid() && !IsLoopHasMultipleBackEdges(block) &&
        //     enc->loop2exit[block->GetLoop()] == block->GetTrueSuccessor() ) {
        //         // break statement type1
        //         // var chain;
        //         // var chain2;
        //         // var chain3;
        //         // while (chain || chain2 || chain3 ) {
        //         //     if (chain === getTextOfChainNode(chain)) {
        //         //         break;
        //         //     }
        //         // }
        //         // while (chain) {
        //         //     if (chain !== chain2){
        //         //         break;
        //         //     }
        //         // }
        //         ret = 2;
        //         enc->specialblockid.insert(block->GetFalseSuccessor()->GetId());
        //         false_statements =   enc->GetBlockStatementById(block->GetFalseSuccessor()); 
        // }else if(enc->backedge2dowhileloop.find(block) == enc->backedge2dowhileloop.end() && block->IsLoopValid() && !IsLoopHasMultipleBackEdges(block) && 
        //         enc->loop2exit[block->GetLoop()] == block->GetFalseSuccessor()){
        //         // break statement type1
        //         ret = 1;
        //         enc->specialblockid.insert(block->GetTrueSuccessor()->GetId());
        //         true_statements =   enc->GetBlockStatementById(block->GetTrueSuccessor());
        // }else{
/*             if(ret == 0){
                XABC_DBG << "#VisitIfImm ret case: " << ret << std::endl;
                enc->specialblockid.insert(block->GetTrueSuccessor()->GetId());
                enc->specialblockid.insert(block->GetFalseSuccessor()->GetId());
                
                false_statements =  enc->GetBlockStatementById(block->GetFalseSuccessor());
                true_statements =   enc->GetBlockStatementById(block->GetTrueSuccessor());
            }else if(ret == 1){
                XABC_DBG << "#VisitIfImm ret case: " << ret << std::endl;
                enc->specialblockid.insert(block->GetTrueSuccessor()->GetId());
                true_statements =   enc->GetBlockStatementById(block->GetTrueSuccessor());
            }else{
                XABC_DBG << "#VisitIfImm ret case: " << ret << std::endl;
                enc->specialblockid.insert(block->GetFalseSuccessor()->GetId());
                false_statements =   enc->GetBlockStatementById(block->GetFalseSuccessor());
            } */
        //}

        /*
            // 0: if-and-else
            // 1: only if
            // 2: only else
        */
            if(ret == 0 || IsLoopBranch(enc, block)){
                XABC_DBG << "#VisitIfImm ret case: " << ret << std::endl;
                enc->specialblockid.insert(block->GetTrueSuccessor()->GetId());
                enc->specialblockid.insert(block->GetFalseSuccessor()->GetId());

                false_statements =  enc->GetBlockStatementById(block->GetFalseSuccessor());
                true_statements =   enc->GetBlockStatementById(block->GetTrueSuccessor());
                ret = 0;
            }else if(ret == 1){
                XABC_DBG << "#VisitIfImm ret case: " << ret << std::endl;
                enc->specialblockid.insert(block->GetTrueSuccessor()->GetId());
                true_statements =   enc->GetBlockStatementById(block->GetTrueSuccessor());
                // Symmetric to the ret==2 case below: if the dropped FALSE
                // successor terminates (return/throw) AND is a pure branch body
                // (no phi — a phi means it's a value-merge continuation, which
                // must stay after the if, not become the else), keep it as the
                // explicit else body instead of letting it be mis-placed.
                if(enc->BlockTerminates(block->GetFalseSuccessor()) &&
                   !enc->BlockHasPhi(block->GetFalseSuccessor()) &&
                   enc->TargetIsSingleBranchBody(block, block->GetFalseSuccessor())){
                    enc->specialblockid.insert(block->GetFalseSuccessor()->GetId());
                    false_statements = enc->GetBlockStatementById(block->GetFalseSuccessor());
                }
            }else{
                XABC_DBG << "#VisitIfImm ret case: " << ret << std::endl;
                enc->specialblockid.insert(block->GetFalseSuccessor()->GetId());
                false_statements =   enc->GetBlockStatementById(block->GetFalseSuccessor());
                // ret==2 normally drops the true successor (treated as post-if
                // continuation, placed later at its dominator). But if the true
                // successor TERMINATES (return/throw) it is the if's else body
                // (e.g. `if(a||b){return false}` second condition: both jump to
                // the same return block). Capture it as the explicit else so it
                // stays nested and is not dropped from the `a===true` path.
                // Skip if the block has a phi: that makes it a value-merge
                // continuation (e.g. `let s=x; if(c){s=y} return s`), which must
                // be emitted AFTER the if, not captured as the else.
                if(enc->BlockTerminates(block->GetTrueSuccessor()) &&
                   !enc->BlockHasPhi(block->GetTrueSuccessor()) &&
                   enc->TargetIsSingleBranchBody(block, block->GetTrueSuccessor())){
                    enc->specialblockid.insert(block->GetTrueSuccessor()->GetId());
                    true_statements = enc->GetBlockStatementById(block->GetTrueSuccessor());
                }
            }
        /////////////////////////////////////////////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////////////////////////////////////
        /// deal with while/do-while
       

        if(enc->backedge2dowhileloop.find(block) != enc->backedge2dowhileloop.end()){
            enc->loopbranches_.insert(inst);
            enc->loopbranchblocks_.insert(block);

            XABC_DBG << "[+] do-while =====" << std::endl;
            compiler::Loop* loop = block->GetLoop();

            auto back_edges = loop->GetBackEdges();
            LogBackEdgeId(back_edges);

            es2panda::ir::DoWhileStatement* dowhilestatement;
            test_expression =  enc->InverseTestExpression(enc, inst, ret, src_expression, false);

            XABC_DBG << "true_statements size: " << true_statements->AsBlockStatement()->Statements().size() << std::endl;

            auto dowhilebody = enc->CopyAndCreateNewBlockStatement(true_statements);
            if(block->GetTrueSuccessor() == loop->GetHeader()){
                XABC_DBG << "do while case 1" << std::endl;
                dowhilestatement = AllocNode<es2panda::ir::DoWhileStatement>(enc,
                    nullptr,
                    dowhilebody,
                    test_expression
                );
            }else{
                XABC_DBG << "do while case 2" << std::endl;
                dowhilestatement = AllocNode<es2panda::ir::DoWhileStatement>(enc,
                        nullptr,
                        dowhilebody,
                        test_expression
                        );
            }
            
            true_statements->AsBlockStatement()->statements_.clear(); 
            enc->AddInstAst2BlockStatemntByBlock(block->GetTrueSuccessor(), dowhilestatement);
            
            if(false_statements != nullptr){
                enc->AddInstAst2BlockStatemntByBlock(block->GetTrueSuccessor(), false_statements);
            }
            
            enc->AddInstAst2BlockStatemntByBlock(loop->GetPreHeader(), enc->GetBlockStatementById(block->GetTrueSuccessor())); 
            XABC_DBG << "[-] do-while =====" << std::endl;
        }else if(block->IsLoopValid() && !block->GetLoop()->IsRoot() && IsLoopConditionBranch(block)  &&  enc->loop2type[block->GetLoop()] == 0 &&
                (block->IsLoopHeader() || enc->loopbranchblocks_.find(block->GetLoop()->GetHeader()) == enc->loopbranchblocks_.end())
               ){
        // }else if(block->IsLoopValid()   &&  block->IsLoopHeader() && enc->loop2type[block->GetLoop()] == 0 
        //        ){
            enc->loopbranches_.insert(inst);
            enc->loopbranchblocks_.insert(block);

            XABC_DBG << "[+] while ===" << std::endl;
            compiler::Loop* loop = block->GetLoop();
            auto back_edges = loop->GetBackEdges();
            LogBackEdgeId(back_edges);

            es2panda::ir::WhileStatement* whilestatement;
            auto header = loop->GetHeader();
            //if( LoopContainBlock(loop, block->GetFalseSuccessor()) && false_statements != nullptr){
            if(LoopContainBlock(loop, block->GetFalseSuccessor()) ){
                XABC_DBG << "while case 1" << std::endl;
                if(!header->IsTryBegin() && enc->whileheader2redundant.find(header) != enc->whileheader2redundant.end() && enc->whileheader2redundant[header]->Statements().size() != 0 ){
                    // add redundant statement in while-header
                    enc->whilebody2redundant[block->GetFalseSuccessor()] = enc->whileheader2redundant[header];
                }

                std::swap(true_statements, false_statements);
                test_expression =  enc->InverseTestExpression(enc, inst, ret, src_expression, true);
                whilestatement = AllocNode<es2panda::ir::WhileStatement>(enc,
                        nullptr,
                        test_expression, 
                        true_statements
                        );        
            }else{
                XABC_DBG << "while case 2" << std::endl;
                if(!header->IsTryBegin() && enc->whileheader2redundant.find(header) != enc->whileheader2redundant.end() && enc->whileheader2redundant[header]->Statements().size() != 0){
                    // add redundant statement in while-header
                    enc->whilebody2redundant[block->GetTrueSuccessor()] = enc->whileheader2redundant[header];
                }
                test_expression = enc->InverseTestExpression(enc, inst, ret, src_expression,false);
                whilestatement = AllocNode<es2panda::ir::WhileStatement>(enc,
                        nullptr,
                        test_expression, 
                        true_statements
                        );
            }

            if(true_statements != nullptr){
                enc->inserted_statements.insert(true_statements);
            }

            // `continue`-shaped loop: register this while's EXACT body container so a
            // single-latch induction step (`i = i +/- N`, parked in
            // phiref2pendingredundant[latch]) can be appended to its TAIL in the final
            // post-RPO pass (when the body is fully built). `true_statements` is the
            // body container in both while case 1 (post-swap) and case 2. Doing the
            // append now would land the step at the body TOP (it is empty here) — an
            // off-by-one. Registering by exact object avoids any re-resolution
            // mismatch. No-op for normal loops (handled by RegisterLatchHoist's guards).
            enc->RegisterLatchHoist(loop, true_statements);

            enc->AddInstAst2BlockStatemntByInst(inst, whilestatement);
            enc->AddInstAst2BlockStatemntByBlock(loop->GetPreHeader(), enc->GetBlockStatementById(block));
            if(false_statements != nullptr){
                enc->AddInstAst2BlockStatemntByInst(inst, false_statements);
            }
            XABC_DBG << "[-] while ===" << std::endl;
        }else{
            XABC_DBG << "[+] if ===" << std::endl;
            es2panda::ir::IfStatement* ifStatement;

            if(ret == 2){
                XABC_DBG << "if case 1" << std::endl;
                std::swap(true_statements, false_statements);
                test_expression = enc->InverseTestExpression(enc, inst, ret, src_expression, true);
                ifStatement = AllocNode<es2panda::ir::IfStatement>(enc, test_expression, true_statements, false_statements);
            }else{
                if(inst->GetCc() == compiler::CC_EQ){
                    if(false_statements != nullptr){
                        XABC_DBG << "if case 2" << std::endl;
                        std::swap(true_statements, false_statements);
                        test_expression = enc->InverseTestExpression(enc, inst, ret, src_expression, true);
                    }else{
                        // Single-branch `if` with NO captured else — the
                        // `continue`-at-body-head shape (dropped successor is the loop
                        // latch). Swapping a null over the body would null the
                        // consequent and the emit-guard below (`true_statements !=
                        // nullptr`) would DROP the whole if -> empty `while(){}`. Keep
                        // the body as the consequent and invert the CC_EQ test in place
                        // (`!==` -> `===`).
                        XABC_DBG << "if case 2b" << std::endl;
                        test_expression = enc->InverseTestExpression(enc, inst, ret, src_expression, false);
                    }
                }else{
                    XABC_DBG << "if case 3" << std::endl;
                    test_expression = enc->InverseTestExpression(enc, inst, ret, src_expression, false);
                }
                ifStatement = AllocNode<es2panda::ir::IfStatement>(enc, test_expression, true_statements, false_statements);
            }

            if(true_statements != nullptr){
                 enc->inserted_statements.insert(true_statements);
            }

            if(false_statements != nullptr){
                enc->inserted_statements.insert(false_statements);
            }

            if(true_statements != nullptr){
                enc->AddInstAst2BlockStatemntByInst(inst, ifStatement);
                enc->block2ifstatement_[block] = ifStatement;

                // A single-branch if (ret 1/2) whose DROPPED successor is a
                // terminating CONTINUATION merge (reached from both branches,
                // e.g. `if(len===0){loop} return out`) must emit that
                // continuation AFTER the if, on all paths — otherwise the
                // not-taken path falls through to undefined. (When the dropped
                // successor is a single-branch body it was already captured as
                // the else above; when it is a value-merge phi the merge-hoist
                // handles it; this covers the no-phi multi-pred return merge.)
                BasicBlock* dropped = (ret == 2) ? block->GetTrueSuccessor()
                                     : (ret == 1) ? block->GetFalseSuccessor()
                                                  : nullptr;
                if(dropped != nullptr &&
                   enc->BlockTerminates(dropped) &&
                   !enc->BlockHasPhi(dropped) &&
                   !enc->TargetIsSingleBranchBody(block, dropped) &&
                   enc->specialblockid.find(dropped->GetId()) == enc->specialblockid.end()){
                    enc->specialblockid.insert(dropped->GetId());
                    auto* cont = enc->GetBlockStatementById(dropped);
                    enc->AddInstAst2BlockStatemntByInst(inst, cont);
                }
            }
            XABC_DBG << "[-] if ===" << std::endl;

        }
        /////////////////////////////////////////////////////////////////////////////////////////////////

        //true_statements->SetParent(block);
        //false_statements->SetParent(block);
    }else{
        return; // skip unhandled if-imm case
    }
    XABC_DBG << "[-] VisitIfImm  >>>>>>>>>>>>>>>>>" << std::endl;
}


void AstGen::VisitLoadString(GraphVisitor *v, Inst *inst_base)
{
    XABC_DBG << "[+] VisitLoadString  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen *>(v);
    auto inst = inst_base->CastToLoadString();

    std::string source_str = enc->ir_interface_->GetStringIdByOffset(inst->GetTypeId()); 
    panda::es2panda::util::StringView name_view = panda::es2panda::util::StringView(*new std::string(source_str));
    
    auto src_expression  = AllocNode<es2panda::ir::StringLiteral>(enc, name_view);

    enc->SetExpressionByRegister(inst, inst->GetDstReg(), src_expression);
   

    XABC_DBG << "[-] VisitLoadString  >>>>>>>>>>>>>>>>>" << std::endl;
}

void AstGen::VisitReturn(GraphVisitor *v, Inst *inst_base)
{
    XABC_DBG << "[+] VisitReturn  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen *>(v);
    auto inst = inst_base->CastToReturn();

    panda::es2panda::ir::Expression* return_expression = *enc->GetExpressionByAcc(inst);

    auto returnstatement = AllocNode<es2panda::ir::ReturnStatement>(enc, return_expression);
    enc->AddInstAst2BlockStatemntByInst(inst, returnstatement);

    XABC_DBG << "[-] VisitReturn  >>>>>>>>>>>>>>>>>" << std::endl;
}

void AstGen::VisitCastValueToAnyType(GraphVisitor *visitor, Inst *inst)
{
    XABC_DBG << "[+] VisitCastValueToAnyType  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen *>(visitor);

    auto cvat = inst->CastToCastValueToAnyType();
    auto input = cvat->GetInput(0).GetInst()->CastToConstant();

    es2panda::ir::Expression* source = nullptr;
    switch (cvat->GetAnyType()) {
        case compiler::AnyBaseType::ECMASCRIPT_NULL_TYPE:
            source = enc->constant_null;
            break;

        case compiler::AnyBaseType::ECMASCRIPT_UNDEFINED_TYPE: {
            source = enc->constant_undefined;
            break;
        }

        case compiler::AnyBaseType::ECMASCRIPT_INT_TYPE: {
            source = AllocNode<es2panda::ir::NumberLiteral>(enc, 
                                                                input->CastToConstant()->GetIntValue()
                                            );
            break;
        }

        case compiler::AnyBaseType::ECMASCRIPT_DOUBLE_TYPE: {
            source = AllocNode<es2panda::ir::NumberLiteral>(enc, 
                                                                input->CastToConstant()->GetDoubleValue()
                                                        );
            break;
        }

        case compiler::AnyBaseType::ECMASCRIPT_BOOLEAN_TYPE: {
            uint64_t val = input->GetInt64Value();
            if (val != 0) {
                source = enc->constant_true;
            } else {
                source = enc->constant_false;
            }
            break;
        }

        case compiler::AnyBaseType::ECMASCRIPT_STRING_TYPE: {
            auto ls = cvat->GetInput(0).GetInst()->CastToLoadString();
            source = enc->id2expression[ls->GetId()];           
            break;
        }

        default:
           // UNREACHABLE();
            LOG(ERROR, BYTECODE_OPTIMIZER) << "VisitConstant with unknown type" ;
            enc->success_ = false;
    }

    enc->SetExpressionByRegister(inst, cvat->GetDstReg(), source);

    XABC_DBG << "[-] VisitCastValueToAnyType  >>>>>>>>>>>>>>>>>" << std::endl;
}


void AstGen::VisitIntrinsic(GraphVisitor *visitor, Inst *inst_base)
{
    XABC_DBG << "[+] VisitIntrinsic  >>>>>>>>>>>>>>>>>" << std::endl;
    ASSERT(inst_base->IsIntrinsic());
    VisitEcma(visitor, inst_base);
    XABC_DBG << "[-] VisitIntrinsic  >>>>>>>>>>>>>>>>>" << std::endl;
}

void AstGen::VisitCatchPhi(GraphVisitor *visitor, Inst *inst)
{
    XABC_DBG << "[+] VisitCatchPhi  >>>>>>>>>>>>>>>>>" << std::endl;
    // The Acc register stores the exception object.
    // Create an STA instruction if the exception is used later in virtual registers.
    
    if (inst->CastToCatchPhi()->IsAcc()) {
        XABC_DBG << "cast to catchphi" << std::endl;
        auto enc = static_cast<AstGen *>(visitor);
        enc->SetExpressionByRegister(inst, inst->GetDstReg(), enc->constant_catcherror);
        bool hasRealUsers = false;
        for (auto &user : inst->GetUsers()) {
            if (!user.GetInst()->IsSaveState()) {
                hasRealUsers = true;
                break;
            }
        }
        if (hasRealUsers) {
            enc->SetExpressionByRegister(inst, inst->GetDstReg(), enc->constant_catcherror);
        }
    }
    XABC_DBG << "[-] VisitCatchPhi  >>>>>>>>>>>>>>>>>" << std::endl;
}

#include "astgen_auxiins.cpp"
}  // namespace panda::bytecodeopt
