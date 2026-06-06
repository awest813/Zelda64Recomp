// Dreamcast-specific configuration
//
// On Dreamcast, config is stored on VMU as a simple binary blob rather
// than JSON files. This file provides the Dreamcast implementation of
// zelda64::get_app_folder_path() and related config persistence that
// is not already handled in dc_support.cpp.
//
// The main config.cpp in src/game/ handles the actual config logic;
// this file only supplements it with DC-specific path resolution.

#ifdef DREAMCAST

// This file is intentionally minimal. The Dreamcast support functions
// in dc_support.cpp already implement get_app_folder_path() and the
// filesystem path resolution. Config serialization (JSON) works on
// Dreamcast as long as the paths point to the VMU filesystem.
//
// If JSON config proves too large for VMU (each block is 512 bytes,
// typical VMU has ~128 KB usable), a binary config format should
// replace the JSON serialization in config.cpp. This can be done by
// adding #ifdef DREAMCAST guards around the JSON read/write calls
// and substituting a compact binary format.
//
// For now, config.cpp should work as-is with the VMU paths from
// dc_support.cpp, provided the config files are small enough to fit
// in VMU storage.

#endif // DREAMCAST
