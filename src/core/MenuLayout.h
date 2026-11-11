#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>

// UNOFFICIAL 2026-11-10 (user: "our folder system with the shaders is the default, but our users can make, rearrange, or delete folders
// if they want and their arrangement is saved"): the settings menu's effects list as folders the player arranges. Only where an effect's
// row SHOWS in the list: every effect keeps its section, keys, switch and pages. The arrangement lives in its own small file beside the
// settings file (VegasShaders.menu.toml), not in the settings file, so a shared settings file or preset never carries someone's folders.
// No file = the built-in folders (ImGuiManager.cpp kEffectGroups). The file is written on the first change and removed by "reset".
//
//   [[Folder]]
//   Name = "Lighting"
//   Effects = ["AmbientLight", "PBR", "@RayTracing"]
//
// An entry is an effect's name (the X of Shaders.X) or one of the list's two fixed pages: "@RayTracing" (the Ray tracing pages) and
// "@GunFX". An effect the file does not name (a new one in a later version) is put into its built-in folder by the menu, so it never
// goes missing; a name this version does not know stays in the file and is not drawn.
namespace MenuLayout {

	struct Folder { std::string name; std::vector<std::string> entries; };

	struct State {
		std::vector<Folder> folders;	// in the list's order
		bool        custom = false;		// the player's own (read from / written to the file); false: the built-in folders
		bool        loaded = false;
		std::string path;				// the file
	};
	inline State S;

	// A folder's name as typed: no characters that would break the file's line or the menu's ids, trimmed, at most 40 characters.
	inline std::string Clean(const std::string& typed) {
		std::string out;
		for (const char c : typed)
			if ((unsigned char)c >= 32 && c != '"' && c != '\\' && c != '#' && c != '[' && c != ']') out += c;
		while (!out.empty() && out.front() == ' ') out.erase(out.begin());
		while (!out.empty() && out.back() == ' ') out.pop_back();
		if (out.size() > 40) out.resize(40);
		return out;
	}

	inline Folder* Find(const std::string& name) {
		for (Folder& f : S.folders) if (f.name == name) return &f;
		return nullptr;
	}

	inline Folder* FolderOf(const std::string& entry) {
		for (Folder& f : S.folders)
			for (const std::string& e : f.entries) if (e == entry) return &f;
		return nullptr;
	}

	inline bool Save() {
		std::ofstream out(S.path, std::ios::trunc);
		if (!out) return false;
		out << "# Vegas Shaders: your own folders in the settings menu's effects list (right-click a folder or an effect in the menu).\n"
			   "# Only where each effect shows in the list: no setting is stored here. Delete this file to get the built-in folders back.\n";
		for (const Folder& f : S.folders) {
			out << "\n[[Folder]]\nName = \"" << f.name << "\"\nEffects = [";
			for (size_t i = 0; i < f.entries.size(); i++) out << (i ? ", " : "") << '"' << f.entries[i] << '"';
			out << "]\n";
		}
		return out.good();
	}

	// Reads the file. False (and no folders) when there is none or it holds no folder: the caller then uses the built-in ones.
	inline bool Load() {
		S.folders.clear();
		S.custom = false;
		std::ifstream in(S.path);
		if (!in) return false;
		auto quoted = [](const std::string& line, size_t from, std::vector<std::string>& out) {
			for (size_t a = line.find('"', from); a != std::string::npos; ) {
				const size_t b = line.find('"', a + 1);
				if (b == std::string::npos) break;
				out.push_back(line.substr(a + 1, b - a - 1));
				a = line.find('"', b + 1);
			}
		};
		std::string line;
		while (std::getline(in, line)) {
			const size_t start = line.find_first_not_of(" \t");
			if (start == std::string::npos || line[start] == '#') continue;
			if (line.compare(start, 10, "[[Folder]]") == 0) { S.folders.push_back({}); continue; }
			if (S.folders.empty()) continue;
			const size_t eq = line.find('=', start);
			if (eq == std::string::npos) continue;
			std::vector<std::string> values;
			quoted(line, eq, values);
			if (line.compare(start, 4, "Name") == 0 && !values.empty()) S.folders.back().name = Clean(values[0]);
			else if (line.compare(start, 7, "Effects") == 0) {
				for (const std::string& v : values) {
					const std::string e = Clean(v);
					if (!e.empty() && !FolderOf(e)) S.folders.back().entries.push_back(e);	// (an effect is in one folder: the first that names it)
				}
			}
		}
		// folders without a name, or with one already taken, get one
		for (size_t i = 0; i < S.folders.size(); i++) {
			Folder& f = S.folders[i];
			bool taken = f.name.empty();
			for (size_t j = 0; j < i && !taken; j++) taken = S.folders[j].name == f.name;
			if (taken) f.name = "Folder " + std::to_string(i + 1);
		}
		S.custom = !S.folders.empty();
		return S.custom;
	}

	inline void RemoveFile() { if (!S.path.empty()) std::remove(S.path.c_str()); }

	inline void RemoveEntry(const std::string& entry) {
		for (Folder& f : S.folders)
			for (size_t i = 0; i < f.entries.size(); i++)
				if (f.entries[i] == entry) { f.entries.erase(f.entries.begin() + i); return; }
	}

	// Moves an entry to the end of a folder (which must exist).
	inline bool MoveEntry(const std::string& entry, const std::string& folder) {
		if (!Find(folder)) return false;
		RemoveEntry(entry);
		Find(folder)->entries.push_back(entry);
		return true;
	}

	// One place up (-1) or down (+1) inside its folder.
	inline bool ShiftEntry(const std::string& entry, int direction) {
		Folder* f = FolderOf(entry);
		if (!f) return false;
		for (size_t i = 0; i < f->entries.size(); i++) {
			if (f->entries[i] != entry) continue;
			const size_t to = i + direction;	// (wraps below 0: caught by the size test)
			if (to >= f->entries.size()) return false;
			std::swap(f->entries[i], f->entries[to]);
			return true;
		}
		return false;
	}

	inline bool ShiftFolder(const std::string& name, int direction) {
		for (size_t i = 0; i < S.folders.size(); i++) {
			if (S.folders[i].name != name) continue;
			const size_t to = i + direction;
			if (to >= S.folders.size()) return false;
			std::swap(S.folders[i], S.folders[to]);
			return true;
		}
		return false;
	}

	// Drag and drop (2026-11-10): an entry dropped on another entry takes its place in that entry's folder. Within one folder a drag
	// downwards lands after the target and a drag upwards before it, as reordering a list by hand does; from another folder: before it.
	inline bool DropEntryOn(const std::string entry, const std::string target) {
		Folder* from = FolderOf(entry);
		Folder* to = FolderOf(target);
		if (entry == target || !from || !to) return false;
		auto index = [](const Folder& f, const std::string& e) { size_t i = 0; while (i < f.entries.size() && f.entries[i] != e) i++; return i; };
		const size_t was = index(*from, entry);
		const bool after = from == to && was < index(*to, target);
		from->entries.erase(from->entries.begin() + was);
		to->entries.insert(to->entries.begin() + index(*to, target) + (after ? 1 : 0), entry);
		return true;
	}

	// The same for a folder dropped on another folder: it takes that folder's place in the list.
	inline bool DropFolderOn(const std::string name, const std::string target) {
		auto index = [](const std::string& n) { size_t i = 0; while (i < S.folders.size() && S.folders[i].name != n) i++; return i; };
		const size_t was = index(name);
		if (name == target || was >= S.folders.size() || index(target) >= S.folders.size()) return false;
		const bool after = was < index(target);
		const Folder moved = S.folders[was];
		S.folders.erase(S.folders.begin() + was);
		S.folders.insert(S.folders.begin() + index(target) + (after ? 1 : 0), moved);
		return true;
	}

	// A new empty folder at the end. False: no name, or the name is taken.
	inline bool AddFolder(const std::string& typed) {
		const std::string name = Clean(typed);
		if (name.empty() || Find(name)) return false;
		S.folders.push_back({ name, {} });
		return true;
	}

	inline bool RenameFolder(const std::string& from, const std::string& typed) {
		const std::string name = Clean(typed);
		Folder* f = Find(from);
		if (!f || name.empty() || Find(name)) return false;
		f->name = name;
		return true;
	}
}
