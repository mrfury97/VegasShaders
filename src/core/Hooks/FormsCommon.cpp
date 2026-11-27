#pragma once

bool (__cdecl* LoadForm)(TESForm*, UInt32) = (bool (__cdecl*)(TESForm*, UInt32))Hooks::LoadForm;
bool __cdecl LoadFormHook(TESForm* Form, UInt32 ModEntry) {
	
	bool r = LoadForm(Form, ModEntry);
	switch (Form->formType) {
		case TESForm::FormType::kFormType_Weather: {
				TESWeatherEx* Weather = (TESWeatherEx*)Form;
				memcpy(Weather->colorsb, Weather->colors, sizeof(Weather->colors));
				if (TheSettingManager->SettingsMain.WeatherMode.Enabled) {
					TheSettingManager->SetSettingsWeather(Weather);
				}
			}
			break;
		default:
			break;
	}
	return r;

}