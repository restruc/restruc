#include "recontex.hxx"

#include "dumper.hxx"
#include "scope_guard.hxx"
#include "utils/adapters.hxx"
#include "utils/hash.hxx"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>

using namespace rstc;

//#define DEBUG_ANALYSIS_PROGRESS
//#define DEBUG_OPTIMAL_COVERAGE
//#define DEBUG_CONTEXT_PROPAGATION
//#define DEBUG_CONTEXT_PROPAGATION_VALUES

ZydisRegister const Recontex::volatile_registers_[] = {
    ZYDIS_REGISTER_RAX,  ZYDIS_REGISTER_RCX,  ZYDIS_REGISTER_RDX,
    ZYDIS_REGISTER_R8,   ZYDIS_REGISTER_R9,   ZYDIS_REGISTER_R10,
    ZYDIS_REGISTER_R11,  ZYDIS_REGISTER_ZMM0, ZYDIS_REGISTER_ZMM1,
    ZYDIS_REGISTER_ZMM2, ZYDIS_REGISTER_ZMM3, ZYDIS_REGISTER_ZMM4,
    ZYDIS_REGISTER_ZMM5,
};

ZydisRegister const Recontex::nonvolatile_registers_[] = {
    ZYDIS_REGISTER_RBX,   ZYDIS_REGISTER_RBP,   ZYDIS_REGISTER_RSP,
    ZYDIS_REGISTER_RDI,   ZYDIS_REGISTER_RSI,   ZYDIS_REGISTER_R12,
    ZYDIS_REGISTER_R13,   ZYDIS_REGISTER_R14,   ZYDIS_REGISTER_R15,
    ZYDIS_REGISTER_ZMM6,  ZYDIS_REGISTER_ZMM7,  ZYDIS_REGISTER_ZMM8,
    ZYDIS_REGISTER_ZMM9,  ZYDIS_REGISTER_ZMM10, ZYDIS_REGISTER_ZMM11,
    ZYDIS_REGISTER_ZMM12, ZYDIS_REGISTER_ZMM13, ZYDIS_REGISTER_ZMM14,
    ZYDIS_REGISTER_ZMM15,
};

std::unordered_map<ZydisMnemonic, Recontex::EmulationCallbackAction>
    Recontex::emulation_callback_actions_{
        { ZYDIS_MNEMONIC_ADD,
          [](uintptr_t dst, uintptr_t src) { return dst + src; } },
        { ZYDIS_MNEMONIC_SUB,
          [](uintptr_t dst, uintptr_t src) { return dst - src; } },
        { ZYDIS_MNEMONIC_OR,
          [](uintptr_t dst, uintptr_t src) { return dst | src; } },
        { ZYDIS_MNEMONIC_AND,
          [](uintptr_t dst, uintptr_t src) { return dst & src; } },
        { ZYDIS_MNEMONIC_XOR,
          [](uintptr_t dst, uintptr_t src) { return dst ^ src; } },
        { ZYDIS_MNEMONIC_IMUL,
          [](uintptr_t dst, uintptr_t src) { return dst * src; } },
    };

#ifdef DEBUG_CONTEXT_PROPAGATION

void dump_register_value(std::ostream &os,
                         Dumper const &dumper,
                         Reflo &reflo,
                         Context const &context,
                         ZydisRegister reg)
{
    if (auto changed = context.get_register(reg); changed) {
        auto flo = reflo.get_flo_by_address(changed->source());
        if (flo) {
            if (!changed->is_symbolic()) {
                os << std::setfill('0') << std::hex << ' ' << std::setw(16)
                   << changed->value() << " \n";
            }
            else {
                os << std::setfill('0') << std::hex << '[' << std::setw(16)
                   << changed->symbol().id() << "]\n";
            }
        }
    }
}

#endif

Recontex::Recontex(Reflo &reflo)
    : reflo_(reflo)
    , pe_(reflo.get_pe())
    , max_analyzing_threads_(std::thread::hardware_concurrency())
{
}

void Recontex::analyze()
{
    for (auto const &[address, flo] : reflo_.get_flos()) {
        run_analysis(*flo);
    }
#ifdef DEBUG_ANALYSIS_PROGRESS
    std::clog << "Waiting for analysis to finish ...\n";
#endif
    wait_for_analysis();
#ifdef DEBUG_ANALYSIS_PROGRESS
    std::clog << "Done.\n";
#endif
}

void Recontex::set_max_analyzing_threads(size_t amount)
{
    max_analyzing_threads_ = amount;
}

Recontex::FloContexts const &Recontex::get_contexts(Flo const &flo) const
{
    return contexts_.at(flo.entry_point);
}

std::vector<Context const *> Recontex::get_contexts(Flo const &flo,
                                                    Address address) const
{
    std::vector<Context const *> contexts;
    auto const &flo_contexts = contexts_.at(flo.entry_point);
    auto range = utils::in_range(flo_contexts.equal_range(address));
    contexts.reserve(std::distance(range.begin(), range.end()));
    for (auto const &[addr, ctx] : range) {
        contexts.push_back(&ctx);
    }
    return contexts;
}

void Recontex::run_analysis(Flo &flo)
{
    auto lock = std::unique_lock(analyzing_threads_mutex_);
    analyzing_threads_cv_.wait(lock, [this] {
        return analyzing_threads_count_ < max_analyzing_threads_;
    });
    ++analyzing_threads_count_;
    analyzing_threads_.emplace_back([this, &flo]() mutable {
        ScopeGuard decrement_analyzing_threads_count([this]() noexcept {
            std::scoped_lock<std::mutex> notify_guard(analyzing_threads_mutex_);
            --analyzing_threads_count_;
            analyzing_threads_cv_.notify_all();
        });
#ifdef DEBUG_ANALYSIS_PROGRESS
        std::clog << "Analyzing: " << std::dec << analyzing_threads_.size()
                  << '/' << std::dec << reflo_.get_flos().size() << ": "
                  << std::setfill('0') << std::setw(8) << std::hex
                  << pe_.raw_to_virtual_address(flo.entry_point) << '\n';
#endif
        FloContexts flo_contexts;
        OptimalCoverage opt_cov(flo);
        if (!opt_cov.analyze()) {
#ifdef DEBUG_OPTIMAL_COVERAGE
            std::clog << "Optimal Coverage for " << std::hex
                      << std::setfill('0') << std::right << std::setw(8)
                      << pe_.raw_to_virtual_address(flo.entry_point)
                      << " cannot be calculated.\n";
#endif
            return;
        }
#ifdef DEBUG_OPTIMAL_COVERAGE
        auto get_va = [&pe_ = pe_](Address a) -> DWORD {
            return a ? pe_.raw_to_virtual_address(a) : 0;
        };
        std::clog << "Optimal Coverage @ " << std::hex << std::setfill('0')
                  << std::right << std::setw(8) << get_va(flo.entry_point)
                  << '\n';
        std::clog << "Nodes:\n";
        for (auto const &[_, node] : opt_cov.nodes()) {
            std::clog << std::hex << get_va(node.source) << " -> ";
            for (auto branch : node.branches) {
                std::clog << std::hex << get_va(branch.branch) << ' ';
            }
            std::clog << '\n';
        }
        std::clog << "\nNodes order:\n";
        std::vector<Address> nodes_order(opt_cov.nodes_order().size());
        for (auto [node, index] : opt_cov.nodes_order()) {
            nodes_order[index] = node;
        }
        for (auto node : nodes_order) {
            std::clog << std::hex << get_va(node) << '\n';
        }
        std::clog << "\nLoops:\n";
        for (auto const &loop : opt_cov.loops()) {
            std::clog << std::hex << get_va(loop.src) << " -> "
                      << get_va(loop.dst) << '\n';
        }
        std::clog << "\nUseless edges:\n";
        for (auto const &edge : opt_cov.useless_edges()) {
            std::clog << std::hex << get_va(edge.src) << " -> "
                      << get_va(edge.dst) << '\n';
        }
        if (opt_cov.paths().size() < 32) {
            std::clog << "\nOptimal paths:\n";
            for (auto const &path : opt_cov.paths()) {
                for (auto [jump, branch] : path) {
                    std::clog << std::hex << get_va(jump)
                              << (branch ? '+' : '-') << ' ';
                }
                std::clog << '\n';
            }
        }
        std::clog << '\n';
#endif
        analyze_flo(flo,
                    flo_contexts,
                    optimal_paths_to_analyze_paths(opt_cov.paths()),
                    make_flo_initial_contexts(flo),
                    flo.entry_point);
        for (auto const &cycle : opt_cov.loops()) {
            flo.add_cycle(cycle.src, cycle.dst);
        }
        {
            std::scoped_lock<std::mutex> add_contexts_guard(
                modify_access_contexts_mutex_);
            contexts_.emplace(flo.entry_point, std::move(flo_contexts));
        }
    });
}

void Recontex::wait_for_analysis()
{
    std::for_each(analyzing_threads_.begin(),
                  analyzing_threads_.end(),
                  [](std::thread &t) { t.join(); });
    analyzing_threads_.clear();
}

void Recontex::analyze_flo(Flo &flo,
                           FloContexts &flo_contexts,
                           AnalyzePaths paths,
                           Contexts contexts,
                           Address address)
{
    auto next_node = paths.begin();
    // Assert all paths have the same next jump
    assert(same_analyze_path(paths));
    auto last_instr = flo.get_disassembly().rbegin();
    auto end = last_instr->first + last_instr->second->length;
    while (address && address < end) {
        assert(!contexts.empty());
#ifdef DEBUG_CONTEXT_PROPAGATION
        DWORD va = pe_.raw_to_virtual_address(address);
#endif
        auto propagation_result =
            propagate_contexts(flo, flo_contexts, address, std::move(contexts));
        contexts = std::move(propagation_result.new_contexts);
        auto const instr = propagation_result.instruction;
#ifdef DEBUG_CONTEXT_PROPAGATION
        std::clog << std::dec << std::setfill(' ') << std::setw(5) << std::right
                  << contexts.size() << "/" << std::setw(5) << std::left
                  << flo_contexts.count(address);
        if (instr) {
            Dumper dumper;
            dumper.dump_instruction(std::clog, va, *instr);
    #ifdef DEBUG_CONTEXT_PROPAGATION_VALUES
            // Read values
            for (size_t i = 0; i < instr->operand_count; i++) {
                auto const &op = instr->operands[i];
                if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_READ)) {
                    continue;
                }
                for (auto const &context : contexts) {
                    switch (op.type) {
                    case ZYDIS_OPERAND_TYPE_REGISTER:
                        if (op.visibility
                            == ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
                            dump_register_value(std::clog,
                                                dumper,
                                                reflo_,
                                                context,
                                                op.reg.value);
                        }
                        break;
                    case ZYDIS_OPERAND_TYPE_MEMORY:
                        if (op.mem.base != ZYDIS_REGISTER_NONE
                            && op.mem.base != ZYDIS_REGISTER_RIP) {
                            dump_register_value(std::clog,
                                                dumper,
                                                reflo_,
                                                context,
                                                op.mem.base);
                        }
                        if (op.mem.index != ZYDIS_REGISTER_NONE) {
                            dump_register_value(std::clog,
                                                dumper,
                                                reflo_,
                                                context,
                                                op.mem.index);
                        }
                        break;
                    default: break;
                    }
                }
            }
    #endif
        }
        else {
            std::clog << std::hex << std::setfill('0') << std::setw(8)
                      << std::right << pe_.raw_to_virtual_address(address)
                      << '\n';
        }
#endif
        if (!instr || contexts.empty()) {
            break;
        }
        assert(next_node->current == next_node->end
               || next_node->current->jump >= address);
        if (Flo::is_any_jump(instr->mnemonic)) {
            if (next_node->current == next_node->end) {
                break;
            }
            assert(next_node->current->jump == address);
            auto skip_jump_paths = split_analyze_paths(paths);
            if (!skip_jump_paths.empty()) {
                advance_analyze_paths(skip_jump_paths);
                analyze_flo(flo,
                            flo_contexts,
                            std::move(skip_jump_paths),
                            make_child_contexts(contexts),
                            address + instr->length);
            }
            if (paths.empty()) {
                return;
            }
            assert(instr->operand_count > 0);
            auto const &op = instr->operands[0];
            assert(op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE);
            address += instr->length + op.imm.value.s;
            advance_analyze_paths(paths);
            assert(same_analyze_path(paths));
            next_node = paths.begin();
            continue;
        }
        else if (instr->mnemonic == ZYDIS_MNEMONIC_RET) {
            assert(next_node->current == next_node->end
                   || (next_node->current->jump == address
                       && std::next(next_node->current) == next_node->end));
#ifndef NDEBUG
            advance_analyze_paths(paths);
            assert(same_analyze_path(paths));
#endif
            break;
        }
        else {
            address += instr->length;
        }
    }
}

Recontex::AnalyzePaths
Recontex::optimal_paths_to_analyze_paths(OptimalCoverage::Paths const &paths)
{
    AnalyzePaths analyze_paths;
    analyze_paths.reserve(paths.size());
    for (auto const &path : paths) {
        analyze_paths.emplace_back(path);
    }
    return analyze_paths;
}

Recontex::AnalyzePaths Recontex::split_analyze_paths(AnalyzePaths &paths)
{
    AnalyzePaths skip_jump_paths;
    auto it =
        std::remove_if(paths.begin(), paths.end(), [](AnalyzePath const &path) {
            return !path.current->take;
        });
    skip_jump_paths.reserve(std::distance(it, paths.end()));
    std::copy(it, paths.end(), std::back_inserter(skip_jump_paths));
    paths.erase(it, paths.end());
    return skip_jump_paths;
}

void Recontex::advance_analyze_paths(AnalyzePaths &paths)
{
    for (auto &path : paths) {
        if (path.current != path.end) {
            ++path.current;
        }
    }
}

bool Recontex::same_analyze_path(AnalyzePaths const &paths)
{
    return std::all_of(
        paths.begin(),
        paths.end(),
        [&paths](AnalyzePath const &path) {
            if (path.current == path.end) {
                return paths.front().current == paths.front().end;
            }
            assert(paths.front().current != paths.front().end);
            return path.current->jump == paths.front().current->jump;
        });
}

Recontex::PropagationResult
Recontex::propagate_contexts(Flo const &flo,
                             FloContexts &flo_contexts,
                             Address address,
                             Contexts contexts)
{
    PropagationResult result;
    result.instruction = flo.get_instruction(address);
    if (!result.instruction) {
        return result;
    }
    while (!contexts.empty()) {
        auto const &context =
            emplace_context(flo_contexts, address, contexts.pop());
        auto new_context = context.make_child();
        emulate(address, *result.instruction, new_context);
        result.new_contexts.emplace(std::move(new_context));
    }
    return result;
}

Context const &Recontex::emplace_context(FloContexts &flo_contexts,
                                         Address address,
                                         Context &&context)
{
    auto range = utils::in_range(flo_contexts.equal_range(address));
    auto insert_hint = std::upper_bound(range.begin(),
                                        range.end(),
                                        context.get_hash(),
                                        [](size_t hash, auto const &it) {
                                            return hash < it.second.get_hash();
                                        });
    auto emplaced = flo_contexts.emplace_hint(insert_hint,
                                              address,
                                              std::forward<Context>(context));
    return emplaced->second;
}

void Recontex::emulate(Address address,
                       ZydisDecodedInstruction const &instruction,
                       Context &context)
{
    assert(address);

    // Operations with:
    // * (A)L, (A)H, (A)X / 8, 16 bits - do not affect HO bits
    // * E(A)X / 32 bits - zerorize HO bits.

    using namespace std::placeholders;

    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_MOV:
    case ZYDIS_MNEMONIC_MOVZX:
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD: {
        emulate_instruction(
            instruction,
            context,
            address,
            [](virt::Value const &dst, virt::Value const &src) -> virt::Value {
                uintptr_t mask = ~0;
                if (dst.size() < 8) {
                    mask = (1ULL << (dst.size() * 8)) - 1;
                }
                if (!dst.is_symbolic() && !src.is_symbolic()
                    && dst.size() < 4) {
                    return virt::make_value(src.source(),
                                            (dst.value() & ~mask)
                                                | (src.value() & mask),
                                            dst.size());
                }
                else if (!src.is_symbolic()) {
                    return virt::make_value(src.source(),
                                            src.value() & mask,
                                            dst.size());
                }
                else {
                    return src;
                }
            });
    } break;
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_IMUL: {
        auto action = emulation_callback_actions_.at(instruction.mnemonic);
        auto callback =
            std::bind(&Recontex::emulate_instruction_helper, _1, _2, action);
        emulate_instruction(instruction, context, address, callback);
    } break;
    case ZYDIS_MNEMONIC_LEA:
        emulate_instruction_lea(instruction, context, address);
        break;
    case ZYDIS_MNEMONIC_PUSH:
        emulate_instruction_push(instruction, context, address);
        break;
    case ZYDIS_MNEMONIC_POP:
        emulate_instruction_pop(instruction, context, address);
        break;
    case ZYDIS_MNEMONIC_CALL:
        emulate_instruction_call(instruction, context, address);
        break;
    case ZYDIS_MNEMONIC_RET:
        emulate_instruction_ret(instruction, context, address);
        break;
    case ZYDIS_MNEMONIC_INC:
        emulate_instruction_inc(instruction, context, address, +1);
        break;
    case ZYDIS_MNEMONIC_DEC:
        emulate_instruction_inc(instruction, context, address, -1);
        break;
    default:
        for (size_t i = 0; i < instruction.operand_count; i++) {
            auto const &op = instruction.operands[i];
            if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
                continue;
            }
            switch (op.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                context.set_register(
                    op.reg.value,
                    virt::make_symbolic_value(address, op.element_size / 8));
                break;
            case ZYDIS_OPERAND_TYPE_MEMORY:
                context.set_memory(
                    get_memory_address(op, context).raw_address_value(),
                    virt::make_symbolic_value(address, op.element_size / 8));
                break;
            default: break;
            }
        }
        break;
    }
}

void Recontex::emulate_instruction(ZydisDecodedInstruction const &instruction,
                                   Context &context,
                                   Address address,
                                   EmulationCallback const &callback)
{
    Operand dst = get_operand(instruction.operands[0], context, address);
    Operand src;
    virt::Value imm;
    int op_count = 1;
    if (instruction.operand_count >= 2) {
        op_count = 2;
        if (auto op2 = instruction.operands[1];
            op2.visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
            src = get_operand(op2, context, address);
        }
    }
    if (instruction.operand_count >= 3) {
        if (auto op3 = instruction.operands[2];
            op3.visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
            op_count = 3;
            if (op3.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                imm = get_operand(op3, context, address).value;
            }
        }
    }
    if (instruction.mnemonic == ZYDIS_MNEMONIC_XOR && dst.reg == src.reg) {
        dst.value = virt::make_value(address,
                                     0,
                                     instruction.operands[1].element_size / 8);
    }
    else {
        if (op_count == 2) {
            dst.value = callback(dst.value, src.value);
        }
        else if (instruction.operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            // TODO: use dst, if needed
            // for now, assume it's unused.
            dst.value = callback(src.value, imm);
        }
        else {
            dst.value = virt::make_symbolic_value(address, dst.value.size());
        }
    }
    dst.value.set_source(address);
    if (dst.reg != ZYDIS_REGISTER_NONE) {
        context.set_register(dst.reg, dst.value);
    }
    else if (dst.address) {
        context.set_memory(*dst.address, dst.value);
    }
}

void Recontex::emulate_instruction_lea(
    ZydisDecodedInstruction const &instruction,
    Context &context,
    Address address)
{
    Operand dst = get_operand(instruction.operands[0], context, address);
    Operand src = get_operand(instruction.operands[1], context, address);
    assert(dst.reg != ZYDIS_REGISTER_NONE);
    if (src.address) {
        context.set_register(dst.reg, virt::make_value(address, *src.address));
    }
}

void Recontex::emulate_instruction_push(
    ZydisDecodedInstruction const &instruction,
    Context &context,
    Address address)
{
    assert(instruction.mnemonic == ZYDIS_MNEMONIC_PUSH);
    if (auto rsp = context.get_register(ZYDIS_REGISTER_RSP);
        rsp && !rsp->is_symbolic()) {
        auto new_rsp = rsp->value() - 8;
        auto op = get_operand(instruction.operands[0], context, address);
        op.value.set_source(address);
        context.set_register(ZYDIS_REGISTER_RSP,
                             virt::make_value(address, new_rsp));
        context.set_memory(new_rsp, op.value);
    }
}

void Recontex::emulate_instruction_pop(
    ZydisDecodedInstruction const &instruction,
    Context &context,
    Address address)
{
    assert(instruction.mnemonic == ZYDIS_MNEMONIC_POP);
    if (auto rsp = context.get_register(ZYDIS_REGISTER_RSP);
        rsp && !rsp->is_symbolic()) {
        auto new_rsp = rsp->value() + 8;
        context.set_register(ZYDIS_REGISTER_RSP,
                             virt::make_value(address, new_rsp));
        auto op = get_operand(instruction.operands[0], context, address);
        op.value.set_source(address);
        if (op.reg != ZYDIS_REGISTER_NONE) {
            context.set_register(op.reg, op.value);
        }
        else if (op.address) {
            context.set_memory(*op.address, op.value);
        }
    }
}

void Recontex::emulate_instruction_call(
    ZydisDecodedInstruction const &instruction,
    Context &context,
    Address address)
{
    assert(instruction.mnemonic == ZYDIS_MNEMONIC_CALL);
    // Assume RSP will be the same after the call
    /*
    if (auto rsp = context.get_register(ZYDIS_REGISTER_RSP);
        rsp && !rsp->is_symbolic()) {
        auto new_rsp = rsp->value() - 8;
        auto return_address =
            pe_.raw_to_virtual_address(address + instruction.length);
        context.set_memory(new_rsp, virt::make_value(address, return_address));
        context.set_register(ZYDIS_REGISTER_RSP,
                             virt::make_value(address, new_rsp));
    }
    */
    // Reset volatile registers
    for (auto volatile_register : volatile_registers_) {
        context.set_register(volatile_register,
                             virt::make_symbolic_value(address));
    }
}

void Recontex::emulate_instruction_ret(
    ZydisDecodedInstruction const &instruction,
    Context &context,
    Address address)
{
    assert(instruction.mnemonic == ZYDIS_MNEMONIC_RET);
    if (auto rsp = context.get_register(ZYDIS_REGISTER_RSP);
        rsp && !rsp->is_symbolic()) {
        auto new_rsp = rsp->value() + 8;
        context.set_register(ZYDIS_REGISTER_RSP,
                             virt::make_value(address, new_rsp));
    }
}

void Recontex::emulate_instruction_inc(
    ZydisDecodedInstruction const &instruction,
    Context &context,
    Address address,
    int offset)
{
    assert(instruction.mnemonic == ZYDIS_MNEMONIC_INC
           || instruction.mnemonic == ZYDIS_MNEMONIC_DEC);
    Operand dst = get_operand(instruction.operands[0], context, address);
    virt::Value result;
    if (!dst.value.is_symbolic()) {
        result = virt::make_value(address, dst.value.value() + offset);
    }
    else {
        result = virt::make_symbolic_value(address,
                                           8,
                                           dst.value.symbol().offset() + offset,
                                           dst.value.symbol().id());
    }
    if (dst.reg != ZYDIS_REGISTER_NONE) {
        context.set_register(dst.reg, result);
    }
    else if (dst.address) {
        context.set_memory(*dst.address, result);
    }
}

virt::Value Recontex::emulate_instruction_helper(
    virt::Value const &dst,
    virt::Value const &src,
    std::function<uintptr_t(uintptr_t, uintptr_t)> action)
{
    if (!dst.is_symbolic() && !src.is_symbolic()) {
        uintptr_t mask = ~0;
        if (dst.size() < 8) {
            mask = (1ULL << (dst.size() * 8)) - 1;
        }
        if (dst.size() < 4) {
            return virt::make_value(
                src.source(),
                (dst.value() & ~mask)
                    | (action(dst.value(), src.value()) & mask),
                dst.size());
        }
        else {
            return virt::make_value(src.source(),
                                    action(dst.value(), src.value()) & mask,
                                    dst.size());
        }
    }
    else if (dst.is_symbolic() && !src.is_symbolic()) {
        return virt::make_symbolic_value(
            src.source(),
            dst.size(),
            action(dst.symbol().offset(), src.value()),
            dst.symbol().id());
    }
    return virt::make_symbolic_value(src.source(), dst.size());
}

Recontex::Operand Recontex::get_operand(ZydisDecodedOperand const &operand,
                                        Context const &context,
                                        Address source)
{
    Operand op;
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        op.value = virt::make_value(
            source,
            operand.imm.is_signed ? operand.imm.value.s : operand.imm.value.u,
            operand.element_size / 8);
    } break;
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        op.reg = operand.reg.value;
        if (auto valsrc = context.get_register(op.reg); valsrc) {
            op.value = *valsrc;
            op.value.set_size(operand.element_size / 8);
        }
        else {
            op.value =
                virt::make_symbolic_value(source, operand.element_size / 8);
        }
    } break;
    case ZYDIS_OPERAND_TYPE_MEMORY: {
        op.address = get_memory_address(operand, context).raw_address_value();
        if (op.address && operand.element_size) {
            op.value =
                context.get_memory(*op.address, operand.element_size / 8);
        }
        else {
            op.value =
                virt::make_symbolic_value(source, operand.element_size / 8);
        }
    } break;
    default:
        op.value = virt::make_symbolic_value(source, operand.element_size / 8);
        break;
    }
    return op;
}

virt::Value Recontex::get_memory_address(ZydisDecodedOperand const &op,
                                         Context const &context)
{
    assert(op.type == ZYDIS_OPERAND_TYPE_MEMORY);
    bool symbolic = false;
    uintptr_t value = 0;
    uintptr_t symbol = 0;
    if (op.mem.base != ZYDIS_REGISTER_NONE
        && op.mem.base != ZYDIS_REGISTER_RIP) {
        auto base = context.get_register(op.mem.base);
        if (base && !base->is_symbolic()) {
            value += base->value();
        }
        else {
            symbolic = true;
        }
        if (base && base->is_symbolic()) {
            if (auto base_reg = virt::Registers::from_zydis(op.mem.base);
                base_reg) {
                utils::hash::combine(symbol, *base_reg);
            }
            utils::hash::combine(symbol, base->symbol().id());
            utils::hash::combine(symbol, base->symbol().offset());
        }
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE) {
        auto index = context.get_register(op.mem.index);
        if (index && !index->is_symbolic()) {
            value += index->value() * op.mem.scale;
        }
        else {
            symbolic = true;
        }
        if (index && index->is_symbolic()) {
            if (auto index_reg = virt::Registers::from_zydis(op.mem.index);
                index_reg) {
                utils::hash::combine(symbol, *index_reg);
            }
            utils::hash::combine(symbol, index->symbol().id());
            utils::hash::combine(symbol, index->symbol().offset());
        }
        utils::hash::combine(symbol, op.mem.scale);
    }
    if (op.mem.disp.has_displacement) {
        value += op.mem.disp.value;
        utils::hash::combine(symbol, op.mem.disp.value);
    }
    if (op.element_size == 0) {
        utils::hash::combine(symbol, true);
    }
    if (symbolic) {
        if (op.mem.base == ZYDIS_REGISTER_RSP) {
            symbol = magic_stack_value_mask_ | (symbol & 0xFFFFFFFFULL);
        }
        return virt::make_symbolic_value(nullptr, 8, 0, symbol);
    }
    return virt::make_value(nullptr, value);
}

bool Recontex::points_to_stack(ZydisRegister reg,
                               Address address,
                               FloContexts const &flo_contexts)
{
    if (reg == ZYDIS_REGISTER_RSP) {
        return true;
    }
    for (auto const &context : utils::multimap_values(flo_contexts, address)) {
        if (auto value = context.get_register(reg);
            value && !value->is_symbolic()) {
            if (points_to_stack(value->value())) {
                return true;
            }
        }
    }
    return false;
}

bool Recontex::points_to_stack(uintptr_t value)
{
    return (value & magic_stack_value_mask_) == magic_stack_value_mask_;
}

unsigned rstc::Recontex::stack_argument_number(uintptr_t value)
{
    assert(points_to_stack(value));
    auto offset = value & 0xFFFFFFFF;
    assert(offset & -8); // divisible by 8
    return offset / 8 - 1;
}

Contexts Recontex::make_flo_initial_contexts(Flo &flo)
{
    auto c = Context(nullptr);
    c.set_register(ZYDIS_REGISTER_RSP,
                   virt::make_value(flo.entry_point, magic_stack_value_ << 32));
    Contexts contexts;
    contexts.emplace(std::move(c));
    return contexts;
}

bool Recontex::instruction_has_memory_access(
    ZydisDecodedInstruction const &instr)
{
    return std::any_of(instr.operands,
                       instr.operands + instr.operand_count,
                       operand_has_memory_access);
}

bool Recontex::operand_has_memory_access(ZydisDecodedOperand const &op)
{
    return op.type == ZYDIS_OPERAND_TYPE_MEMORY
           && op.visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT;
}

bool Recontex::instruction_has_nonstack_memory_access(
    ZydisDecodedInstruction const &instr)
{
    return std::any_of(instr.operands,
                       instr.operands + instr.operand_count,
                       operand_has_nonstack_memory_access);
}

bool Recontex::operand_has_nonstack_memory_access(ZydisDecodedOperand const &op)
{
    return op.type == ZYDIS_OPERAND_TYPE_MEMORY
           && op.visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT
           && op.mem.base != ZYDIS_REGISTER_RSP
           && op.mem.index != ZYDIS_REGISTER_RSP;
}

bool Recontex::is_history_term_instr(ZydisDecodedInstruction const &instr)
{
    if (instr.mnemonic == ZYDIS_MNEMONIC_XOR) {
        auto const &dst = instr.operands[0];
        auto const &src = instr.operands[1];
        if (dst.type == ZYDIS_OPERAND_TYPE_REGISTER
            && src.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            return dst.reg.value == src.reg.value;
        }
    }
    return false;
}

void Recontex::debug(std::ostream &os)
{
    Dumper dumper;
    for (auto const &[entry_point, flo] : reflo_.get_flos()) {
        if (get_contexts(*flo).empty()) {
            continue;
        }
        for (auto const &[address, instr] : flo->get_disassembly()) {
            if (!instruction_has_nonstack_memory_access(*instr)) {
                continue;
            }
            dump_instruction_history(os,
                                     dumper,
                                     address,
                                     *instr,
                                     get_contexts(*flo, address));
            os << "-----------------------------------------\n";
        }
    }
}

void Recontex::dump_register_history(std::ostream &os,
                                     Dumper const &dumper,
                                     Context const &context,
                                     ZydisRegister reg,
                                     std::unordered_set<Address> &visited) const
{
    {
        if (auto changed = context.get_register(reg); changed) {
            auto flo = reflo_.get_flo_by_address(changed->source());
            if (flo && !visited.contains(changed->source())) {
                visited.emplace(changed->source());
                if (!changed->is_symbolic()) {
                    os << std::hex << ' ' << std::setw(16) << changed->value()
                       << "      \t";
                }
                else {
                    os << std::hex << '[' << std::setw(8)
                       << changed->symbol().id() << '+' << std::setw(4)
                       << changed->symbol().offset() << "]\t";
                }
                dump_instruction_history(
                    os,
                    dumper,
                    changed->source(),
                    *flo->get_disassembly().at(changed->source()),
                    get_contexts(*flo, changed->source()),
                    visited);
                os << "---\n";
            }
        }
    }
}

void Recontex::dump_memory_history(std::ostream &os,
                                   Dumper const &dumper,
                                   Context const &context,
                                   ZydisDecodedOperand const &op,
                                   std::unordered_set<Address> &visited) const
{
    auto mem_addr =
        Recontex::get_memory_address(op, context).raw_address_value();
    auto values = context.get_memory(mem_addr, op.element_size / 8);
    std::unordered_set<Address> sources;
    for (auto const &value : values.container) {
        sources.emplace(value.source());
    }
    for (auto source : sources) {
        if (visited.contains(source)) {
            continue;
        }
        if (auto flo = reflo_.get_flo_by_address(source); flo) {
            visited.emplace(source);
            dump_instruction_history(os,
                                     dumper,
                                     source,
                                     *flo->get_disassembly().at(source),
                                     get_contexts(*flo, source),
                                     visited);
        }
    }
}

void Recontex::dump_instruction_history(
    std::ostream &os,
    Dumper const &dumper,
    Address address,
    ZydisDecodedInstruction const &instr,
    std::vector<Context const *> const &contexts,
    std::unordered_set<Address> visited) const
{
    visited.emplace(address);
    DWORD va = pe_.raw_to_virtual_address(address);
    dumper.dump_instruction(os, va, instr);
    if (is_history_term_instr(instr)) {
        return;
    }
    for (size_t i = 0; i < instr.operand_count; i++) {
        auto const &op = instr.operands[i];
        if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_READ)) {
            continue;
        }
        for (auto const &context : contexts) {
            switch (op.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                if (op.visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
                    dump_register_history(os,
                                          dumper,
                                          *context,
                                          op.reg.value,
                                          visited);
                }
                break;
            case ZYDIS_OPERAND_TYPE_MEMORY:
                if (op.mem.base != ZYDIS_REGISTER_NONE
                    && op.mem.base != ZYDIS_REGISTER_RIP) {
                    dump_register_history(os,
                                          dumper,
                                          *context,
                                          op.mem.base,
                                          visited);
                }
                if (op.mem.index != ZYDIS_REGISTER_NONE) {
                    dump_register_history(os,
                                          dumper,
                                          *context,
                                          op.mem.index,
                                          visited);
                }
                dump_memory_history(os, dumper, *context, op, visited);
                break;
            default: break;
            }
        }
    }
}

Recontex::OptimalCoverage::OptimalCoverage(Flo const &flo)
    : flo_(flo)
{
}

bool Recontex::OptimalCoverage::analyze()
{
    if (!build_nodes()) {
        return false;
    }
    assert(validate_nodes());
    normalize_nodes();
    top_sort();
    find_loops();
    find_useless_edges();
    build_paths();
    return true;
}

bool Recontex::OptimalCoverage::build_nodes()
{
    auto const &disassembly = flo_.get_disassembly();
    for (auto it = disassembly.begin(); it != disassembly.end(); ++it) {
        auto const &instruction = *it->second;
        if (Flo::is_any_jump(instruction.mnemonic)) {
            Address dst = Flo::get_jump_destination(it->first, *it->second);
            if (!dst) {
                return false;
            }
            if (flo_.is_inside(dst)) {
                std::list<Branch> branches;
                Address src = it->first;
                Address next = nullptr;
                while (it != disassembly.end()
                       && Flo::is_conditional_jump(it->second->mnemonic)) {
                    Address dst =
                        Flo::get_jump_destination(it->first, *it->second);
                    if (!dst) {
                        return false;
                    }
                    if (!flo_.is_inside(dst)) {
                        break;
                    }
                    branches.emplace_back(it->first, dst, Branch::Conditional);
                    next = it->first + it->second->length;
                    ++it;
                }
                if (it != disassembly.end()) {
                    if (it->second->mnemonic == ZYDIS_MNEMONIC_JMP) {
                        auto dst =
                            Flo::get_jump_destination(it->first, *it->second);
                        if (!dst) {
                            return false;
                        }
                        if (flo_.is_inside(dst)) {
                            branches.emplace_front(it->first,
                                                   dst,
                                                   Branch::Unconditional);
                        }
                    }
                    else if (next) {
                        branches.emplace_front(std::prev(it)->first,
                                               next,
                                               Branch::Next);
                    }
                }
                if (!branches.empty()) {
                    nodes_.emplace(src, Node(src, std::move(branches)));
                }
                if (it == disassembly.end()) {
                    break;
                }
            }
            else if (dst) {
                nodes_.emplace(it->first, Node(it->first, {}));
                ends_.emplace(it->first);
            }
        }
        else if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) {
            nodes_.emplace(it->first, Node(it->first, {}));
            ends_.emplace(it->first);
        }
    }
    return true;
}

bool Recontex::OptimalCoverage::validate_nodes()
{
    for (auto const &[_, node] : nodes_) {
        if (node.branches.size() == 1) {
            assert(node.branches.front().type == Branch::Unconditional);
        }
        else if (node.branches.size() > 1) {
            assert(node.branches.front().type == Branch::Next
                   || node.branches.front().type == Branch::Unconditional);
            assert(node.branches.front().source >= std::next(node.branches.begin())->source);
            for (auto it = std::next(node.branches.begin(), 2);
                 it != node.branches.end();
                 ++it) {
                assert(it->source > std::prev(it)->source);
            }
        }
    }
    return true;
}

void Recontex::OptimalCoverage::normalize_nodes()
{
    for (auto &[_, node] : nodes_) {
        for (auto &branch : node.branches) {
            if (!branch.branch) {
                continue;
            }
            if (auto it = nodes_.lower_bound(branch.branch);
                it != nodes_.end()) {
                branch.branch = it->first;
            }
        }
    }
}

void Recontex::OptimalCoverage::top_sort()
{
    if (nodes_.empty()) {
        return;
    }
    std::vector<Address> top_sort;
    std::unordered_set<Address> visited;
    std::function<void(Address)> dfs = [&](Address v) mutable {
        auto it = nodes_.lower_bound(v);
        Node const *node = nullptr;
        if (it != nodes_.end()) {
            node = &it->second;
            v = node->source;
        }
        auto [_, inserted] = visited.insert(v);
        if (!inserted) {
            return;
        }
        if (node) {
            for (auto branch : node->branches) {
                dfs(branch.branch);
            }
        }
        top_sort.push_back(v);
    };
    dfs(flo_.entry_point);
    for (auto it = top_sort.rbegin(); it != top_sort.rend(); ++it) {
        nodes_order_.emplace(*it, std::distance(top_sort.rbegin(), it));
    }
}

void Recontex::OptimalCoverage::find_loops()
{
    for (auto const &[_, node] : nodes_) {
        for (auto branch : node.branches) {
            if (nodes_order_.at(branch.branch) <= nodes_order_.at(node.source)) {
                loops_.emplace(node.source, branch.branch);
            }
        }
    }
}

void Recontex::OptimalCoverage::find_useless_edges()
{
    auto is_reachable =
        [&](Edge const &blocked, Address start, Address end) -> bool {
        std::unordered_set<Address> visited;
        std::function<bool(Address)> dfs = [&](Address v) -> bool {
            if (auto it = nodes_order_.find(v);
                it == nodes_order_.end() || it->second > nodes_order_[end]) {
                return false;
            }
            visited.insert(v);
            if (auto it = nodes_.find(v); it != nodes_.end()) {
                auto const &node = it->second;
                for (auto branch : node.branches) {
                    Edge edge(node.source, branch.branch);
                    if (edge != blocked && !loops_.contains(edge)) {
                        if (edge.dst == end) {
                            return true;
                        }
                        if (!visited.contains(edge.dst)) {
                            if (dfs(edge.dst)) {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        };
        return dfs(start);
    };
    for (auto const &[_, node] : nodes_) {
        if (node.branches.empty()) {
            continue;
        }
        for (auto branch : node.branches) {
            if (is_reachable(Edge(node.source, branch.branch),
                             node.source,
                             branch.branch)) {
                useless_edges_.emplace(node.source, branch.branch);
            }
        }
    }
}

void Recontex::OptimalCoverage::build_paths()
{
    if (nodes_.empty()) {
        paths_.push_back(Path{});
        return;
    }
    Edges visited_loops;
    Path path;
    std::function<void(Address)> dfs = [&](Address v) {
        if (ends_.contains(v) || !nodes_.contains(v)) {
            paths_.push_back(path);
            return;
        }
        auto const &node = nodes_.at(v);
        assert(!node.branches.empty());
        size_t nodes_added = 0;
        std::vector<std::list<Branch>::const_iterator> branches;
        branches.reserve(node.branches.size());
        for (auto it = std::next(node.branches.begin());
             it != node.branches.end();
             ++it) {
            branches.push_back(it);
        }
        branches.push_back(node.branches.begin());
        for (auto it : branches) {
            auto const &branch = *it;
            if (it != node.branches.begin() || nodes_added == 0) {
                bool is_jump = branch.type == Branch::Conditional
                               || branch.type == Branch::Unconditional;
                path.emplace_back(branch.source, is_jump);
                nodes_added++;
            }
            else {
                assert(it == node.branches.begin());
                path.back().take = false;
                if (branch.type == Branch::Unconditional) {
                    path.emplace_back(branch.source, true);
                    nodes_added++;
                }
            }
            // Visit edge
            Edge edge(node.source, branch.branch);
            bool loop = false;
            if (loops_.contains(edge)) {
                auto [_, inserted] = visited_loops.insert(edge);
                if (!inserted) {
                    continue;
                }
                loop = true;
            }
            if (!useless_edges_.contains(edge)) {
                dfs(edge.dst);
            }
            if (loop) {
                visited_loops.erase(edge);
            }
        }
        path.erase(path.end() - nodes_added, path.end());
    };
    dfs(nodes_.lower_bound(flo_.entry_point)->first);
}
