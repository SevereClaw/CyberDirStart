// CyberDirStat - short cyberpunk interface sounds
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace UiSounds
{
    enum class Sound { ScanDone, Delete, Purge };

    // Plays asynchronously if "Interface sounds" is enabled in the settings (off by default)
    void Play(Sound sound);
}
