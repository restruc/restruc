#include "struc.hxx"

#include "utils/adapters.hxx"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>

using namespace rstc;

Struc::Field::Field(Type type,
                    size_t size,
                    size_t count,
                    class Struc const *struc)
    : struc_(struc)
    , size_(size)
    , count_(count)
    , type_(type)
{
}

bool Struc::Field::is_pointer_alias(size_t) const
{
    return size_ == 8 && (type_ == Int || type_ == UInt || type_ == Pointer);
}

bool Struc::Field::is_float_alias(size_t size) const
{
    if (size_ != size) {
        return false;
    }
    return type_ == Int || type_ == UInt || type_ == Float;
}

bool Struc::Field::is_typed_int_alias(size_t size) const
{
    if (size_ != size) {
        return false;
    }
    return type_ == Int || type_ == UInt || type_ == Float || type_ == Pointer;
}

std::string Struc::Field::type_to_string() const
{
    switch (type_) {
    case Struc::Field::UInt:
        switch (size_) {
        case 1: return "uint8_t";
        case 2: return "uint16_t";
        case 4: return "uint32_t";
        case 8: return "uint64_t";
        }
        break;
    case Struc::Field::Int:
        switch (size_) {
        case 1: return "int8_t";
        case 2: return "int16_t";
        case 4: return "int32_t";
        case 8: return "int64_t";
        }
        break;
    case Struc::Field::Float:
        switch (size_) {
        case 2: return "f16_t";
        case 4: return "float";
        case 8: return "double";
        case 10: return "long double";
        }
        break;
    case Struc::Field::Pointer:
        if (struc_) {
            return struc_->name() + "*";
        }
        else {
            return "void*";
        }
        break;
    case Struc::Field::Struc: return struc_->name(); break;
    }
    return "";
}

Struc::Struc(std::string name)
    : name_(std::move(name))
{
}

void Struc::add_int_field(size_t offset,
                          size_t size,
                          Field::Signedness signedness,
                          size_t count)
{
    // Is 1, 2, 4, 8, 16, 32, 64
    assert(size > 0 && (size & (size - 1)) == 0 && size <= 64);
    if (!has_aliases(offset, &Field::is_typed_int_alias, size)) {
        Field field(signedness == Field::Unsigned ? Field::UInt : Field::Int,
                    size,
                    count,
                    Atomic);
        add_field(offset, std::move(field));
    }
}

void Struc::add_float_field(size_t offset, size_t size, size_t count)
{
    assert(size == 2 || size == 4 || size == 8 || size == 10);
    size_t max_removed_count =
        remove_aliases(offset, &Field::is_float_alias, size);
    Field field(Field::Float, size, std::max(max_removed_count, count), Atomic);
    add_field(offset, std::move(field));
}

void Struc::add_pointer_field(size_t offset, size_t count, Struc const *struc)
{
    size_t max_removed_count =
        remove_aliases(offset, &Field::is_pointer_alias, 8);
    Field field(Field::Pointer, 8, std::max(max_removed_count, count), struc);
    add_field(offset, std::move(field));
}

void Struc::add_struc_field(size_t offset, Struc const *struc, size_t count)
{
    Field field(Field::Struc, 0, count, struc);
    add_field(offset, std::move(field));
}

void Struc::add_field(size_t offset, Field field)
{
    if (is_duplicate(offset, field)) {
        return;
    }
    {
        std::scoped_lock<std::recursive_mutex> modify_guard(mutex());
        for (size_t i = 0; i < field.count(); i++) {
            field_set_.insert(offset + i * field.size());
        }
        fields_.emplace(offset, std::move(field));
    }
}

bool Struc::is_duplicate(size_t offset, Field const &field) const
{
    if (fields_.empty()) {
        return false;
    }
    for (auto it = std::reverse_iterator(fields_.upper_bound(offset));
         it != fields_.rend();
         ++it) {
        auto const &current_field = it->second;
        auto current_offset = it->first;
        auto current_offset_end =
            current_offset + current_field.count() * current_field.size();
        if (current_offset_end <= offset) {
            break;
        }
        if (current_field.size() != field.size()) {
            continue;
        }
        // Alignment check
        if (current_offset % field.size() != offset % field.size()) {
            continue;
        }
        // TODO: use better type priorities
        switch (current_field.type()) {
        case Field::Type::UInt:
        case Field::Type::Int:
            if (field.is_typed_int_alias(current_field.size())
                && field.type() <= current_field.type()) {
                return true;
            }
            break;
        case Field::Type::Float:
            if (field.is_float_alias(current_field.size())
                && field.type() <= current_field.type()) {
                return true;
            }
            break;
        case Field::Type::Pointer:
            if (field.is_pointer_alias(current_field.size())
                && field.type() <= current_field.type()) {
                return true;
            }
            break;
        case Field::Type::Struc:
            if (field.type() == current_field.type()) {
                return true;
            }
            break;
        }
    }
    return false;
}

bool Struc::has_aliases(size_t offset,
                        bool (Field::*alias_check)(size_t size) const,
                        size_t size)
{
    for (auto it = fields_.find(offset);
         it != fields_.end() && it->first == offset;
         ++it) {
        if ((it->second.*alias_check)(size)) {
            return true;
        }
    }
    return false;
}

size_t Struc::remove_aliases(size_t offset,
                             bool (Field::*alias_check)(size_t) const,
                             size_t size)
{
    size_t count = 1;
    for (auto it = fields_.find(offset);
         it != fields_.end() && it->first == offset;) {
        count = std::max(count, it->second.count());
        if ((it->second.*alias_check)(size)) {
            it = fields_.erase(it);
        }
        else {
            ++it;
        }
    }
    return count;
}

void Struc::merge(Struc const &src, MergeCallback merge_callback)
{
    if (this == &src) {
        return;
    }
    {
        std::scoped_lock<std::recursive_mutex> modify_guard(src.mutex());
        for (auto const &[offset, field] : src.fields()) {
            if (!try_merge_struc_field_at_offset(offset,
                                                 field,
                                                 merge_callback)) {
                merge_fields(offset, field);
            }
        }
    }
    merge_callback(*this, src);
}

bool Struc::try_merge_struc_field_at_offset(size_t offset,
                                            Field const &src_field,
                                            MergeCallback merge_callback)
{
    if (src_field.type() != Struc::Field::Pointer || !src_field.struc()) {
        return false;
    }
    std::scoped_lock<std::recursive_mutex> modify_guard(mutex());
    bool merged = false;
    for (auto it = std::reverse_iterator(fields_.upper_bound(offset));
         it != fields_.rend();
         ++it) {
        auto &dst_field = it->second;
        auto dst_field_offset = it->first;
        auto dst_field_offset_end =
            dst_field_offset + dst_field.count() * dst_field.size();
        if (dst_field_offset_end <= offset) {
            break;
        }
        if (dst_field.type() != Struc::Field::Type::Pointer
            || !dst_field.struc() || dst_field_offset % 8 != offset % 8) {
            continue;
        }
        dst_field.struc()->merge(*src_field.struc(), merge_callback);
        merged = true;
    }
    return merged;
}

void Struc::merge_fields(size_t offset, Field const &field)
{
    if (!has_field_at_offset(offset)) {
        add_field(offset, field);
        return;
    }
    if (is_duplicate(offset, field)) {
        return;
    }
    if (field.type() == Field::Pointer && field.struc()) {
        add_pointer_field(offset, 1, field.struc());
    }
    else if (field.type() == Field::Float) {
        add_float_field(offset, field.size(), field.count());
    }
    else {
        add_field(offset, field);
    }
}

size_t Struc::get_size() const
{
    if (fields_.empty()) {
        return 0;
    }
    // TODO: fix the case when element before last
    // ends at offset larger than last element
    auto last_offset = fields_.rbegin()->first;
    auto last_fields = utils::multimap_values(fields_, last_offset);
    auto largest_last_field = std::max_element(
        last_fields.begin(),
        last_fields.end(),
        [](Field const &a, Field const &b) { return a.size() < b.size(); });
    return last_offset + largest_last_field->size();
}

bool Struc::has_field_at_offset(size_t offset)
{
    return field_set_.contains(offset);
}

void Struc::print(std::ostream &os) const
{
    auto os_flags = os.flags();
    os << std::setfill('0');
    os << "struct " << name_ << " {\n";
    size_t next_offset = 0;
    for (auto it = fields_.begin(); it != fields_.end();) {
        auto base_offset = it->first;
        if (base_offset > next_offset) {
            os << "    char _padding_" << std::hex << std::setw(4)
               << next_offset << "[0x" << std::hex << std::setw(4)
               << base_offset - next_offset << "];\n";
        }
        size_t union_field_count = 1;
        next_offset = base_offset + it->second.size() * it->second.count();
        auto it_end = std::next(it);
        while (it_end != fields_.end()) {
            auto prev = std::prev(it_end);
            auto offset =
                prev->first + prev->second.size() * prev->second.count();
            if (offset <= it_end->first) {
                break;
            }
            if (next_offset < offset) {
                next_offset = offset;
            }
            ++it_end;
            union_field_count++;
        }
        bool is_union = union_field_count > 1;
        std::string indent = "    ";
        if (is_union) {
            os << "    union {\n";
            indent += "    ";
        }
        for (size_t j = 1; j <= union_field_count; j++, ++it) {
            auto offset = it->first;
            auto const &field = it->second;
            if (offset == base_offset) {
                os << indent << field.type_to_string() << ' ' << "field_"
                   << std::hex << std::setw(4) << offset;
                if (is_union) {
                    os << "_" << std::dec << j;
                }
                if (field.count() > 1) {
                    os << '[' << std::dec << field.count() << ']';
                }
            }
            else {
                os << indent << "struct { char _padding[0x" << std::hex
                   << std::setw(4) << offset - base_offset << "]; "
                   << field.type_to_string() << " value";
                if (field.count() > 1) {
                    os << '[' << std::dec << field.count() << ']';
                }
                os << "; } field_" << std::hex << std::setw(4) << offset;
                if (is_union) {
                    os << "_" << std::dec << j;
                }
            }
            os << ";\n";
        }
        if (is_union) {
            os << "    };\n";
        }
    }
    os << "};\n";
    os.flags(os_flags);
}
