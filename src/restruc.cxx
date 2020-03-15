#include "restruc.hxx"

#include "zyan_error.hxx"

#include <cinttypes>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>

using namespace rstc;

#define ZYAN_THROW(expr)               \
    do {                               \
        ZyanStatus _status = (expr);   \
        if (ZYAN_FAILED(_status)) {    \
            throw zyan_error(_status); \
        }                              \
    } while (0)

static bool is_conditional_jump(ZydisMnemonic mnemonic)
{
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_JCXZ:
    case ZYDIS_MNEMONIC_JECXZ:
    case ZYDIS_MNEMONIC_JKNZD:
    case ZYDIS_MNEMONIC_JKZD:
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_JMP:
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JZ:
        // Jxx
        return true;
    case ZYDIS_MNEMONIC_LOOP:
    case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
        // LOOPxx
        return true;
    }
    return false;
}

void Restruc::Flow::merge(Flow &other)
{
    instructions.merge(other.instructions);
    impl::merge_keeping_src_unique(inner_jumps, other.inner_jumps);
    impl::merge_keeping_src_unique(outer_jumps, other.outer_jumps);
    impl::merge_keeping_src_unique(unknown_jumps, other.unknown_jumps);
    impl::merge_keeping_src_unique(calls, other.calls);
}

Restruc::CFGraph::CFGraph()
    : entry_point(nullptr)
    , outer_cfgraph(nullptr)
{
}

Restruc::CFGraph::CFGraph(Address entry_point, CFGraph *outer_cfgraph)
    : entry_point(entry_point)
    , outer_cfgraph(outer_cfgraph)
{
}

bool Restruc::CFGraph::is_complete() const
{
    return !instructions.empty() && unknown_jumps.empty() && has_ret;
}

bool Restruc::CFGraph::can_merge_with_outer_cfgraph() const
{
    if (!outer_cfgraph) {
        return false;
    }
    if (is_complete()) {
        return true;
    }
    if (instructions.empty()) {
        return false;
    }
    auto const &last_outer_instruction = *outer_cfgraph->instructions.rbegin();
    auto const &first_instruction = *instructions.begin();
    // Can merge if first instruction of this CFGraph is comes right
    // after the last instruction of outer CFGraph.
    return first_instruction.first
           == last_outer_instruction.first
                  + last_outer_instruction.second.length;
}

Address Restruc::CFGraph::analyze(Address address)
{
    auto const &instruction = instructions[address];
    Address next_address = address + instruction.length;
    visit(address);
    if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
        // Assume calls always return (i.e. they are not no-return)
        Address dst = next_address + instruction.operands[0].imm.value.s;
        add_call(dst, address, next_address);
    }
    else if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) {
        has_ret = true;
        if (!is_inside(next_address)) {
            next_address = nullptr;
        }
    }
    else if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP
             || is_conditional_jump(instruction.mnemonic)) {
        bool unconditional = instruction.mnemonic == ZYDIS_MNEMONIC_JMP;
        auto offset = instruction.operands[0].imm.value.s;
        Address dst = next_address + offset;
        auto type = get_jump_type(dst, address, next_address);
        add_jump(type, dst, address);
        if (unconditional) {
            switch (type) {
            case Jump::Unknown:
                if (promote_unknown_jump(next_address, Jump::Inner)) {
                    return next_address;
                }
                else {
                    return nullptr;
                }
                break;
            case Jump::Inner:
                if (dst >= next_address) {
                    return next_address;
                }
                else {
                    // Looping inside CFGraph
                    return nullptr;
                }
            case Jump::Outer:
                //
                return nullptr;
            }
        }
    }
    return next_address;
}

void Restruc::CFGraph::add_instruction(
    Address address,
    ZydisDecodedInstruction const &instruction)
{
    instructions.emplace(address, instruction);
}

void Restruc::CFGraph::add_jump(Jump::Type type, Address dst, Address src)
{
    switch (type) {
    case Jump::Inner:
        inner_jumps.emplace(dst, Jump(Jump::Inner, dst, src));
        break;
    case Jump::Outer:
        outer_jumps.emplace(dst, Jump(Jump::Outer, dst, src));
        break;
    case Jump::Unknown:
        unknown_jumps.emplace(dst, Jump(Jump::Unknown, dst, src));
        break;
    }
}

void Restruc::CFGraph::add_call(Address dst, Address src, Address ret)
{
    calls.emplace(src, Call(dst, src, ret));
}

bool Restruc::CFGraph::promote_unknown_jump(Address dst, Jump::Type new_type)
{
    bool promoted = false;
    while (true) {
        if (auto jump = unknown_jumps.extract(dst); !jump.empty()) {
            promoted = true;
            add_jump(new_type, dst, jump.mapped().src);
        }
        else {
            break;
        }
    }
    return promoted;
}

bool Restruc::CFGraph::promote_outer_unknown_jump(Address dst,
                                                  Jump::Type new_type)
{
    if (!outer_cfgraph) {
        return false;
    }
    return outer_cfgraph->promote_unknown_jump(dst, new_type);
}

void Restruc::CFGraph::visit(Address address)
{
    promote_unknown_jump(address, Jump::Inner);
    promote_outer_unknown_jump(address, Jump::Inner);
}

Restruc::Jump::Type
Restruc::CFGraph::get_jump_type(Address dst, Address src, Address next)
{
    // If jumping with offset 0, i.e. no jump
    if (dst == next) {
        return Jump::Inner;
    }
    // If jump is first function instruction
    if (instructions.size() == 1) {
        return Jump::Outer;
    }
    // If destination is one of the previous instructions
    if (instructions.find(dst) != instructions.end()) {
        return Jump::Inner;
    }
    // If jumping above entry-point
    if (dst < entry_point) {
        return Jump::Outer;
    }
    return Jump::Unknown;
}

bool Restruc::CFGraph::is_inside(Address address)
{
    return instructions.find(address) != instructions.end()
           || inner_jumps.find(address) != inner_jumps.end();
}

Restruc::Restruc(std::filesystem::path const &pe_path)
    : pe_(pe_path)
{
    ZYAN_THROW(ZydisDecoderInit(&decoder_,
                                ZYDIS_MACHINE_MODE_LONG_64,
                                ZYDIS_ADDRESS_WIDTH_64));
#ifndef NDEBUG
    ZYAN_THROW(ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL));
#endif
}

void Restruc::fill_cfgraph(CFGraph &cfgraph)
{
    Address address;
    Address next_address;
    if (cfgraph.instructions.empty()) {
        next_address = cfgraph.entry_point;
    }
    else {
        next_address = cfgraph.instructions.rbegin()->first;
    }
    Address end = pe_.get_end(next_address);
    while (true) {
        address = next_address;
        if (address == nullptr || address >= end) {
            break;
        }

        ZydisDecodedInstruction instruction;
        ZYAN_THROW(ZydisDecoderDecodeBuffer(&decoder_,
                                            address,
                                            end - address,
                                            &instruction));
#ifndef NDEBUG
        dump_instruction(std::clog,
                         pe_.raw_to_virtual_address(address),
                         instruction);
#endif

        cfgraph.add_instruction(address, instruction);
        next_address = cfgraph.analyze(address);
    }
}

void Restruc::resolve_incomplete_cfgraph(CFGraph &outer_cfgraph)
{
    if (outer_cfgraph.instructions.empty()
        || outer_cfgraph.unknown_jumps.empty()) {
        return;
    }
    while (!outer_cfgraph.unknown_jumps.empty()) {
        auto const unknown_jump_dst =
            outer_cfgraph.unknown_jumps.begin()->first;
        CFGraph new_cfgraph(unknown_jump_dst, &outer_cfgraph);
        Address address;
        Address next_address = new_cfgraph.entry_point;
        Address end = pe_.get_end(new_cfgraph.entry_point);
        bool can_merge = false;
        while (!can_merge) {
            address = next_address;
            if (address == nullptr || address >= end) {
                break;
            }

            ZydisDecodedInstruction instruction;
            ZYAN_THROW(ZydisDecoderDecodeBuffer(&decoder_,
                                                address,
                                                end - address,
                                                &instruction));
#ifndef NDEBUG
            dump_instruction(std::clog,
                             pe_.raw_to_virtual_address(address),
                             instruction);
#endif

            new_cfgraph.add_instruction(address, instruction);
            next_address = new_cfgraph.analyze(address);
            can_merge = new_cfgraph.can_merge_with_outer_cfgraph();
        }
        if (can_merge) {
            outer_cfgraph.merge(new_cfgraph);
            break;
        }
        else {
            outer_cfgraph.promote_unknown_jump(unknown_jump_dst, Jump::Outer);
        }
    }
}

void Restruc::create_function(Address entry_point)
{
    // Prevent recursive analysis
    if (functions_.find(entry_point) != functions_.end()) {
        return;
    }

    Address address = entry_point;
    CFGraph cfgraph(entry_point);
    while (true) {
        fill_cfgraph(cfgraph);
        if (cfgraph.is_complete()) {
            break;
        }
        resolve_incomplete_cfgraph(cfgraph);
    }

    functions_.emplace(entry_point, cfgraph);
    unanalyzed_functions_.push_back(entry_point);
}

Address Restruc::pop_unanalyzed_function()
{
    auto address = unanalyzed_functions_.front();
    unanalyzed_functions_.pop_front();
    return address;
}

void Restruc::analyze()
{
    create_function(pe_.get_entry_point());
    while (!unanalyzed_functions_.empty()) {
        auto &function = functions_[pop_unanalyzed_function()];

        // Iterate over unique call destinations
        for (auto it = function.calls.begin(), end = function.calls.end();
             it != end;
             it = function.calls.upper_bound(it->first)) {
            create_function(it->second.dst);
        }

        // Iterate over unique outer jumps
        for (auto it = function.outer_jumps.begin(),
                  end = function.outer_jumps.end();
             it != end;
             it = function.outer_jumps.upper_bound(it->first)) {
            create_function(it->second.dst);
        }
    }
}

#ifndef NDEBUG

void Restruc::debug(std::ostream &os)
{
    auto address = pe_.get_entry_point();
    create_function(address);
    dump_function(os, formatter_, functions_[address]);
}

void Restruc::dump_instruction(std::ostream &os,
                               DWORD va,
                               ZydisDecodedInstruction const &instruction)
{
    char buffer[256];
    ZYAN_THROW(ZydisFormatterFormatInstruction(&formatter_,
                                               &instruction,
                                               buffer,
                                               sizeof(buffer),
                                               va));
    os << std::hex << std::setfill('0') << std::setw(8) << va << "    "
       << buffer << '\n';
}

void Restruc::dump_function(std::ostream &os,
                            ZydisFormatter const &formatter,
                            CFGraph const &function)
{
    char buffer[256];
    os << std::hex << std::setfill('0');
    os << std::setw(8) << pe_.raw_to_virtual_address(function.entry_point)
       << ":\n";
    for (auto const &[address, instruction] : function.instructions) {
        auto va = pe_.raw_to_virtual_address(address);
        dump_instruction(os, va, instruction);
    }
    os << '\n';
}

#endif
