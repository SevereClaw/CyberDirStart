// CyberDirStat - short cyberpunk interface sounds
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "UiSounds.h"

#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

// The sounds are synthesized by res/sounds/make_sounds.py and embedded as WAVE resources
void UiSounds::Play(const Sound sound)
{
    if (!COptions::UiSounds) return;

    int id = IDR_SOUND_SCAN;
    if (sound == Sound::Delete) id = IDR_SOUND_DELETE;
    else if (sound == Sound::Purge) id = IDR_SOUND_PURGE;

    PlaySoundW(MAKEINTRESOURCEW(id), GetModuleHandleW(nullptr), SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}
