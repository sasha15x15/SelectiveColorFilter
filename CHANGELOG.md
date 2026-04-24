# Changelog

All notable changes to Selective Color Filter GPU will be documented in this file.

## [1.0.0] - 2024-12-XX

### Added
- Initial release with GPU-accelerated color filtering
- Multiple color selection (up to 10 colors)
- Multi-monitor support with independent FPS display
- 6 grayscale conversion modes (Average, Luminosity, HD, Desaturation, Max Channel, Green Only)
- Real-time threshold (1-100%) and smoothing (0-200%) adjustment
- V-Sync control for optimal performance
- Auto-save settings to Windows registry
- Export/import settings profiles (.cfg files)
- Built-in color picker and eyedropper tools
- High refresh rate monitor support (144Hz+)
- DirectX 11 pixel shader implementation
- Desktop Duplication API integration
- Multi-threaded rendering architecture
- Automatic window exclusion from capture
- ESC key to stop filtering
- Responsive UI during active filtering

### Technical Features
- GPU-only processing for maximum performance
- Fullscreen overlay rendering
- Registry-based settings persistence
- File-based configuration profiles
- Real-time shader constant updates
- Per-monitor FPS monitoring
- Optimized for gaming and professional use

### System Requirements
- Windows 10/11
- DirectX 11 compatible graphics card
- Visual C++ Redistributable 2019+