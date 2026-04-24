# Contributing to Selective Color Filter GPU

Thank you for your interest in contributing! This document provides guidelines for contributing to the project.

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Create a new branch for your feature/fix
4. Make your changes
5. Test thoroughly
6. Submit a pull request

## Development Setup

### Prerequisites
- Visual Studio 2019 or later
- Windows SDK 10.0.19041.0 or later
- DirectX 11 SDK (included with Windows SDK)

### Building
1. Open `ColorFilterGPU.sln` in Visual Studio
2. Select Release x64 configuration
3. Build > Build Solution

## Code Style

- Use consistent indentation (4 spaces)
- Follow existing naming conventions
- Add comments for complex logic
- Keep functions focused and reasonably sized
- Use meaningful variable names

## Testing

Before submitting:
- Test on multiple monitor setups if possible
- Verify all color picker functionality
- Test settings save/load
- Check performance with different grayscale modes
- Ensure V-Sync toggle works correctly

## Pull Request Guidelines

- Provide clear description of changes
- Include screenshots for UI changes
- Reference any related issues
- Ensure code compiles without warnings
- Test on Windows 10 and 11 if possible

## Reporting Issues

When reporting bugs:
- Include Windows version
- Describe graphics card/driver
- Provide steps to reproduce
- Include any error messages
- Mention monitor setup (single/multi)

## Feature Requests

- Check existing issues first
- Provide clear use case
- Explain expected behavior
- Consider performance implications

## Areas for Contribution

- Additional grayscale algorithms
- Performance optimizations
- UI improvements
- Multi-language support
- Additional color picker modes
- Better error handling
- Documentation improvements

## License

By contributing, you agree that your contributions will be licensed under the MIT License.