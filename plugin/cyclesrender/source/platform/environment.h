// SPDX-License-Identifier: Apache-2.0
// Environment variables of this process.
//
// Like the other platform headers, this one uses only plain C++ types: the implementation
// includes system headers that clash with Cinema 4D's.

#ifndef CYCLESRENDER_PLATFORM_ENVIRONMENT_H__
#define CYCLESRENDER_PLATFORM_ENVIRONMENT_H__

namespace cyclesrender
{
namespace platform
{

/// Copies the value of the environment variable @p name into @p buffer as UTF-8, always
/// terminated. @p name is plain ASCII. Returns false, with an empty buffer, when the variable
/// is not set, is empty, or does not fit into @p bufferSize bytes.
bool GetEnvironmentText(const char* name, char* buffer, int bufferSize);

} // namespace platform
} // namespace cyclesrender

#endif // CYCLESRENDER_PLATFORM_ENVIRONMENT_H__
