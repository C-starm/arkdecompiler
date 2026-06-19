void AstGen::VisitPhi(GraphVisitor* v, Inst* inst_base) {
    std::cout << "[+] VisitPhi  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen*>(v);
    auto inst = inst_base->CastToPhi();
    ArenaVector<es2panda::ir::Expression *> arguments(enc->parser_program_->Allocator()->Adapter());

    // Short-circuit boolean (a||b / a&&b): if this phi merges a condition
    // block's own tested value with the value of the other (lazily-evaluated)
    // operand, rebuild the logical expression instead of emitting two clobbering
    // `dst = source` assignments (the unconditional second of which dropped the
    // first operand, e.g. `a || b` decompiled to just `b`).
    {
        BasicBlock* condbb = nullptr;
        size_t cond_idx = 0;
        bool merge_on_truthy = false;
        if(enc->DetectShortCircuitPhi(inst, &condbb, &cond_idx, &merge_on_truthy) &&
           enc->shortcircuit_condblocks_.find(condbb) != enc->shortcircuit_condblocks_.end()){
            size_t other_idx = 1 - cond_idx;
            auto cond_expr  = *enc->GetExpressionByRegIndex(inst, cond_idx);
            auto other_expr = *enc->GetExpressionByRegIndex(inst, other_idx);
            // merge taken when cond is truthy  => short-circuit OR  (cond || other)
            // merge taken when cond is falsy   => short-circuit AND (cond && other)
            auto op = merge_on_truthy ? es2panda::lexer::TokenType::PUNCTUATOR_LOGICAL_OR
                                      : es2panda::lexer::TokenType::PUNCTUATOR_LOGICAL_AND;
            auto logical = AllocNode<es2panda::ir::BinaryExpression>(enc, cond_expr, other_expr, op);
            // Bind the phi's value to the logical expression so its single user
            // (return / further use) inlines it. Materialise only if multi-user.
            enc->HandleNewCreatedExpression(inst, logical);
            std::cout << "[short-circuit] phi " << inst->GetId()
                      << (merge_on_truthy ? " => ||" : " => &&") << std::endl;
            std::cout << "[-] VisitPhi  <<<<<<<<<<<<<<<" << std::endl;
            return;
        }
    }

    // Value-select ternary (cond ? X : Y): a 2-input phi whose default edge Y is
    // reached only via if-FALSE paths and whose value edge X is the deep result.
    // Rebuild it as a ConditionalExpression bound to the phi, so the default is
    // never emitted as an unconditional `dst = Y` that clobbers the X branch
    // (e.g. `A && B ? s.longToken : ''` decompiling to always `''`). Pure
    // expression (like the short-circuit case) — no statement-placement conflict.
    {
        auto* ternary = enc->TryBuildValueSelectTernary(inst);
        if(ternary != nullptr){
            enc->HandleNewCreatedExpression(inst, ternary);
            std::cout << "[value-ternary] phi " << inst->GetId() << " => cond ? X : Y" << std::endl;
            std::cout << "[-] VisitPhi  <<<<<<<<<<<<<<<" << std::endl;
            return;
        }
    }

    auto dst_reg_identifier = enc->GetIdentifierByReg(inst->GetId());
    enc->SetExpressionByRegister(inst, inst->GetDstReg(), dst_reg_identifier);

    for (size_t i = 0; i < inst->GetInputsCount(); i++) {
        auto bb = inst->GetPhiInputBb(i);
        auto sourceexpression = *enc->GetExpressionByRegIndex(inst, i);

        // Loop induction step: if this back-edge value is `phi +/- 1` (an inc/dec
        // of the phi itself, modulo tonumeric), render it as `i = i + 1` directly.
        // The default `i = <backedge tmp>` is wrong here: the bytecode routes the
        // step through a separate register that is undefined on the first
        // iteration and can collide with an unrelated reused register (e.g. a
        // materialized array sharing the physical reg), corrupting both.
        es2panda::lexer::TokenType step_op;
        if(enc->DetectInductionStep(inst, inst->GetInput(i).GetInst(), &step_op)){
            sourceexpression = AllocNode<es2panda::ir::BinaryExpression>(
                enc, dst_reg_identifier, enc->constant_one, step_op);
        }

        auto assignexpression = AllocNode<es2panda::ir::AssignmentExpression>(enc,
                                                                            dst_reg_identifier,
                                                                            sourceexpression,
                                                                            es2panda::lexer::TokenType::PUNCTUATOR_SUBSTITUTION
                                                                        );
        auto assignstatement = AllocNode<es2panda::ir::ExpressionStatement>(enc, assignexpression);

        if(std::find(enc->visited.begin(), enc->visited.end(), bb) != enc->visited.end()){
            // If this phi edge comes from a block that ends in an IfImm, it is the
            // condition block's DEFAULT (fall-through) value — the other phi
            // edge(s) are the if's branch bodies. Source idiom:
            //   let dst = <default>; if (cond) { dst = <branch> }
            // Emitting `dst = default` at the END of the condition block places it
            // AFTER the `if`, unconditionally clobbering the branch assignment
            // (the dead-store bug, e.g. `if(s.endsWith('/')){s=s.slice(0,-1)} s=s;`
            // — the trim is lost). Instead insert it BEFORE the if so it acts as
            // the default that the branch conditionally overrides.
            Inst* last = bb->GetLastInst();
            auto if_it = enc->block2ifstatement_.find(bb);
            if(last != nullptr && last->GetOpcode() == Opcode::IfImm &&
               if_it != enc->block2ifstatement_.end()){
                // This phi edge comes from a condition block — it is the DEFAULT
                // (fall-through) value the if's branch conditionally overrides.
                // Source idiom: `let dst = <default>; if (cond) { dst = <branch> }`.
                // Insert it right BEFORE THIS block's matching IfStatement, so it
                // is not appended AFTER the if where it would unconditionally
                // clobber the branch assignment (the dead-store bug).
                auto* blkstmt = enc->GetBlockStatementById(bb);
                const auto& stmts = blkstmt->Statements();
                size_t pos = stmts.size();
                for(size_t k = 0; k < stmts.size(); ++k){
                    if(stmts[k] == if_it->second){
                        pos = k;
                        break;
                    }
                }
                if(enc->inserted_statements.find(assignstatement) == enc->inserted_statements.end()){
                    enc->inserted_statements.insert(assignstatement);
                    blkstmt->AddStatementAtPos(pos, assignstatement);
                }
            }else{
                enc->AddInstAst2BlockStatemntByBlock(bb, assignstatement);
            }
        }else{
            if(enc->phiref2pendingredundant.find(bb) != enc->phiref2pendingredundant.end()){
                auto found_block_statement = enc->phiref2pendingredundant[bb];
                const auto &statements = found_block_statement->Statements();
                found_block_statement->AddStatementAtPos(statements.size(), assignstatement);
            }else{
                ArenaVector<panda::es2panda::ir::Statement *> statements(enc->parser_program_->Allocator()->Adapter());
                auto new_block_statement = AllocNode<es2panda::ir::BlockStatement>(enc, nullptr, std::move(statements));
                new_block_statement->AddStatementAtPos(statements.size(), assignstatement);
                enc->phiref2pendingredundant[bb] = new_block_statement;
            }
        }
    }

    std::cout << "[-] VisitPhi  <<<<<<<<<<<<<<<" << std::endl;
}

void AstGen::VisitSaveState(GraphVisitor* v, Inst* inst_base) {
    std::cout << "[+] VisitSaveState  >>>>>>>>>>>>>>>>>" << std::endl;
    std::cout << "[-] VisitSaveState  >>>>>>>>>>>>>>>>>" << std::endl;
}
void AstGen::VisitParameter(GraphVisitor* v, Inst* inst_base) {
    std::cout << "[+] VisitParameter  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen *>(v);
    auto inst = inst_base->CastToParameter();

    panda::es2panda::ir::Expression* arg = enc->getParameterName(inst->GetArgNumber());    
    enc->SetExpressionByRegister(inst, inst->GetDstReg(), arg);
    std::cout << "[-] VisitParameter  >>>>>>>>>>>>>>>>>" << std::endl;
}

void AstGen::VisitTry(GraphVisitor* v, Inst* inst_base) {
    std::cout << "[+] VisitTry  >>>>>>>>>>>>>>>>>" << std::endl;
    auto enc = static_cast<AstGen*>(v);
    auto inst = inst_base->CastToTry();

    // find tryblock
    BasicBlock* tryblock = nullptr;
    if(inst->GetBasicBlock()->GetSuccessor(0)->IsCatchBegin()){
        tryblock = inst->GetBasicBlock()->GetSuccessor(1);
    }else if(inst->GetBasicBlock()->GetSuccessor(1)->IsCatchBegin()){
        tryblock = inst->GetBasicBlock()->GetSuccessor(0);
    }else{
        tryblock = inst->GetBasicBlock()->GetSuccessor(0); // fallback, don't abort
    }

    enc->specialblockid.insert(tryblock->GetId());
    
    panda::es2panda::ir::BlockStatement* tryblock_statement = enc->GetBlockStatementById(tryblock);

    if(inst->GetBasicBlock()->GetTryId() !=  panda::compiler::INVALID_ID){
        enc->tyrid2block[inst->GetBasicBlock()->GetTryId()] = tryblock_statement;
    }
    

    /// find case block
    auto type_ids = inst->GetCatchTypeIds();
    auto catch_indexes = inst->GetCatchEdgeIndexes();

    panda::es2panda::ir::CatchClause *catchClause = nullptr;
    for (size_t idx = 0; idx < type_ids->size(); idx++) {
        auto succ =  inst->GetBasicBlock()->GetSuccessor(catch_indexes->at(idx));
        
        while (!succ->IsCatchBegin()) {
            succ = succ->GetSuccessor(0);
        }

        enc->specialblockid.insert(succ->GetId());
        auto catch_block = enc->GetBlockStatementById(succ);
   
        panda::es2panda::ir::Expression *param = enc->constant_catcherror;
        

        catchClause =  AllocNode<panda::es2panda::ir::CatchClause>(enc, nullptr, param, catch_block);
        enc->tyrid2catchclause[inst->GetBasicBlock()->GetTryId()] = catchClause;
    }

    
    // if(inst->GetBasicBlock()->GetPredsBlocks().size() > 2){
    //     HandleError("analysis try-catch error for more than one predecessor");
    // }
    
    // create null finally case
    ArenaVector<panda::es2panda::ir::Statement *> finally_statements(enc->parser_program_->Allocator()->Adapter());
    auto finnalyClause = AllocNode<es2panda::ir::BlockStatement>(enc, nullptr, std::move(finally_statements));
    
    // create try-catch statement
    enc->GetBlockStatementById(inst->GetBasicBlock());

    auto tryStatement = AllocNode<panda::es2panda::ir::TryStatement>(enc, tryblock_statement, catchClause, finnalyClause);
    enc->tyridtrystatement[inst->GetBasicBlock()->GetTryId()] = tryStatement;
    
    enc->AddInstAst2BlockStatemntByInst(inst_base, tryStatement);

    std::cout << "[-] VisitTry  >>>>>>>>>>>>>>>>>" << std::endl;

}
