#include "lexicalenv.h"
#include <algorithm>
#include <utility>



LexicalEnv::LexicalEnv(size_t capacity) 
    : expressions_(capacity, nullptr), capacity_(capacity), full_size_(0) {
}

void LexicalEnv::AddIndexes(size_t index){
    indexes_.insert(index);
}

void LexicalEnv::LogIndexes() {
    std::cout << "[*] index >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@" << std::endl;
    for (const auto& value : indexes_) {
        std::cout << value << " ";
    }
    std::cout << std::endl;
}


bool LexicalEnv::IsFull() const {
    for (size_t i = 0; i < capacity_; ++i) {
        if(expressions_[i] == nullptr){
            //HandleError("#LexicalEnv::IsFull : " + std::to_string(i));
            return false;
        }
    }
    return true;
}

LexicalEnv::LexicalEnv(const LexicalEnv& other) 
    : expressions_(other.capacity_), capacity_(other.capacity_), full_size_(other.full_size_){
    
    for (size_t i = 0; i < capacity_; ++i) {
        if(other.expressions_[i] == nullptr){
        }else{
            expressions_[i] = new std::string(*(other.expressions_[i]));
        }
    }

    indexes_.insert(other.indexes_.begin(), other.indexes_.end());
}

void LexicalEnv::GrowToFit(size_t index) {
    // The env capacity comes from the NEWLEXENV lexenv_size, but per-BB env copies
    // and captured outer-scope slots can address an index beyond it. Rather than
    // aborting the whole decompile (old CheckIndex), grow the slot vector to fit —
    // consistent with the tolerant LexicalEnvStack::GetLexicalEnv auto-grow.
    if (index >= capacity_) {
        capacity_ = index + 1;
        expressions_.resize(capacity_, nullptr);
    }
}

std::string*& LexicalEnv::operator[](size_t index) {
    GrowToFit(index);
    return expressions_[index];
}

const std::string* LexicalEnv::operator[](size_t index) const {
    if (index >= capacity_) {
        return nullptr;  // const path can't grow; treat as an unpopulated slot
    }
    return expressions_[index];
}

std::string* LexicalEnv::Get(size_t index) const {
    if (index >= capacity_) {
        return nullptr;  // unpopulated/out-of-range captured slot
    }
    return expressions_[index];
}

void LexicalEnv::Set(size_t index, std::string* expr) {
    GrowToFit(index);
    if(expr == nullptr){
        return;  // nothing to store; skip
    }
    expressions_[index] = expr;
    AddIndexes(index); /// support callruntime.createprivateproperty
}

size_t LexicalEnv::Size() const {
    size_t cout = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        if(expressions_[i] != nullptr){
            full_size_ = i;
            cout++;
        }
    }
    return cout;
}

bool LexicalEnv::IsValidIndex(size_t index) const {
    return index < capacity_;
}

void LexicalEnv::CheckIndex(size_t index) const {
    if (index >= capacity_) {
        return;  // out of range tolerated; accessors guard/auto-grow
    }
}

LexicalEnvStack::LexicalEnvStack() {
}

LexicalEnvStack::LexicalEnvStack(const LexicalEnvStack& other) 
    : stack_(other.stack_) {
}

bool LexicalEnvStack::IsFull() const {
    for(auto const &lexicalenv : stack_){
        if(!lexicalenv.IsFull()){
            return false;
        }
    }
    return true;
}

LexicalEnvStack::~LexicalEnvStack() {
}

LexicalEnv* LexicalEnvStack::Push(size_t capacity) {
    stack_.emplace_back(capacity);
    return &stack_.back();
}

void LexicalEnvStack::Pop() {
    if (stack_.empty()) {
        return;
        //HandleError("#LexicalEnvStack::Pop: Cannot pop from empty stack");
        // In some cases, the path lexical stack is empty, but still performs a pop operation.
        // for (const i of [0, 1]) {
        //     if (i === 0) {
        //         continue;
        //     }

        //     (() => {
        //         i;
        //     })();
        // }

    
    }
    stack_.pop_back();
}

size_t LexicalEnvStack::Size() const {
    return stack_.size();
}

bool LexicalEnvStack::Empty() const {
    return stack_.empty();
}

std::string* LexicalEnvStack::Get(size_t A, size_t B) const {
    // Bounds-safe: return nullptr on out-of-range (callers handle null by
    // synthesising a name) instead of aborting via CheckIndex.
    if (stack_.empty() || A >= stack_.size()) {
        return nullptr;
    }
    size_t actualIndex = stack_.size() - 1 - A;
    return stack_[actualIndex].Get(B);
}

bool LexicalEnvStack::IsSetSafe(size_t A, size_t B) {
    if (stack_.empty()) {
        return false;
    }
    
    if (A >= stack_.size()) {
        return false;
    }

    size_t actualIndex = stack_.size() - 1 - A;
    if (!stack_[actualIndex].IsValidIndex(B)) {
       return false;
    }
    return true;
}

void LexicalEnvStack::Set(size_t A, size_t B, std::string* expr) {
    // Bounds-safe: grow the stack if the slot is beyond current depth (captured
    // outer-scope env), so we never index past the end (CheckIndex no longer
    // aborts, so the deref must be guarded here).
    while (stack_.size() <= A) {
        stack_.emplace(stack_.begin());
    }
    size_t actualIndex = stack_.size() - 1 - A;
    stack_[actualIndex].Set(B, expr);
}

void LexicalEnvStack::SetIndexes(size_t A, std::set<size_t> indexes) {
    if (stack_.empty() || A >= stack_.size()) {
        return;
    }
    
    size_t actualIndex = stack_.size() - 1 - A;
    stack_[actualIndex].indexes_.insert(indexes.begin(), indexes.end());
}


LexicalEnv& LexicalEnvStack::GetLexicalEnv(size_t A) {
    // A LDLEXVAR/STLEXVAR can reference an environment from an enclosing function
    // scope that exists at runtime but was never materialised in this method's
    // per-BB stack model (the parent's NEWLEXENV lives in another method). Rather
    // than aborting the whole decompile (the old CheckStackIndex behaviour), grow
    // the stack with placeholder envs so the access resolves to a captured/closure
    // variable. This matches the already-tolerant Pop()/SetIndexes()/IsSetSafe().
    while (stack_.size() <= A) {
        stack_.emplace(stack_.begin());  // default capacity (256)
    }

    size_t actualIndex = stack_.size() - 1 - A;
    return stack_[actualIndex];
}

LexicalEnv& LexicalEnvStack::Top() {
    if (stack_.empty()) {
        // Empty stack — push a placeholder env so we return a valid reference
        // instead of aborting (consistent with GrowToFit tolerance).
        stack_.emplace_back();
    }
    return stack_.back();
}

void LexicalEnvStack::Clear() {
    stack_.clear();
}

void LexicalEnvStack::CheckIndex(size_t A, size_t B) const {
    CheckStackIndex(A);
    
    size_t actualIndex = stack_.size() - 1 - A;
    if (!stack_[actualIndex].IsValidIndex(B)) {
        return;  // tolerated; accessors handle out-of-range
    }
}

void LexicalEnvStack::CheckStackIndex(size_t A) const {
    if (stack_.empty()) {
        return;  // tolerated; accessors bounds-check
    }
    
    if (A >= stack_.size()) {
        return;  // tolerated; accessors bounds-check
    }
}


void DealWithGlobalLexicalWaitlist(uint32_t tier, uint32_t index, std::string closure_name, std::vector<LexicalEnvStack*> *globallexical_waitlist){
    for (auto it = globallexical_waitlist->begin(); it != globallexical_waitlist->end(); ) {
        auto* waitelement = *it;

        std::cout << "DealWithGlobalLexicalWaitlist: tier: " << tier << " , index: " << index << std::endl; 
        
        if(waitelement->IsSetSafe(tier, index)){
            waitelement->Set(tier, index, new std::string(closure_name));
        }
       
        if(waitelement->IsFull()){
            it = globallexical_waitlist->erase(it);
        }else{
            ++it;
        }
    }
}

void MergeMethod2LexicalMap(Inst* inst, std::map<panda::compiler::BasicBlock*, LexicalEnvStack*> bb2lexicalenvstack,
                  uint32_t source_methodoffset, uint32_t target_methodoffset, std::map<uint32_t, std::map<uint32_t,  std::set<size_t>>>* method2lexicalmap) {
    // merge instance_initializer lexical to current 
    auto& tmpmethod2lexicalmap = *(method2lexicalmap);

    auto source_lexicalmap = tmpmethod2lexicalmap.find(source_methodoffset);
    if (source_lexicalmap == tmpmethod2lexicalmap.end()) {
        return;
    }
    auto lexicalenvstack = bb2lexicalenvstack[inst->GetBasicBlock()];
    std::cout << "lexicalenvstack size: " << lexicalenvstack->Size() << std::endl;
    for (const auto& [tier, source_indexes] : source_lexicalmap->second) {
        // std::cout << "source_indexes: ";
        // for(const auto&v : source_indexes){
        //     std::cout << " , " << v;
        // }
        // std::cout << std::endl;
        std::cout << "tier: " << tier << std::endl;
        lexicalenvstack->SetIndexes(tier, source_indexes);
    }
}

void PrintInnerMethod2LexicalMap(std::map<uint32_t, std::map<uint32_t,  std::set<size_t>>>* method2lexicalmap, uint32_t methodoffset){
    auto outerIt = method2lexicalmap->find(methodoffset);
    if (outerIt == method2lexicalmap->end()) {
        std::cerr << "Method offset not found in the map." << std::endl;
        return;
    }

    const std::map<uint32_t, std::set<size_t>>& innerMap = outerIt->second;

    for (const auto& pair : innerMap) {
        uint32_t key = pair.first;
        const std::set<size_t>& vec = pair.second;

        std::cout << "Key: " << key << " Values: ";
        for (const auto& value : vec) {
            std::cout << value << " ";
        }
        std::cout << std::endl;
    }
}


uint32_t SearchStartposForCreatePrivateproperty(Inst *inst, std::map<panda::compiler::BasicBlock*, LexicalEnvStack*> bb2lexicalenvstack,
        std::map<uint32_t, std::map<uint32_t,  std::set<size_t>>>* method2lexicalmap, uint32_t methodoffset){
    
    auto lexicalenvstack = bb2lexicalenvstack[inst->GetBasicBlock()];
    auto &toplexicalenv = lexicalenvstack->Top();
    std::set<size_t> vec(toplexicalenv.indexes_);

    std::vector<size_t> sorted(vec.begin(), vec.end());
    std::sort(sorted.begin(), sorted.end());

    std::cout << "lexicalenvstack size: " << lexicalenvstack->Size() << std::endl;
    std::cout << "sorted size: " << sorted.size() << std::endl;
    
    for(const auto & i: vec){
        std::cout << "X - #: " << i << std::endl;
    }

    std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>" << std::endl;

    ////////////////////////////////////////////////////////////
    if(lexicalenvstack->Size() > 2){
        auto aa = lexicalenvstack->stack_[lexicalenvstack->Size() - 2];
        for(const auto & i: aa.indexes_){
            std::cout << "Y - #: " << i << std::endl;
        }
    }
    ////////////////////////////////////////////////////////////

    for (size_t i = 0; i < sorted.size(); ++i) {
        std::cout << "i: " << i << " , sorted[i]: " <<  sorted[i] << std::endl;
        if (i != sorted[i]) {
            return i;
        }
    } 

    // No gap found — the next free slot is at the end (don't abort).
    return sorted.size();
}


void CopyLexicalenvStack(uint32_t methodoffset_, Inst* inst, std::map<uint32_t, LexicalEnvStack*>* method2lexicalenvstack, 
        std::map<panda::compiler::BasicBlock*, LexicalEnvStack*> bb2lexicalenvstack, std::vector<LexicalEnvStack*> *globallexical_waitlist){
            
    if(bb2lexicalenvstack[inst->GetBasicBlock()]->Empty()){
        return;
        //HandleError("#CopyLexicalenvStack: source bb2lexicalenvstack is empty");
    }
    auto wait_method = new LexicalEnvStack(*(bb2lexicalenvstack[inst->GetBasicBlock()]));
    (*method2lexicalenvstack)[methodoffset_] = wait_method;
    globallexical_waitlist->push_back(wait_method);
}