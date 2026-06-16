#include "classconstruction.h"


std::string ExtractPrefix(const std::string& input){
    size_t firstPos = input.find('#'); 
    if (firstPos != std::string::npos) {
        size_t secondPos = input.find('#', firstPos + 1); 
        if (secondPos != std::string::npos) {
            return input.substr(firstPos + 1, secondPos - firstPos - 1);
        }
    }
    return ""; 
}

bool IsInstanceMethod(std::string func_name){
    auto prefix = ExtractPrefix(func_name);
    return prefix.find('~') != std::string::npos && prefix.find('>') != std::string::npos;
}

// An anonymous build/callback closure (ArkUI build() lambdas, .then/animation
// callbacks) has a lambda marker `@<digit>` in its name prefix
// (e.g. "#~@0>@2*#", "#~@0>@1*^1*#"). These look like instance methods to
// IsInstanceMethod (prefix has ~ and >) but are NOT declared class members, so
// they aren't in `memberfuncs` and were wrongly early-skipped in
// DecompileFunction — their bodies never got decompiled (the func_8.. = undefined
// problem). Detect them so they're still decompiled and emitted.
bool IsAnonymousClosure(std::string func_name){
    auto prefix = ExtractPrefix(func_name);
    size_t at = prefix.find('@');
    while (at != std::string::npos) {
        // a '@' immediately followed by a digit is the lambda-index marker
        if (at + 1 < prefix.size() && std::isdigit(static_cast<unsigned char>(prefix[at + 1]))) {
            // distinguish the class marker `@0>` (always present) from extra
            // `@N*` lambda markers: a closure has a '@<digit>' that is followed
            // later by '*' (the lambda suffix) rather than '>'.
            size_t star = prefix.find('*', at);
            size_t gt = prefix.find('>', at);
            if (star != std::string::npos && (gt == std::string::npos || star < gt)) {
                return true;
            }
        }
        at = prefix.find('@', at + 1);
    }
    return false;
}

bool ConstructClasses(std::map<uint32_t, std::set<uint32_t>> &class2memberfuns, panda::es2panda::parser::Program *parser_program,  BytecodeOptIrInterface *ir_interface,
        std::map<uint32_t, panda::es2panda::ir::Expression*> &class2father, std::map<uint32_t, panda::es2panda::ir::ScriptFunction *> &method2scriptfunast,
        std::map<uint32_t, panda::es2panda::ir::ClassDeclaration *> &ctor2classdeclast, std::map<std::string, std::string>& raw2newname
        ){
    for(const auto & pair : class2memberfuns){
        auto constructor_offset = pair.first;

        std::cout << constructor_offset << std::endl;
        auto member_funcs = pair.second;
        //std::cout << "constructor_offset: " << constructor_offset << std::endl;
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        auto methodid = ir_interface->GetMethodIdByOffset(constructor_offset);
        auto constructor_offset_name = RemoveArgumentsOfFunc(methodid);
        std::string newname_constructor_offset_name;
        
        if(raw2newname.find(constructor_offset_name) != raw2newname.end()){
            newname_constructor_offset_name =  raw2newname[constructor_offset_name];
        }else{
            HandleError("#ConstructClasses: find constructor_offset_name newname error");
        }
        panda::es2panda::util::StringView name_view1 = panda::es2panda::util::StringView(*(new std::string(RemoveArgumentsOfFunc(newname_constructor_offset_name))));

        auto identNode = AllocNode<panda::es2panda::ir::Identifier>(parser_program, name_view1);


        ArenaVector<es2panda::ir::TSClassImplements *> implements(parser_program->Allocator()->Adapter());
        ArenaVector<es2panda::ir::TSIndexSignature *> indexSignatures(parser_program->Allocator()->Adapter());
        ArenaVector<es2panda::ir::Statement *> properties(parser_program->Allocator()->Adapter());
        
        //////////////////////////////////////////////////////////////////////////////////////////////
        ArenaVector<es2panda::ir::Decorator *> decorators(parser_program->Allocator()->Adapter());
        ArenaVector<es2panda::ir::Annotation *> annotations(parser_program->Allocator()->Adapter());
        ArenaVector<es2panda::ir::ParamDecorators> paramDecorators(parser_program->Allocator()->Adapter());

        panda::es2panda::util::StringView name_view2 = panda::es2panda::util::StringView("constructor");
        auto keyNode = AllocNode<panda::es2panda::ir::Identifier>(parser_program, name_view2);

        auto func = method2scriptfunast[constructor_offset];
        method2scriptfunast.erase(constructor_offset);

        if(func == nullptr){
            // The class constructor itself was skipped/unanalysable — we can't
            // build a class without it, so skip the whole class rather than abort.
            std::cout << "skip class with no constructor AST, offset: " << constructor_offset << std::endl;
            continue;
        }

        auto funcExpr = AllocNode<panda::es2panda::ir::FunctionExpression>(parser_program, func);


        auto ctor = AllocNode<es2panda::ir::MethodDefinition>(parser_program, es2panda::ir::MethodDefinitionKind::CONSTRUCTOR, keyNode, funcExpr,
                                                        es2panda::ir::ModifierFlags::PUBLIC, parser_program->Allocator(), 
                                                    std::move(decorators), std::move(annotations), 
                                                    std::move(paramDecorators), false);
        
        ///////////////////////////////////////////////////////////////////////////////////////////////

        auto father = class2father[constructor_offset];                          
        auto classDefinition = AllocNode<es2panda::ir::ClassDefinition>(parser_program, nullptr, identNode, nullptr, nullptr,
                                                                       std::move(implements), ctor, nullptr,
                                                                       nullptr, father, std::move(properties), 
                                                                       std::move(indexSignatures), false, false);
        
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        for (const auto& member_func_offset : member_funcs) {
            if(constructor_offset == member_func_offset){
                continue;
            }

            auto func = method2scriptfunast[member_func_offset];
            method2scriptfunast.erase(member_func_offset);

            if(func == nullptr){
                // This member function has no AST because it was skipped during
                // decompilation (unanalysable — e.g. IR-build failure, irreducible
                // loop, or one of the skipfailfuns from the dep-scan pass). The
                // class still lists it as a member. Rather than aborting the whole
                // file, just omit this one method and keep building the class.
                std::cout << "skip member function with no AST, offset: " << member_func_offset << std::endl;
                continue;
            }

            auto funcExpr = AllocNode<es2panda::ir::FunctionExpression>(parser_program, func);


            auto raw_member_name  = RemoveArgumentsOfFunc(ir_interface->GetMethodIdByOffset(member_func_offset));
            std::string new_member_name;
            if(raw2newname.find(raw_member_name) != raw2newname.end()){
                new_member_name =  raw2newname[raw_member_name];
            }else{
                // No renamed identifier for this member — skip it rather than abort.
                continue;
            }

            panda::es2panda::util::StringView name_view3 = panda::es2panda::util::StringView(*(new std::string(new_member_name)));
            auto keyNode =  AllocNode<es2panda::ir::Identifier>(parser_program, name_view3);

            ArenaVector<es2panda::ir::Decorator *> decorators(parser_program->Allocator()->Adapter());
            ArenaVector<es2panda::ir::Annotation *> annotations(parser_program->Allocator()->Adapter());
            ArenaVector<es2panda::ir::ParamDecorators> paramDecorators(parser_program->Allocator()->Adapter());

            

            auto method = AllocNode<es2panda::ir::MethodDefinition>(parser_program, es2panda::ir::MethodDefinitionKind::METHOD, keyNode, funcExpr,
                                                            es2panda::ir::ModifierFlags::PUBLIC, parser_program->Allocator(), 
                                                        std::move(decorators), std::move(annotations), 
                                                        std::move(paramDecorators), false);                                              
            classDefinition->AddToBody(method);

        }
  
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        ArenaVector<es2panda::ir::Decorator *> decorators1(parser_program->Allocator()->Adapter());
        ArenaVector<es2panda::ir::Annotation *> annotations1(parser_program->Allocator()->Adapter());

       
        auto classDecl =  AllocNode<es2panda::ir::ClassDeclaration>(parser_program, classDefinition, 
                                                std::move(decorators1), std::move(annotations1), false);
        
        ctor2classdeclast[constructor_offset] = classDecl;
    }
    return true;
}
