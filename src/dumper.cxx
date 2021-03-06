#include "dumper.hxx"
#include "zyan_error.hxx"

using namespace rstc;

Dumper::Dumper()
{
    ZYAN_THROW(ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL));
}

void Dumper::dump_flo(std::ostream &os,
                      Flo const &flo,
                      DWORD entry_point_va) const
{
    auto flags = os.flags();
    os << std::hex << std::setfill('0');
    os << std::setw(8) << entry_point_va << ":\n";
    for (auto const &[address, instr] : flo.get_disassembly()) {
        dump_instruction(os,
                         static_cast<DWORD>(address - flo.entry_point)
                             + entry_point_va,
                         *instr);
    }
    os << '\n';
    os.flags(flags);
}

void Dumper::dump_instruction(std::ostream &os,
                              DWORD va,
                              ZydisDecodedInstruction const &instruction) const
{
    char buffer[256];
    auto flags = os.flags();
    ZYAN_THROW(ZydisFormatterFormatInstruction(&formatter_,
                                               &instruction,
                                               buffer,
                                               sizeof(buffer),
                                               va));
    os << std::hex << std::setfill('0') << std::setw(8) << std::right << va
       << "    " << buffer << '\n';
    os.flags(flags);
}

void Dumper::dump_value(std::ostream &os, virt::Value const &value) const
{
    auto flags = os.flags();
    if (!value.is_symbolic()) {
        os << ' ' << std::setfill('0') << std::hex << std::setw(16)
           << std::right << value.value() << "      ";
    }
    else {
        os << '[' << std::setfill('0') << std::hex << std::setw(16)
           << std::right << value.symbol().id() << '+' << std::hex
           << std::setw(4) << std::right << value.symbol().offset() << "]";
    }
    os.flags(flags);
}
