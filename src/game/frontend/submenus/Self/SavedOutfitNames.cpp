#include "SavedOutfitNames.hpp"
#include "core/filemgr/FileMgr.hpp"
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace YimMenu
{
namespace
{
	constexpr int kSlotCount = 20;
	constexpr std::uintptr_t kNameStride = 0x40;
	struct PartDef { const char* label; std::ptrdiff_t offset; bool prop; };
	constexpr std::array<PartDef, 16> kParts{{
	    {"Mask", -0x2310, false}, {"Hair", -0x2308, false}, {"Gloves", -0x2300, false},
	    {"Legs", -0x22F8, false}, {"Bags", -0x22F0, false}, {"Shoes", -0x22E8, false},
	    {"Accessories", -0x22E0, false}, {"Undershirt", -0x22D8, false},
	    {"Armor", -0x22D0, false}, {"Decals", -0x22C8, false}, {"Top", -0x22C0, false},
	    {"Hats", -0x11F8, true}, {"Glasses", -0x11F0, true}, {"Ears", -0x11E8, true},
	    {"Watches", -0x11C8, true}, {"Bracelets", -0x11C0, true}}};
	struct NameEditor
	{
		std::uintptr_t base = 0;
		int slot = -1;
		std::array<char, 64> name{};
		std::string original;
		std::array<int, kParts.size()> drawables{};
		std::array<int, kParts.size()> textures{};
		std::array<int, kParts.size()> originalDrawables{};
		std::array<int, kParts.size()> originalTextures{};
		bool partsLoaded = false;
		bool extended = false;
		bool pending = false;
		std::chrono::steady_clock::time_point due{};
		std::array<char, 512> importPath{};
		std::string message;
	};
	NameEditor g_Editor;
	std::string g_ResolveStatus;

	std::filesystem::path ExportFolder()
	{
		return FileMgr::GetProjectFolder("./saved_outfit_exports").Path();
	}

	std::optional<std::string> ReadName(std::uintptr_t address)
	{
		std::array<char, 64> bytes{};
		SIZE_T read = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), bytes.data(), bytes.size(), &read) || read != bytes.size()) return std::nullopt;
		const auto end = std::find(bytes.begin(), bytes.end(), '\0');
		if (end == bytes.end()) return std::nullopt;
		return std::string(bytes.begin(), end);
	}

	bool ValidArray(std::uintptr_t base)
	{
		if (!base) return false;
		// The external editor validates the first saved slot, not all 20. Empty
		// slots may contain non-name bytes, so checking them hides valid outfits.
		const auto name = ReadName(base);
		if (!name || name->empty()) return false;
		// Accept UTF-8 symbol bytes as well as ASCII: a saved slot may have a
		// name made entirely of GTA formatting symbols.
		const auto printable = std::count_if(name->begin(), name->end(), [](unsigned char ch) { return ch >= 0x20 && ch != 0x7f; });
		return printable >= static_cast<std::ptrdiff_t>(std::min<std::size_t>(4, name->size()));
	}

	std::uintptr_t ResolveDirectNameBase()
	{
		// Enhanced module RVAs -> outfit base, then +0x4D8 to slot 0's name.
		const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (!module) { g_ResolveStatus = "GTA executable module is unavailable."; return 0; }
		int readablePointers = 0;
		for (const std::uintptr_t rva : {0x043490B0u, 0x04404938u, 0x04404940u})
		{
			std::uintptr_t outfitBase = 0;
			SIZE_T read = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(module + rva), &outfitBase, sizeof(outfitBase), &read) || read != sizeof(outfitBase) || outfitBase < 0x10000) continue;
			++readablePointers;
			const auto namesBase = outfitBase + 0x4D8;
			if (namesBase < outfitBase) continue;
			if (ValidArray(namesBase)) { g_ResolveStatus.clear(); return namesBase; }
		}
		g_ResolveStatus = readablePointers ? "Saved-outfit pointers were readable, but slot 0 did not contain a valid name. The game layout may have changed." : "No saved-outfit pointer was readable for this GTA build.";
		return 0;
	}

	bool BaseValid()
	{
		return ValidArray(g_Editor.base);
	}

	std::uintptr_t PartAddress(int slot, std::size_t part, bool texture)
	{
		const auto& def = kParts[part];
		const auto offset = def.offset + static_cast<std::ptrdiff_t>(slot) * (def.prop ? 0x50 : 0x68) +
		    (texture ? (def.prop ? 0x698 : 0x890) : 0);
		return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(g_Editor.base) + offset);
	}

	bool ReadPartInt(std::uintptr_t address, int& value)
	{
		SIZE_T read = 0;
		return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(value), &read) && read == sizeof(value);
	}

	bool WritePartInt(std::uintptr_t address, int value)
	{
		MEMORY_BASIC_INFORMATION region{};
		if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
		    (region.Protect & PAGE_GUARD) || ((region.Protect & 0xff) != PAGE_READWRITE && (region.Protect & 0xff) != PAGE_EXECUTE_READWRITE) ||
		    reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize < address + sizeof(value)) return false;
		SIZE_T written = 0;
		return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &written) && written == sizeof(value);
	}

	void LoadParts()
	{
		auto& state = g_Editor;
		state.partsLoaded = false;
		for (std::size_t part = 0; part < kParts.size(); ++part)
		{
			int drawable = 0, texture = 0;
			if (!ReadPartInt(PartAddress(state.slot, part, false), drawable) ||
			    !ReadPartInt(PartAddress(state.slot, part, true), texture) ||
			    drawable < -999 || drawable > 4096 || texture < -999 || texture > 255)
			{ state.message = "Saved-part layout did not validate on this GTA build. Name editing remains available."; return; }
			state.drawables[part] = state.originalDrawables[part] = drawable;
			state.textures[part] = state.originalTextures[part] = texture;
		}
		state.partsLoaded = true;
	}

	void SelectSlot(int slot)
	{
		auto& state = g_Editor;
		if (!BaseValid()) { state.message = "Saved-outfit address changed. Reopen the GTA saved-outfits menu."; state.base = 0; return; }
		const auto current = ReadName(state.base + static_cast<std::uintptr_t>(slot) * kNameStride);
		if (!current || current->empty()) { state.message = "Save an outfit into this slot in GTA first."; return; }
		state.slot = slot;
		state.original = *current;
		std::snprintf(state.name.data(), state.name.size(), "%s", current->c_str());
		state.extended = current->size() > 31;
		state.pending = false;
		LoadParts();
	}

	void SaveName()
	{
		auto& state = g_Editor;
		state.pending = false;
		if (state.slot < 0 || !BaseValid()) { state.message = "Saved-outfit address changed. Reopen the GTA saved-outfits menu."; return; }
		const auto length = std::strlen(state.name.data());
		if (!length || length > (state.extended ? 63u : 31u) || std::strpbrk(state.name.data(), "\r\n"))
		{ state.message = "Name must be one line and 1-31 bytes (or 63 in experimental mode)."; return; }
		const auto address = state.base + static_cast<std::uintptr_t>(state.slot) * kNameStride;
		const auto current = ReadName(address);
		if (!current || *current != state.original) { state.message = "GTA changed this slot. Select it again before editing."; return; }
		if (*current == state.name.data()) return;
		const auto writeLength = length > 31 || current->size() > 31 ? 64u : 32u;
		MEMORY_BASIC_INFORMATION region{};
		if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
		    (region.Protect & PAGE_GUARD) || ((region.Protect & 0xff) != PAGE_READWRITE && (region.Protect & 0xff) != PAGE_EXECUTE_READWRITE) ||
		    reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize < address + writeLength)
		{ state.message = "GTA name memory is not safely writable."; return; }
		std::array<char, 64> before{}, after{}, verify{};
		SIZE_T count = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), before.data(), writeLength, &count) || count != writeLength)
		{ state.message = "Could not read the original name bytes."; return; }
		if (writeLength == 64 && !std::all_of(before.begin() + std::max<std::size_t>(32, current->size() + 1), before.end(), [](char ch) { return ch == 0; }))
		{ state.message = "Extended name blocked: the rest of this slot is in use."; return; }
		std::memcpy(after.data(), state.name.data(), length);
		if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), after.data(), writeLength, &count) || count != writeLength ||
		    !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), verify.data(), writeLength, &count) || count != writeLength ||
		    std::memcmp(after.data(), verify.data(), writeLength))
		{ WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), before.data(), writeLength, &count); state.message = "Name write failed; previous bytes were restored where possible."; return; }
		state.original = state.name.data();
		state.message = "Saved slot " + std::to_string(state.slot) + " name in live GTA memory. Confirm it in GTA; persistence after reload is unverified.";
	}

	void SaveParts()
	{
		auto& state = g_Editor;
		if (!state.partsLoaded || state.slot < 0 || !BaseValid()) return;
		struct Change { std::uintptr_t address; int before; int after; };
		std::vector<Change> changes;
		for (std::size_t part = 0; part < kParts.size(); ++part)
		{
			if (state.drawables[part] < -999 || state.drawables[part] > 4096 || state.textures[part] < -999 || state.textures[part] > 255)
			{ state.message = std::string("Out-of-range saved part: ") + kParts[part].label; return; }
			for (int texture = 0; texture < 2; ++texture)
			{
				const auto address = PartAddress(state.slot, part, texture != 0);
				const int before = texture ? state.originalTextures[part] : state.originalDrawables[part];
				const int after = texture ? state.textures[part] : state.drawables[part];
				int current = 0;
				if (!ReadPartInt(address, current) || current != before)
				{ state.message = "GTA changed saved outfit parts. Select the slot again before editing."; return; }
				if (after != before) changes.push_back({address, before, after});
			}
		}
		for (std::size_t i = 0; i < changes.size(); ++i)
		{
			int verify = 0;
			if (!WritePartInt(changes[i].address, changes[i].after) || !ReadPartInt(changes[i].address, verify) || verify != changes[i].after)
			{
				for (std::size_t j = i + 1; j > 0; --j) WritePartInt(changes[j - 1].address, changes[j - 1].before);
				state.message = "A saved-part write failed; previous values were restored where possible.";
				return;
			}
		}
		if (!changes.empty())
		{
			state.originalDrawables = state.drawables;
			state.originalTextures = state.textures;
			state.message = "Saved " + std::to_string(changes.size()) + " outfit part values to live GTA memory.";
		}
	}

	void SavePending()
	{
		g_Editor.pending = false;
		SaveName();
		SaveParts();
	}

	void ExportNames()
	{
		if (g_Editor.pending) SavePending();
		if (!BaseValid()) { g_Editor.message = "Find GTA saved outfits first."; return; }
		const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		const auto prefix = "Max1382-saved-outfit-names-" + std::to_string(stamp);
		const auto txtPath = ExportFolder() / (prefix + ".txt");
		const auto jsonPath = ExportFolder() / (prefix + ".json");
		std::ofstream file(txtPath, std::ios::trunc);
		std::ofstream jsonFile(jsonPath, std::ios::trunc);
		if (!file || !jsonFile) { g_Editor.message = "Could not create outfit-name exports."; return; }
		nlohmann::json data = {{"format", "Max1382 GTA saved outfits v2"}, {"slot_count", kSlotCount}, {"slots", nlohmann::json::array()}};
		for (int slot = 0; slot < kSlotCount; ++slot)
		{
			const auto name = ReadName(g_Editor.base + static_cast<std::uintptr_t>(slot) * kNameStride);
			if (!name) { g_Editor.message = "A slot became unreadable; export stopped."; return; }
			file << "Slot: " << slot << '\n' << "Name: " << *name << '\n';
			nlohmann::json entry = {{"slot", slot}, {"name", *name}, {"occupied", !name->empty()},
			    {"name_utf8_bytes", name->size()}, {"components", nlohmann::json::object()}, {"props", nlohmann::json::object()}};
			if (!name->empty())
				for (std::size_t part = 0; part < kParts.size(); ++part)
				{
					int drawable = 0, texture = 0;
					if (!ReadPartInt(PartAddress(slot, part, false), drawable) || !ReadPartInt(PartAddress(slot, part, true), texture))
					{ g_Editor.message = "A saved part became unreadable; export stopped."; return; }
					entry[kParts[part].prop ? "props" : "components"][kParts[part].label] = {{"drawable", drawable}, {"texture", texture}};
					file << kParts[part].label << ": " << drawable << "/" << texture << '\n';
				}
			file << '\n';
			data["slots"].push_back(std::move(entry));
		}
		jsonFile << data.dump(2) << '\n';
		g_Editor.message = file.good() && jsonFile.good() ? "Exported 20 GTA saved outfits to TXT and JSON in " + ExportFolder().string() : "Saved-outfit export failed while writing.";
	}

	void ExportSelectedOutfit()
	{
		if (g_Editor.pending) SavePending();
		if (g_Editor.slot < 0 || !BaseValid() || !g_Editor.partsLoaded)
		{ g_Editor.message = "Select a readable GTA saved outfit before exporting it."; return; }
		const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		const auto prefix = "Max1382-outfit-slot-" + std::to_string(g_Editor.slot) + "-" + std::to_string(stamp);
		const auto txtPath = ExportFolder() / (prefix + ".txt");
		const auto jsonPath = ExportFolder() / (prefix + ".json");
		std::ofstream txt(txtPath), jsonFile(jsonPath);
		if (!txt || !jsonFile) { g_Editor.message = "Could not create selected-outfit exports."; return; }
		nlohmann::json data = {{"format", "Max1382 GTA saved outfit v2"}, {"source_slot", g_Editor.slot},
		    {"name", g_Editor.original}, {"components", nlohmann::json::object()}, {"props", nlohmann::json::object()}};
		txt << "Slot: " << g_Editor.slot << '\n' << "Name: " << g_Editor.original << '\n';
		for (std::size_t part = 0; part < kParts.size(); ++part)
		{
			const auto& def = kParts[part];
			data[def.prop ? "props" : "components"][def.label] =
			    {{"drawable", g_Editor.originalDrawables[part]}, {"texture", g_Editor.originalTextures[part]}};
			txt << def.label << ": " << g_Editor.originalDrawables[part] << '/' << g_Editor.originalTextures[part] << '\n';
		}
		jsonFile << data.dump(2) << '\n';
		g_Editor.message = txt.good() && jsonFile.good() ? "Exported selected GTA outfit to " + jsonPath.string() : "Selected-outfit export failed while writing.";
	}

	void ImportName()
	{
		if (g_Editor.slot < 0) { g_Editor.message = "Choose a GTA saved-outfit slot first."; return; }
		std::ifstream file(g_Editor.importPath.data());
		if (!file) { g_Editor.message = "Could not open the name import file."; return; }
		std::string importedName;
		auto importedDrawables = g_Editor.drawables;
		auto importedTextures = g_Editor.textures;
		bool importedParts = false;
		const auto extension = std::filesystem::path(g_Editor.importPath.data()).extension().string();
		if (extension == ".json")
		{
			const auto data = nlohmann::json::parse(file, nullptr, false);
			if (!data.is_object()) { g_Editor.message = "Invalid JSON import."; return; }
			const nlohmann::json* selected = &data;
			if (data.contains("slots") && data["slots"].is_array())
			{
				selected = nullptr;
				for (const auto& entry : data["slots"])
					if (entry.is_object() && entry.value("slot", -1) == g_Editor.slot) { selected = &entry; break; }
			}
			if (selected && selected->value("name", nlohmann::json()).is_string()) importedName = (*selected)["name"].get<std::string>();
			if (selected && g_Editor.partsLoaded)
			{
				for (std::size_t part = 0; part < kParts.size(); ++part)
				{
					const auto& def = kParts[part];
					const auto section = def.prop ? "props" : "components";
					if (!selected->contains(section) || !(*selected)[section].is_object() || !(*selected)[section].contains(def.label)) continue;
					const auto& value = (*selected)[section][def.label];
					if (!value.is_object() || !value.value("drawable", nlohmann::json()).is_number_integer() ||
					    !value.value("texture", nlohmann::json()).is_number_integer())
					{ g_Editor.message = std::string("Invalid imported part: ") + def.label; return; }
					importedDrawables[part] = value["drawable"].get<int>();
					importedTextures[part] = value["texture"].get<int>();
					if (importedDrawables[part] < -999 || importedDrawables[part] > 4096 || importedTextures[part] < -999 || importedTextures[part] > 255)
					{ g_Editor.message = std::string("Imported part is out of range: ") + def.label; return; }
					importedParts = true;
				}
			}
			if (importedName.empty() && data.value("name", nlohmann::json()).is_string()) importedName = data["name"].get<std::string>();
		}
		else
		{
			int blockSlot = -1;
			bool sawSlotField = false;
			std::string blockName, singleName, line;
			while (std::getline(file, line))
			{
				if (line.find_first_not_of(" \t\r\n") == std::string::npos)
				{
					if (blockSlot == g_Editor.slot && !blockName.empty()) { importedName = blockName; break; }
					blockSlot = -1;
					blockName.clear();
					continue;
				}
				const auto equal = line.find_first_of("=:");
				if (equal == std::string::npos) continue;
				std::string key = line.substr(0, equal), value = line.substr(equal + 1);
				auto trim = [](std::string& text) { const auto first = text.find_first_not_of(" \t\r\n"); if (first == std::string::npos) { text.clear(); return; } const auto last = text.find_last_not_of(" \t\r\n"); text = text.substr(first, last - first + 1); };
				trim(key); trim(value);
				if (key == std::to_string(g_Editor.slot)) { importedName = value; break; }
				if (key == "Slot" || key == "slot") { sawSlotField = true; try { blockSlot = std::stoi(value); } catch (...) { blockSlot = -1; } }
				if (key == "Name" || key == "name") { blockName = value; if (singleName.empty()) singleName = value; }
			}
			if (importedName.empty() && blockSlot == g_Editor.slot && !blockName.empty()) importedName = blockName;
			if (importedName.empty() && !sawSlotField) importedName = singleName;
		}
		if (importedName.empty() && !importedParts) { g_Editor.message = "No outfit data for the selected slot found in the file."; return; }
		if (importedName.size() > (g_Editor.extended ? 63u : 31u) || importedName.find_first_of("\r\n") != std::string::npos)
		{ g_Editor.message = "Imported name exceeds the enabled byte limit or contains a newline."; return; }
		if (!importedName.empty()) std::snprintf(g_Editor.name.data(), g_Editor.name.size(), "%s", importedName.c_str());
		if (importedParts) { g_Editor.drawables = importedDrawables; g_Editor.textures = importedTextures; }
		g_Editor.pending = true;
		g_Editor.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
		g_Editor.message = "Imported selected outfit data; saving automatically.";
	}

	void AppendNamePreset(const char* value)
	{
		auto& state = g_Editor;
		const auto current = std::strlen(state.name.data());
		const auto added = std::strlen(value);
		if (current + added > (state.extended ? 63u : 31u))
		{ state.message = "That preset exceeds this saved-outfit name's byte limit."; return; }
		std::memcpy(state.name.data() + current, value, added + 1);
		state.pending = true;
		state.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
	}

	std::optional<ImVec4> PreviewColour(std::string_view tag)
	{
		if (tag == "r") return ImVec4(0.95f, 0.27f, 0.27f, 1.0f);
		if (tag == "g") return ImVec4(0.30f, 0.85f, 0.40f, 1.0f);
		if (tag == "b") return ImVec4(0.35f, 0.62f, 1.00f, 1.0f);
		if (tag == "d") return ImVec4(0.22f, 0.38f, 0.73f, 1.0f);
		if (tag == "y") return ImVec4(1.00f, 0.86f, 0.23f, 1.0f);
		if (tag == "o") return ImVec4(1.00f, 0.57f, 0.20f, 1.0f);
		if (tag == "q") return ImVec4(1.00f, 0.52f, 0.78f, 1.0f);
		if (tag == "p") return ImVec4(0.72f, 0.48f, 0.95f, 1.0f);
		if (tag == "c") return ImVec4(0.72f, 0.74f, 0.77f, 1.0f);
		if (tag == "m") return ImVec4(0.47f, 0.49f, 0.53f, 1.0f);
		if (tag == "w") return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
		return std::nullopt;
	}

	std::string VisibleName(std::string_view raw)
	{
		std::string visible;
		for (std::size_t i = 0; i < raw.size();)
		{
			if (raw[i] == '~')
			{
				const auto end = raw.find('~', i + 1);
				if (end != std::string_view::npos && end - i <= 12)
				{
					const auto tag = raw.substr(i + 1, end - i - 1);
					if (tag == "ws") visible += "\xE2\x98\x85";
					if (PreviewColour(tag) || tag == "s" || tag == "h" || tag == "bold" || tag == "italic" || tag == "ws" || tag.substr(0, 3) == "HC_") { i = end + 1; continue; }
				}
			}
			if (raw.substr(i, 5) == "<font")
			{
				const auto end = raw.find('>', i + 5);
				if (end != std::string_view::npos) { i = end + 1; continue; }
			}
			visible.push_back(raw[i++]);
		}
		return visible.empty() ? "Empty" : visible;
	}

	void DrawNamePreview(std::string_view raw)
	{
		ImGui::TextUnformatted("Preview:");
		ImGui::SameLine();
		ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		std::string chunk;
		bool first = true;
		const auto flush = [&]() {
			if (chunk.empty()) return;
			if (!first) ImGui::SameLine(0.0f, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, colour);
			ImGui::TextUnformatted(chunk.c_str());
			ImGui::PopStyleColor();
			chunk.clear();
			first = false;
		};
		for (std::size_t i = 0; i < raw.size();)
		{
			if (raw[i] == '~')
			{
				const auto end = raw.find('~', i + 1);
				if (end != std::string_view::npos && end - i <= 12)
				{
					const auto tag = raw.substr(i + 1, end - i - 1);
					if (const auto next = PreviewColour(tag)) { flush(); colour = *next; i = end + 1; continue; }
					if (tag == "s") { flush(); colour = ImGui::GetStyleColorVec4(ImGuiCol_Text); i = end + 1; continue; }
					if (tag == "ws") { chunk += "\xE2\x98\x85"; i = end + 1; continue; }
					if (tag == "h" || tag == "bold" || tag == "italic" || tag.substr(0, 3) == "HC_") { i = end + 1; continue; }
				}
			}
			if (raw.substr(i, 5) == "<font")
			{
				const auto end = raw.find('>', i + 5);
				if (end != std::string_view::npos) { i = end + 1; continue; }
			}
			chunk.push_back(raw[i++]);
		}
		flush();
		if (first) ImGui::TextDisabled("(empty)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Menu preview: GTA may render its special fonts and icons differently.");
	}

	void DrawNamePresets()
	{
		struct Preset { const char* label; const char* value; };
		constexpr Preset symbols[] = {{"\xC2\xB1  Plus/minus", "\xC2\xB1"}, {"\xC2\xA9  Copyright", "\xC2\xA9"},
		    {"\xC2\xA4  Invisible currency sign", "\xC2\xA4"}, {"\xE2\x88\x91  Rockstar", "\xE2\x88\x91"},
		    {"\xC2\xA6  Verified", "\xC2\xA6"}, {"\xCE\xA9  Lock", "\xCE\xA9"},
		    {"\xE2\x80\xB9  Left icon", "\xE2\x80\xB9"}, {"\xE2\x80\xBA  Right icon", "\xE2\x80\xBA"},
		    {"\xE2\x86\x90  Left arrow", "\xE2\x86\x90"}, {"\xE2\x86\x91  Up arrow", "\xE2\x86\x91"},
		    {"\xE2\x86\x92  Right arrow", "\xE2\x86\x92"}, {"\xE2\x86\x93  Down arrow", "\xE2\x86\x93"},
		    {"\xE2\x88\x9E  Infinity", "\xE2\x88\x9E"}, {"\xC3\xB7  Division", "\xC3\xB7"}, {"\xE2\x98\x85  Wanted star", "~ws~"}};
		constexpr Preset colors[] = {{"Red", "~r~"}, {"Green", "~g~"}, {"Blue", "~b~"},
		    {"Dark blue", "~d~"}, {"Yellow", "~y~"}, {"Orange", "~o~"},
		    {"Pink", "~q~"}, {"Purple", "~p~"}, {"Grey", "~c~"},
		    {"Dark grey", "~m~"}, {"White", "~w~"}, {"Reset", "~s~"}};
		constexpr Preset fonts[] = {{"GTA leaderboard", "<font face='$GTAVLeaderboard'>"},
		    {"GTA cash", "<font face='$gtaCash'>"}, {"Rockstar tag", "<font face='$RockstarTAG'>"},
		    {"Fixed-width numbers", "<font face='$FixedWidthNumbers'>"},
		    {"Handstyle heist", "<font face='$HandstyleHeist'>"}, {"Font 2", "<font face='$Font2'>"},
		    {"Font 2 condensed", "<font face='$Font2_cond'>"},
		    {"Font 2 condensed no gamer name", "<font face='$Font2_cond_NOT_GAMERNAME'>"},
		    {"Font 2 MP timer", "<font face='$Font2_cond_MPTimer'>"}, {"Font 5", "<font face='$Font5'>"}};
		if (ImGui::BeginCombo("Symbols", "Insert symbol"))
		{
			for (const auto& preset : symbols) if (ImGui::Selectable(preset.label)) AppendNamePreset(preset.value);
			ImGui::EndCombo();
		}
		if (ImGui::BeginCombo("Colours", "Insert colour code"))
		{
			for (const auto& preset : colors)
			{
				const std::string_view code(preset.value);
				const auto colour = PreviewColour(code.size() >= 3 ? code.substr(1, code.size() - 2) : std::string_view{});
				if (colour) ImGui::PushStyleColor(ImGuiCol_Text, *colour);
				if (ImGui::Selectable(preset.label)) AppendNamePreset(preset.value);
				if (colour) ImGui::PopStyleColor();
			}
			ImGui::EndCombo();
		}
		static int hudColourId = 0;
		ImGui::SetNextItemWidth(105);
		ImGui::InputInt("HUD colour ID", &hudColourId, 0, 0);
		ImGui::SameLine();
		if (ImGui::Button("Insert HUD colour"))
		{
			if (hudColourId >= 0 && hudColourId <= 255)
			{
				const auto code = "~HC_" + std::to_string(hudColourId) + "~";
				AppendNamePreset(code.c_str());
			}
			else g_Editor.message = "HUD colour ID must be 0-255.";
		}
		if (ImGui::BeginCombo("Fonts", "Insert font tag"))
		{
			for (const auto& preset : fonts) if (ImGui::Selectable(preset.label)) AppendNamePreset(preset.value);
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("The currency sign may render invisible in GTA. Long font tags may exceed the name limit.");
	}
}

void DrawMax1382SavedOutfits()
{
	auto& state = g_Editor;
	if (!BaseValid())
	{
		state.base = ResolveDirectNameBase();
		if (!state.base) state.slot = -1;
	}
	if (state.pending && std::chrono::steady_clock::now() >= state.due) SavePending();
	ImGui::Separator();
	ImGui::TextColored(ImVec4(0.43f, 0.82f, 0.53f, 1.0f), "Saved Outfits - made by Max1382.com");
	ImGui::TextDisabled("Direct GTA saved slots 0-19; names auto-save after a short pause.");
	if (BaseValid())
	{
		const auto currentName = state.slot >= 0 ? ReadName(state.base + static_cast<std::uintptr_t>(state.slot) * kNameStride) : std::nullopt;
		const std::string selected = state.slot >= 0 ? std::to_string(state.slot) + "  " + (currentName ? VisibleName(*currentName) : "Empty") : "Choose a saved outfit";
		ImGui::SetNextItemWidth(std::min(480.0f, ImGui::GetContentRegionAvail().x));
		if (ImGui::BeginCombo("Saved outfit", selected.c_str()))
		{
			for (int slot = 0; slot < kSlotCount; ++slot)
			{
				const auto name = ReadName(state.base + static_cast<std::uintptr_t>(slot) * kNameStride);
				const std::string label = std::to_string(slot) + "  " + (name ? VisibleName(*name) : "Empty") + "##saved" + std::to_string(slot);
				if (ImGui::Selectable(label.c_str(), state.slot == slot))
				{
					if (state.pending) SavePending();
					if (!state.pending) SelectSlot(slot);
				}
			}
			ImGui::EndCombo();
		}
		if (state.slot >= 0)
		{
			if (ImGui::InputText("Saved outfit name", state.name.data(), state.name.size()))
			{ state.pending = true; state.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(650); }
			ImGui::TextDisabled("%zu / %d bytes; saves after a short pause", std::strlen(state.name.data()), state.extended ? 63 : 31);
			DrawNamePreview(state.name.data());
			ImGui::Checkbox("Experimental 32-63 byte name", &state.extended);
			DrawNamePresets();
			if (state.partsLoaded && ImGui::CollapsingHeader("Saved outfit parts", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::BeginTable("saved-outfit-parts", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch, 1.7f);
					ImGui::TableSetupColumn("Drawable", ImGuiTableColumnFlags_WidthStretch, 1.0f);
					ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch, 1.0f);
					ImGui::TableSetupColumn("Part##right", ImGuiTableColumnFlags_WidthStretch, 1.7f);
					ImGui::TableSetupColumn("Drawable##right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
					ImGui::TableSetupColumn("Texture##right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
					ImGui::TableHeadersRow();
					const auto drawPart = [&](std::size_t part, int firstColumn) {
						ImGui::PushID(static_cast<int>(part) + 20000);
						ImGui::TableSetColumnIndex(firstColumn);
						ImGui::TextUnformatted(kParts[part].label);
						ImGui::TableSetColumnIndex(firstColumn + 1);
						ImGui::SetNextItemWidth(-1);
						if (ImGui::InputInt("##drawable", &state.drawables[part], 0, 0))
						{ state.pending = true; state.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(650); }
						ImGui::TableSetColumnIndex(firstColumn + 2);
						ImGui::SetNextItemWidth(-1);
						if (ImGui::InputInt("##texture", &state.textures[part], 0, 0))
						{ state.pending = true; state.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(650); }
						ImGui::PopID();
					};
					for (std::size_t row = 0; row < kParts.size() / 2; ++row)
					{
						ImGui::TableNextRow();
						drawPart(row, 0);
						drawPart(row + kParts.size() / 2, 3);
					}
					ImGui::EndTable();
				}
			}
			if (ImGui::TreeNode("Import / export saved names"))
			{
				ImGui::InputText("Import .txt or .json path", state.importPath.data(), state.importPath.size());
				if (ImGui::Button("Import into selected slot")) ImportName();
				if (ImGui::Button("Export selected outfit")) ExportSelectedOutfit();
				if (ImGui::Button("Export all 20 outfits as TXT + JSON")) ExportNames();
				ImGui::TextDisabled("JSON imports names and parts; TXT imports names. Exports are timestamped in YimMenuV2/saved_outfit_exports.");
				ImGui::TreePop();
			}
		}
	}
	else
	{
		ImGui::TextWrapped("Saved GTA outfits are not available yet. This editor reads the game's live saved slots, not local outfit files.");
		if (!g_ResolveStatus.empty()) ImGui::TextWrapped("Lookup: %s", g_ResolveStatus.c_str());
	}
	if (!state.message.empty()) ImGui::TextWrapped("%s", state.message.c_str());
}
}
