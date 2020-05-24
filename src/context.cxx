#include "contexts.hxx"

#include "utils/hash.hxx"

#include <algorithm>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <unordered_set>

using namespace rstc;

static std::atomic<size_t> GLOBAL_ID = 0;

Context::Context(Address source)
    : hash_(0)
    , id_(++GLOBAL_ID)
    , caller_id_(0)
    , registers_()
    , memory_(source)
{
    set_all_registers_symbolic(source);
}

Context::Context(Context const *parent, ParentRole parent_role)
    : hash_(parent->hash_)
    , id_(++GLOBAL_ID)
    , caller_id_(parent_role == ParentRole::Caller ? parent->id_ :
                                                     parent->caller_id_)
    , registers_(&parent->registers_)
    , memory_(&parent->memory_)
{
}

std::optional<virt::Value> Context::get_register(ZydisRegister reg) const
{
    return registers_.get(reg);
}

Context::MemoryValues Context::get_memory(uintptr_t address, size_t size) const
{
    return memory_.get(address, size);
}

void Context::set_register(ZydisRegister reg, virt::Value value)
{
    if (!registers_.is_tracked(reg)) {
        return;
    }
    if (auto old = get_register(reg); old) {
        utils::hash_combine(hash_, old->source());
        if (old->is_symbolic()) {
            utils::hash_combine(hash_, old->symbol().id());
        }
        else {
            utils::hash_combine(hash_, old->value());
        }
        // Don't "un"-hash `reg`,
        // as we will hash it only if old value didn't exist
    }
    else {
        utils::hash_combine(hash_, reg);
    }
    if (value.is_symbolic()) {
        utils::hash_combine(hash_, value.symbol().id());
    }
    else {
        utils::hash_combine(hash_, value.value());
    }
    utils::hash_combine(hash_, value.source());
    registers_.set(reg, value);
}

void Context::set_all_registers_symbolic(Address source)
{
    for (auto const &[zydis_reg, reg] : virt::Registers::register_map) {
        set_register(zydis_reg, virt::make_symbolic_value(source));
    }
}

void Context::set_memory(uintptr_t address, virt::Value value)
{
    memory_.set(address, value);
}

Context Context::make_child(ParentRole parent_role) const
{
    return Context(this, parent_role);
}
