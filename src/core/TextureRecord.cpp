// Default sampler states of a texture record, indexed by D3DSAMPLERSTATETYPE (entry 0 is unused padding so the index
// equals the D3D enum value).
static const DWORD kDefaultSamplerStates[SamplerStatesMax] = {
	0,					// (unused)
	D3DTADDRESS_WRAP,	// ADDRESSU
	D3DTADDRESS_WRAP,	// ADDRESSV
	D3DTADDRESS_WRAP,	// ADDRESSW
	0,					// BORDERCOLOR
	D3DTEXF_POINT,		// MAGFILTER
	D3DTEXF_POINT,		// MINFILTER
	D3DTEXF_NONE,		// MIPFILTER
	0,					// MIPMAPLODBIAS
	0,					// MAXMIPLEVEL
	1,					// MAXANISOTROPY
	0,					// SRGBTEXTURE
};

TextureRecord::TextureRecord() {
	Texture = nullptr;
	for (int s = 0; s < SamplerStatesMax; ++s) SamplerStates[s] = kDefaultSamplerStates[s];
}

// Maps the sampler variable type reported by D3DX to the kind of texture to load for it.
TextureRecord::TextureRecordType TextureRecord::GetTextureType(UINT paramType) {
	switch (paramType) {
	case D3DXPT_SAMPLER:
	case D3DXPT_SAMPLER1D:
	case D3DXPT_SAMPLER2D:
		return TextureRecordType::PlanarBuffer;
	case D3DXPT_SAMPLER3D:
		return TextureRecordType::VolumeBuffer;
	case D3DXPT_SAMPLERCUBE:
		return TextureRecordType::CubeBuffer;
	default:
		return TextureRecordType::None;
	}
}

namespace {
	struct WordValue { const char* word; int value; };

	// Returns the value paired with the word, or 0 when it is not in the table.
	template <size_t N> int LookUpWord(const WordValue (&table)[N], const std::string& word) {
		for (const WordValue& entry : table)
			if (word == entry.word) return entry.value;
		return 0;
	}

	const WordValue kStateNames[] = {
		{ "ADDRESSU", D3DSAMP_ADDRESSU }, { "ADDRESSV", D3DSAMP_ADDRESSV }, { "ADDRESSW", D3DSAMP_ADDRESSW },
		{ "BORDERCOLOR", D3DSAMP_BORDERCOLOR }, { "MAGFILTER", D3DSAMP_MAGFILTER }, { "MINFILTER", D3DSAMP_MINFILTER },
		{ "MIPFILTER", D3DSAMP_MIPFILTER }, { "MIPMAPLODBIAS", D3DSAMP_MIPMAPLODBIAS }, { "MAXMIPLEVEL", D3DSAMP_MAXMIPLEVEL },
		{ "MAXANISOTROPY", D3DSAMP_MAXANISOTROPY }, { "SRGBTEXTURE", D3DSAMP_SRGBTEXTURE },
	};
	const WordValue kAddressModes[] = {
		{ "WRAP", D3DTADDRESS_WRAP }, { "MIRROR", D3DTADDRESS_MIRROR }, { "CLAMP", D3DTADDRESS_CLAMP },
		{ "BORDER", D3DTADDRESS_BORDER }, { "MIRRORONCE", D3DTADDRESS_MIRRORONCE },
	};
	const WordValue kFilterModes[] = {
		{ "NONE", D3DTEXF_NONE }, { "POINT", D3DTEXF_POINT }, { "LINEAR", D3DTEXF_LINEAR },
		{ "ANISOTROPIC", D3DTEXF_ANISOTROPIC }, { "PYRAMIDALQUAD", D3DTEXF_PYRAMIDALQUAD },
		{ "GAUSSIANQUAD", D3DTEXF_GAUSSIANQUAD }, { "CONVOLUTIONMONO", D3DTEXF_CONVOLUTIONMONO },
	};
	const WordValue kSrgbModes[] = { { "FALSE", 0 }, { "TRUE", 1 } };

	std::string UpperCase(std::string text) {
		std::transform(text.begin(), text.end(), text.begin(), ::toupper);
		return text;
	}
}

// Reads "NAME = VALUE;" pairs (the body of a sampler_state block) into SamplerStates. Unknown names are ignored, unknown
// words for a known state store 0.
void TextureRecord::GetSamplerStates(std::string stateBlock) {
	std::stringstream block(stateBlock);
	std::string entry;
	while (std::getline(block, entry, ';')) {
		const size_t lineBreak = entry.find("\n");
		if (lineBreak != std::string::npos) entry.erase(lineBreak, 1);

		// UNOFFICIAL 2026-10-19: the name runs up to the '=' and is trimmed (the old '- 1' dropped the name's last letter when
		// written without a space, 'ADDRESSU=CLAMP' in GunFX's BarrelHaze, so those states were silently ignored)
		const size_t equals = entry.find("=");
		const std::string name = UpperCase(trim(entry.substr(0, equals)));
		const std::string word = UpperCase(trim(entry.substr(equals + 1, entry.length())));

		const int state = LookUpWord(kStateNames, name);
		DWORD& slot = SamplerStates[state];
		switch (state) {
		case D3DSAMP_ADDRESSU:
		case D3DSAMP_ADDRESSV:
		case D3DSAMP_ADDRESSW:
			slot = LookUpWord(kAddressModes, word);
			break;
		case D3DSAMP_MAGFILTER:
		case D3DSAMP_MINFILTER:
		case D3DSAMP_MIPFILTER:
			slot = LookUpWord(kFilterModes, word);
			break;
		case D3DSAMP_SRGBTEXTURE:
			slot = LookUpWord(kSrgbModes, word);
			break;
		// (2026-11-17: strtof / strtol, were stof / stoi, which throw on text that is not a number: nothing caught it)
		case D3DSAMP_BORDERCOLOR: {
			const float color = strtof(word.c_str(), nullptr);
			slot = *reinterpret_cast<const DWORD*>(&color);
			break;
		}
		case D3DSAMP_MAXANISOTROPY:
			slot = (DWORD)std::clamp(strtol(word.c_str(), nullptr, 10), 1L, 16L);
			break;
		default:
			break;	// other states keep their default
		}
	}
}

// Points the record at a texture the TextureManager knows by name (render targets, game buffers).
bool TextureRecord::BindTexture(const char* samplerName) {
	std::string slotName(samplerName);
	TextureRef = TheTextureManager->GetTextureSlotByName(slotName);
	Texture = (TextureRef != nullptr) ? *TextureRef : nullptr;
	return Texture != nullptr;
}
