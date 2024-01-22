#include "file.h"
#include "pe.h"
#include "coff.h"
#include "dotnet.h"
#include "utils.h"

namespace pe
{
	// format

	bool format::check(base::stream &stream) const
	{
		stream.seek(0);
		return (stream.read<uint16_t>() == dos_signature);
	}

	std::unique_ptr<base::file> format::instance() const
	{
		return std::make_unique<file>();
	}

	// file

	base::status file::load()
	{
		auto &pe = add<architecture>(this, 0, size());
		base::status status = pe.load();
		if (status == base::status::success) {
			auto dir = pe.commands()->find_type(format::directory_id::com_descriptor);
			if (dir && dir->address()) {
				auto &net = add<net::architecture>(pe);
				status = net.load();
			}
		}
		return status;
	}

	// directory

	directory::directory(directory_list *owner, format::directory_id type)
		: base::load_command(owner), type_(type)
	{
		address_ = 0;
		size_ = 0;
	}

	std::string directory::name() const
	{
		switch (type_) {
		case format::directory_id::exports: return "Export";
		case format::directory_id::import: return "Import";
		case format::directory_id::resource: return "Resource";
		case format::directory_id::exception: return "Exception";
		case format::directory_id::security: return "Security";
		case format::directory_id::basereloc: return "Relocation";
		case format::directory_id::debug: return "Debug";
		case format::directory_id::architecture: return "Architecture";
		case format::directory_id::globalptr: return "GlobalPtr";
		case format::directory_id::tls: return "Thread Local Storage";
		case format::directory_id::load_config: return "Load Config";
		case format::directory_id::bound_import: return "Bound Import";
		case format::directory_id::iat: return "Import Address Table";
		case format::directory_id::delay_import: return "Delay Import";
		case format::directory_id::com_descriptor: return ".NET MetaData";
		}
		return base::load_command::name();
	}

	void directory::load(architecture &file)
	{
		auto data = file.read<format::data_directory_t>();
		address_ = data.rva ? data.rva + file.image_base() : 0;
		size_ = data.size;
	}

	// directory_list

	void directory_list::load(architecture &file, size_t count)
	{
		for (size_t type = 0; type < count; type++) {
			auto &item = add(static_cast<format::directory_id>(type));
			item.load(file);
			if (!item.address() && !item.size())
				pop();
		}
	}

	// segment

	void segment::load(architecture &file, coff::string_table *table)
	{
		auto header = file.read<format::section_header_t>();
		address_ = header.virtual_address + file.image_base();
		size_ = header.virtual_size;
		physical_offset_ = header.ptr_raw_data;
		physical_size_ = header.size_raw_data;
		characteristics_ = header.characteristics;
		name_ = header.name.to_string(table);
	}

	base::memory_type_t segment::memory_type() const
	{
		base::memory_type_t res{};
		res.read = characteristics_.mem_read;
		res.write = characteristics_.mem_write;
		res.execute = characteristics_.mem_execute;
		res.discardable = characteristics_.mem_discardable;
		res.not_cached = characteristics_.mem_not_cached;
		res.not_paged = characteristics_.mem_not_paged;
		res.shared = characteristics_.mem_shared;
		res.mapped = true;
		return res;
	}

	// segment_list

	void segment_list::load(architecture &file, size_t count, coff::string_table *table)
	{
		for (size_t index = 0; index < count; ++index) {
			add().load(file, table);
		}
	}

	// import_function

	import_function::import_function(import *owner, uint64_t address)
		: base::import_function(owner), address_(address)
	{

	}

	bool import_function::load(architecture &file)
	{
		uint64_t value = 0;
		if (file.address_size() == base::operand_size::dword) {
			auto header = file.read<format::image_thunk_data_32_t>();
			if (!header.address)
				return false;

			is_ordinal_ = header.is_ordinal;
			value = is_ordinal_ ? header.ordinal : header.address;
		}
		else {
			auto header = file.read<format::image_thunk_data_64_t>();
			if (!header.address)
				return false;

			is_ordinal_ = header.is_ordinal;
			value = is_ordinal_ ? header.ordinal : header.address;
		}

		if (is_ordinal_) {
			ordinal_ = (uint32_t)value;
			name_ = utils::format("Ordinal: %.4X", ordinal_);
		} else {
			auto position = file.tell();
			if (!file.seek_address(value + file.image_base() + sizeof(uint16_t)))
				throw std::runtime_error("Format error");
			name_ = file.read_string();
			file.seek(position);
		}

		return true;
	}

	// import

	bool import::load(architecture &file)
	{
		auto header = file.read<format::import_directory_t>();
		if (!header.rva_first_thunk)
			return false;

		auto position = file.tell();
		if (!file.seek_address(header.rva_name + file.image_base()))
			throw std::runtime_error("Format error");

		name_ = file.read_string();

		if (!file.seek_address((header.rva_original_first_thunk ? header.rva_original_first_thunk : header.rva_first_thunk) + file.image_base()))
			throw std::runtime_error("Format error");

		uint64_t address = header.rva_first_thunk + file.image_base();
		while (true) {
			if (!add(address).load(file)) {
				pop();
				break;
			}
			address += (file.address_size() == base::operand_size::dword) ? sizeof(uint32_t) : sizeof(uint64_t);
		}

		file.seek(position);
		return true;
	}

	// import_list

	void import_list::load(architecture &file)
	{
		auto dir = file.commands()->find_type(format::directory_id::import);
		if (!dir)
			return;

		if (!file.seek_address(dir->address()))
			throw std::runtime_error("Format error");

		while (true) {
			if (!add().load(file)) {
				pop();
				return;
			}
		}
	}

	// export_symbol

	void export_symbol::load(architecture &file, uint64_t name_address, bool is_forwarded)
	{
		if (name_address) {
			if (!file.seek_address(name_address))
				throw std::runtime_error("Format error");
			name_ = file.read_string();
		}

		if (is_forwarded) {
			if (!file.seek_address(address_))
				throw std::runtime_error("Format error");
			forwarded_ = file.read_string();
		}
	}

	// export_list

	void export_list::load(architecture &file)
	{
		auto dir = file.commands()->find_type(format::directory_id::exports);
		if (!dir)
			return;

		if (!file.seek_address(dir->address()))
			throw std::runtime_error("Format error");

		auto header = file.read<format::export_directory_t>();
		if (!header.num_functions)
			return;

		std::map<uint32_t, uint32_t> name_map;
		if (header.num_names) {
			if (!file.seek_address(header.rva_names + file.image_base()))
				throw std::runtime_error("Format error");

			std::vector<uint32_t> rva_names;
			rva_names.resize(header.num_names);
			for (size_t i = 0; i < header.num_names; i++) {
				rva_names[i] = file.read<uint32_t>();
			}

			if (!file.seek_address(header.rva_name_ordinals + file.image_base()))
				throw std::runtime_error("Format error");

			for (size_t i = 0; i < header.num_names; i++) {
				name_map[header.base + file.read<uint16_t>()] = rva_names[i];
			}
		}

		if (!file.seek_address(header.rva_functions + file.image_base()))
			throw std::runtime_error("Format error");

		for (uint32_t index = 0; index < header.num_functions; index++) {
			if (uint32_t rva = file.read<uint32_t>()) {
				add(rva + file.image_base(), header.base + index);
			}
		}

		for (auto &item : *this) {
			auto it = name_map.find(item.ordinal());
			item.load(file, (it != name_map.end()) ? it->second + file.image_base() : 0, (item.address() >= dir->address() && item.address() < dir->address() + dir->size()));
		}
	}
	
	// architecture

	architecture::architecture(file *owner, uint64_t offset, uint64_t size)
		: base::architecture(owner, offset, size)
	{
		machine_ = format::machine_id::unknown;
		image_base_ = 0;
		entry_point_ = 0;
		address_size_ = base::operand_size::dword;
		subsystem_ = format::subsystem_id::unknown;
		directory_list_ = std::make_unique<directory_list>(this);
		segment_list_ = std::make_unique<pe::segment_list>(this);
		import_list_ = std::make_unique<pe::import_list>(this);
		export_list_ = std::make_unique<pe::export_list>();
		symbol_list_ = std::make_unique<pe::symbol_list>();
	}

	std::string architecture::name() const
	{
		switch (machine_) {
		case format::machine_id::i386: return "i386";
		case format::machine_id::r3000:
		case format::machine_id::r4000:
		case format::machine_id::r10000:
		case format::machine_id::mips16:
		case format::machine_id::mipsfpu:
		case format::machine_id::mipsfpu16: return "mips";
		case format::machine_id::wcemipsv2: return "mips_wce_v2";
		case format::machine_id::alpha: return "alpha_axp";
		case format::machine_id::sh3: return "sh3";
		case format::machine_id::sh3dsp: return "sh3dsp";
		case format::machine_id::sh3e: return "sh3e";
		case format::machine_id::sh4: return "sh4";
		case format::machine_id::sh5: return "sh5";
		case format::machine_id::arm: return "arm";
		case format::machine_id::thumb: return "thumb";
		case format::machine_id::am33: return "am33";
		case format::machine_id::powerpc:
		case format::machine_id::powerpcfp: return "ppc";
		case format::machine_id::ia64: return "ia64";
		case format::machine_id::alpha64: return "alpha64";
		case format::machine_id::tricore: return "infineon";
		case format::machine_id::cef: return "cef";
		case format::machine_id::ebc: return "ebc";
		case format::machine_id::amd64: return "amd64";
		case format::machine_id::m32r: return "m32r";
		case format::machine_id::cee: return "cee";
		case format::machine_id::arm64: return "arm64";
		}
		return utils::format("unknown 0x%X", machine_);
	}

	base::status architecture::load()
	{
		seek(0);

		auto dos_header = read<format::dos_header_t>();
		if (dos_header.e_magic != format::dos_signature)
			return base::status::unknown_format;

		seek(dos_header.e_lfanew);
		if (read<uint32_t>() != format::nt_signature)
			return base::status::unknown_format;

		size_t num_data_directories;
		auto file_header = read<format::file_header_t>();
		switch (file_header.machine) {
		case format::machine_id::i386:
			{
				auto optional = read<format::optional_header_32_t>();
				if (optional.magic != format::hdr32_magic)
					throw std::runtime_error("Format error");
				image_base_ = optional.image_base;
				entry_point_ = optional.entry_point ? optional.entry_point + image_base_ : 0;
				subsystem_ = optional.subsystem;
				address_size_ = base::operand_size::dword;
				num_data_directories = optional.num_data_directories;
			}
			break;
		case format::machine_id::amd64:
			{
				auto optional = read<format::optional_header_64_t>();
				if (optional.magic != format::hdr64_magic)
					throw std::runtime_error("Format error");
				image_base_ = optional.image_base;
				entry_point_ = optional.entry_point ? optional.entry_point + image_base_ : 0;
				subsystem_ = optional.subsystem;
				address_size_ = base::operand_size::qword;
				num_data_directories = optional.num_data_directories;
			}
			break;
		default:
			return base::status::unsupported_cpu;
		}

		switch (subsystem_) {
		case format::subsystem_id::native:
		case format::subsystem_id::windows_gui:
		case format::subsystem_id::windows_cui:
			break;
		default:
			return base::status::unsupported_subsystem;
		}

		directory_list_->load(*this, num_data_directories);

		machine_ = file_header.machine;

		coff::string_table string_table;
		if (file_header.ptr_symbols) {
			seek(file_header.ptr_symbols + file_header.num_symbols * sizeof(coff::format::symbol_t));
			string_table.load(*this);
		}
		seek(dos_header.e_lfanew + sizeof(uint32_t) + sizeof(format::file_header_t) + file_header.size_optional_header);
		segment_list_->load(*this, file_header.num_sections, &string_table);

		import_list_->load(*this);
		export_list_->load(*this);

		if (file_header.ptr_symbols) {
			seek(file_header.ptr_symbols);
			for (size_t i = 0; i < file_header.num_symbols; i++) {
				auto symbol = read<coff::format::symbol_t>();
				switch (symbol.storage_class) {
				case coff::format::storage_class_id::public_symbol:
				case coff::format::storage_class_id::private_symbol:
					if (symbol.section_index == 0 || symbol.section_index >= segment_list_->size())
						continue;

					symbol_list_->add(segment_list_->item(symbol.section_index - 1).address() + symbol.value, symbol.name.to_string(&string_table),
						(symbol.derived_type == coff::format::derived_type_id::function) ? base::symbol_type_id::function : base::symbol_type_id::data);
					break;
				}
			}
		}

		return base::status::success;
	}
};