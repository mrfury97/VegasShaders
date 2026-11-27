#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// UNOFFICIAL material rules (docs/MATERIAL-RULES-PLAN.md): per-texture settings for the object shaders' PBR guess, picked
// from the diffuse texture's path by a small text table the user can edit (VegasShaders.materials.txt). Kept free
// of NVR's globals so the offline check (tests\material_rules.cpp) runs exactly this code over every texture in the game.
//
// Table lines (# starts a comment):
//   class   <name> metal=<off|guess|mask|full> strength=<0-1> rough=<min>-<max> reflections=<x> tint=<r>,<g>,<b> authored=<keep|replace>
//                                        authored (2026-11-05 VanillaBridge): keep (default) = where the game's own highlight data is
//                                        (normal map alpha + the mesh's glossiness) the highlight follows it and rough= only fills the
//                                        gap; replace = rough= decides as before
//   folder  <prefix> <class> [final]     longest prefix wins; "final" folders are decided before any keyword
//   keyword <word> <class> [not=<a>,<b>] the file name contains <word> and none of the not= words
//   file    <path> <class>               one texture, before everything else
//   strip   <prefix>                     a folder level to look through: "strip textures\nvdlc01" makes
//                                        textures\nvdlc01\clutter\x.dds match as textures\clutter\x.dds
// Order: file, final folder, keyword, folder, else no rule (today's look exactly). The class "default" is no rule too.
namespace MaterialRules {

	enum Metal { MetalGuess = 0, MetalMask = 1, MetalFull = 2, MetalOff = 3 };
	enum SssProfile {
		SssNone = 0, SssLeaf = 1, SssCloth = 2, SssWax = 3,
		SssMarble = 4, SssSnow = 5, SssFlesh = 6, SssPlastic = 7
	};

	inline int SssModelId(int sss) {
		switch (sss) {
			case SssLeaf:    return 5;
			case SssCloth:   return 3;
			case SssWax:
			case SssPlastic: return 11;
			case SssMarble:  return 12;
			case SssSnow:    return 13;
			case SssFlesh:   return 14;
			default:         return 0;
		}
	}

	inline int InferSssFromClass(const std::string& name) {
		if (name.find("plant") != std::string::npos || name.find("leaf") != std::string::npos || name.find("foliage") != std::string::npos) return SssLeaf;
		if (name.find("cloth") != std::string::npos || name == "soft") return SssCloth;
		if (name.find("wax") != std::string::npos || name.find("candle") != std::string::npos) return SssWax;
		if (name.find("plastic") != std::string::npos) return SssPlastic;
		if (name.find("marble") != std::string::npos || name.find("jade") != std::string::npos) return SssMarble;
		if (name.find("snow") != std::string::npos || name.find("ice") != std::string::npos) return SssSnow;
		if (name.find("food") != std::string::npos || name.find("flesh") != std::string::npos || name.find("meat") != std::string::npos || name.find("creature") != std::string::npos) return SssFlesh;
		return SssNone;
	}

	struct Rule {
		std::string	Name;
		int			Metal = MetalGuess;
		float		Strength = 1.0f;		// the metal's strength (replaces [Shaders.PBR.Metal] World/FirstPerson for this texture)
		float		RoughMin = -1.0f;		// < 0: the global roughness (getRoughness)
		float		RoughMax = -1.0f;
		float		Reflections = 1.0f;		// metal's surroundings reflection, times [Shaders.PBR.Metal] Reflections
		float		Tint[3] = { 0.5f, 0.5f, 0.5f };	// DebugView colour
		bool		ReplaceAuthored = false;	// UNOFFICIAL 2026-11-05 authored=replace: the rule's roughness over the vanilla highlight data (Object.hlsl BridgeSetup)
		int			Sss = SssNone;			// UNOFFICIAL 2026-11-24 MaterialSSS Phase 1 profile (sss=<none|leaf|cloth|wax|marble|snow|flesh|plastic>)
	};

	// "Data\Textures\Weapons\Foo.DDS" -> "textures\weapons\foo.dds"
	inline std::string Normalize(const char* path) {
		std::string p(path ? path : "");
		for (char& c : p) c = (c == '/') ? '\\' : (char)tolower((unsigned char)c);
		const size_t data = p.find("data\\");
		if (data != std::string::npos && (data == 0 || p[data - 1] == '\\' || p[data - 1] == ':')) p.erase(0, data + 5);
		if (p.compare(0, 9, "textures\\") != 0) {
			const size_t t = p.find("\\textures\\");
			if (t != std::string::npos) p.erase(0, t + 1);
			else p.insert(0, "textures\\");
		}
		return p;
	}

	class Table {
	public:
		// Rule 0 is "no rule": Match returns 0 for anything the table does not cover.
		Table() { Rules.push_back(Rule()); Rules[0].Name = "default"; }

		// Returns false and fills Errors (one line each) for lines it could not read; good lines are kept.
		bool Load(const std::string& text, std::string& errors) {
			*this = Table();
			bool ok = true;
			size_t start = 0;
			int lineNumber = 0;
			while (start <= text.size()) {
				size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();
				std::string line = text.substr(start, end - start);
				start = end + 1;
				++lineNumber;
				const size_t hash = line.find('#');
				if (hash != std::string::npos) line.erase(hash);
				std::vector<std::string> words = Split(line);
				if (words.empty()) continue;
				std::string error;
				if (!ReadLine(words, error)) {
					ok = false;
					char head[32];
					snprintf(head, sizeof(head), "line %d: ", lineNumber);
					errors += head + error + "\n";
				}
				if (end == text.size()) break;
			}
			std::stable_sort(Folders.begin(), Folders.end(), [](const Prefix& a, const Prefix& b) { return a.Path.size() > b.Path.size(); });
			return ok;
		}

		// The rule for a normalised path (Normalize).
		int Match(const std::string& path) const {
			auto exact = Files.find(path);
			if (exact != Files.end()) return exact->second;
			for (const std::string& s : Strips)
				if (path.size() > s.size() && path.compare(0, s.size(), s) == 0) return MatchStripped("textures\\" + path.substr(s.size()));
			return MatchStripped(path);
		}

		const Rule& Get(int index) const { return Rules[index >= 0 && index < (int)Rules.size() ? index : 0]; }
		int Count() const { return (int)Rules.size(); }
		bool Empty() const { return Files.empty() && Folders.empty() && Keywords.empty(); }

	private:
		struct Prefix { std::string Path; int RuleIndex; bool Final; };
		struct Word { std::string Text; std::vector<std::string> Not; int RuleIndex; };

		std::vector<Rule>						Rules;
		std::unordered_map<std::string, int>	Files;
		std::vector<Prefix>						Folders;
		std::vector<Word>						Keywords;
		std::vector<std::string>				Strips;

		int MatchStripped(const std::string& path) const {
			for (const Prefix& f : Folders)
				if (f.Final && path.compare(0, f.Path.size(), f.Path) == 0) return f.RuleIndex;
			const size_t slash = path.rfind('\\');
			const char* name = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
			for (const Word& k : Keywords) {
				if (!strstr(name, k.Text.c_str())) continue;
				bool excluded = false;
				for (const std::string& n : k.Not) if (strstr(name, n.c_str())) { excluded = true; break; }
				if (!excluded) return k.RuleIndex;
			}
			for (const Prefix& f : Folders)
				if (!f.Final && path.compare(0, f.Path.size(), f.Path) == 0) return f.RuleIndex;
			return 0;
		}

		static std::vector<std::string> Split(const std::string& line) {
			std::vector<std::string> words;
			size_t i = 0;
			while (i < line.size()) {
				while (i < line.size() && isspace((unsigned char)line[i])) ++i;
				size_t j = i;
				while (j < line.size() && !isspace((unsigned char)line[j])) ++j;
				if (j > i) words.push_back(line.substr(i, j - i));
				i = j;
			}
			return words;
		}

		static std::string Lower(std::string s) {
			for (char& c : s) c = (char)tolower((unsigned char)c);
			return s;
		}

		int Find(const std::string& name) const {
			for (int i = 0; i < (int)Rules.size(); ++i) if (Rules[i].Name == name) return i;
			return -1;
		}

		static bool Number(const std::string& s, float& out) {
			char* end = nullptr;
			out = (float)strtod(s.c_str(), &end);
			return end && end != s.c_str() && *end == 0;
		}

		bool ReadLine(const std::vector<std::string>& words, std::string& error) {
			const std::string kind = Lower(words[0]);
			if (kind == "class") {
				if (words.size() < 2) { error = "class without a name"; return false; }
				Rule rule;
				rule.Name = Lower(words[1]);
				if (rule.Name == "default") { error = "\"default\" is the built-in no-rule class"; return false; }
				bool explicitSss = false;
				for (size_t i = 2; i < words.size(); ++i) {
					const std::string w = Lower(words[i]);
					const size_t eq = w.find('=');
					if (eq == std::string::npos) { error = "expected key=value, got " + w; return false; }
					const std::string key = w.substr(0, eq), value = w.substr(eq + 1);
					if (key == "metal") {
						if (value == "off") rule.Metal = MetalOff;
						else if (value == "guess") rule.Metal = MetalGuess;
						else if (value == "mask") rule.Metal = MetalMask;
						else if (value == "full") rule.Metal = MetalFull;
						else { error = "metal must be off, guess, mask or full"; return false; }
					}
					else if (key == "sss") {
						explicitSss = true;
						if (value == "none" || value == "off") rule.Sss = SssNone;
						else if (value == "leaf" || value == "plant" || value == "foliage") rule.Sss = SssLeaf;
						else if (value == "cloth") rule.Sss = SssCloth;
						else if (value == "wax") rule.Sss = SssWax;
						else if (value == "marble") rule.Sss = SssMarble;
						else if (value == "snow" || value == "ice") rule.Sss = SssSnow;
						else if (value == "flesh" || value == "food") rule.Sss = SssFlesh;
						else if (value == "plastic") rule.Sss = SssPlastic;
						else { error = "sss must be none, leaf, cloth, wax, marble, snow, flesh or plastic"; return false; }
					}
					else if (key == "strength") { if (!Number(value, rule.Strength)) { error = "bad strength"; return false; } }
					else if (key == "reflections") { if (!Number(value, rule.Reflections)) { error = "bad reflections"; return false; } }
					else if (key == "rough") {
						const size_t dash = value.find('-', 1);
						if (dash == std::string::npos || !Number(value.substr(0, dash), rule.RoughMin) || !Number(value.substr(dash + 1), rule.RoughMax)) {
							error = "rough must be <min>-<max>"; return false;
						}
					}
					else if (key == "authored") {
						if (value == "keep") rule.ReplaceAuthored = false;
						else if (value == "replace") rule.ReplaceAuthored = true;
						else { error = "authored must be keep or replace"; return false; }
					}
					else if (key == "tint") {
						float t[3];
						size_t a = value.find(','), b = a == std::string::npos ? a : value.find(',', a + 1);
						if (b == std::string::npos || !Number(value.substr(0, a), t[0]) || !Number(value.substr(a + 1, b - a - 1), t[1]) || !Number(value.substr(b + 1), t[2])) {
							error = "tint must be <r>,<g>,<b>"; return false;
						}
						for (int c = 0; c < 3; ++c) rule.Tint[c] = t[c];
					}
					else { error = "unknown key " + key; return false; }
				}
				rule.Strength = std::clamp(rule.Strength, 0.0f, 1.0f);
				rule.Reflections = std::clamp(rule.Reflections, 0.0f, 4.0f);
				if (rule.RoughMin >= 0.0f) {
					rule.RoughMin = std::clamp(rule.RoughMin, 0.02f, 1.0f);
					rule.RoughMax = std::clamp(rule.RoughMax, rule.RoughMin, 1.0f);
				}
				if (!explicitSss) rule.Sss = InferSssFromClass(rule.Name);
				const int existing = Find(rule.Name);
				if (existing >= 0) Rules[existing] = rule;
				else Rules.push_back(rule);
				return true;
			}
			if (kind == "strip") {
				if (words.size() != 2) { error = "strip takes one folder"; return false; }
				std::string p = Normalize(words[1].c_str());
				if (p.back() != '\\') p += '\\';
				Strips.push_back(p);
				return true;
			}
			if (kind == "folder" || kind == "keyword" || kind == "file") {
				if (words.size() < 3) { error = kind + " needs a path or word and a class"; return false; }
				const std::string className = Lower(words[2]);
				const int rule = className == "default" ? 0 : Find(className);
				if (rule < 0) { error = "unknown class " + className + " (define it with a class line first)"; return false; }
				if (kind == "file") {
					if (words.size() > 3) { error = "file takes no options"; return false; }
					Files[Normalize(words[1].c_str())] = rule;
					return true;
				}
				if (kind == "folder") {
					std::string p = Normalize(words[1].c_str());
					if (p.empty() || p.back() != '\\') p += '\\';
					bool final = false;
					for (size_t i = 3; i < words.size(); ++i) {
						if (Lower(words[i]) == "final") final = true;
						else { error = "unknown folder option " + words[i]; return false; }
					}
					Folders.push_back({ p, rule, final });
					return true;
				}
				Word k;
				k.Text = Lower(words[1]);
				k.RuleIndex = rule;
				for (size_t i = 3; i < words.size(); ++i) {
					const std::string w = Lower(words[i]);
					if (w.compare(0, 4, "not=") != 0) { error = "unknown keyword option " + w; return false; }
					std::string list = w.substr(4);
					size_t s = 0;
					while (s <= list.size()) {
						size_t e = list.find(',', s);
						if (e == std::string::npos) e = list.size();
						if (e > s) k.Not.push_back(list.substr(s, e - s));
						s = e + 1;
					}
				}
				Keywords.push_back(k);
				return true;
			}
			error = "unknown line type " + words[0];
			return false;
		}
	};
}
